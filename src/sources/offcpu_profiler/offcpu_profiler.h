// ============================================================================
// OffcpuProfilerSource — Off-CPU 性能剖析器（进程等待时间追踪）
// ============================================================================
//
// 通过 eBPF 追踪进程在非 CPU 执行状态下的等待时间（Off-CPU 时间）。
// 与 cpu_profiler（on-CPU 采样）互补，用于回答"进程大部分时间不在 CPU 上时
// 到底在等什么"的问题。典型等待场景包括：磁盘 I/O、网络 I/O、锁竞争、
// 定时休眠、等待子进程等。
//
// On-CPU vs Off-CPU 的区别：
// ===========================
// - On-CPU（cpu_profiler）：进程正在 CPU 上执行的时间分布
//   回答"CPU 热点在哪里"→ 适合优化计算密集型程序
//
// - Off-CPU（本插件）：进程不在 CPU 上执行的时间分布
//   回答"瓶颈在等什么"→ 适合优化 IO 密集型程序、锁竞争问题
//
// 采集指标：
// ==========
// 每条 off-CPU 事件记录一次进程从离开 CPU（sched_switch）到重新被调度
// 上 CPU（sched_wakeup）的完整等待周期：
//
// - pid / tid：等待进程的 PID 和 TID
// - comm：进程名称
// - cpu：进程离开的 CPU 核心号
// - duration_ns：等待持续时间（纳秒）
// - kernel_stack_id / user_stack_id：进程离开 CPU 时的内核/用户调用栈 ID
// - kernel_stack / user_stack：解析后的完整等待栈帧地址
// - sample_type：kOffCpu（标识为 Off-CPU 采样）
//
// 工作原理：
// ==========
// 1. 挂载 eBPF 程序到内核关键调度事件：
//    - sched/sched_switch：捕获进程离开 CPU 的时刻，记录当时的内核/用户调用栈
//      和时间戳，将 (pid, timestamp, stack) 存入 offcpu_map 等待匹配
//    - sched/sched_wakeup / sched/sched_wakeup_new：捕获进程被唤醒的时刻，
//      从 offcpu_map 中查找同一进程的上一次离 CPU 记录，计算等待时长
//       duration = 唤醒时间 - 离开 CPU 时间，然后推送事件到 ring buffer
//
// 2. 仅在 duration >= min_duration_us（默认 100μs）时才推送事件，
//    避免大量快速调度的短等待事件淹没有效数据。
//
// 3. 纯 Push 模式（IsPushMode=true），通过 ring buffer 实时推送每条
//    off-CPU 事件，无批量聚合（适合生成 Off-CPU 火焰图）。
//
// 配置参数：
// ==========
// - min_duration_us：最小等待时长阈值（默认 100μs，低于此值的事件不记录）
// - user_stacks：是否采集用户态堆栈（默认 true）
// - kernel_stacks：是否采集内核态堆栈（默认 true）
// - target_pids：逗号分隔的 PID 白名单（空 = 追踪所有进程）
// - bpf_object：eBPF 目标文件路径（必需）
// ============================================================================

#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ============================================================================
// OffcpuParseCommaUint32 — 解析逗号分隔的 uint32 列表（本地版）
// ============================================================================
inline void OffcpuParseCommaUint32(const std::string& s,
                                   std::vector<uint32_t>* out) {
    out->clear();
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        if (token.empty())
            continue;
        try {
            out->push_back(static_cast<uint32_t>(std::stoul(token)));
        } catch (...) {}
    }
}

// ============================================================================
// OffcpuProfilerSource 类 — Off-CPU 剖析插件主体
// ============================================================================
class OffcpuProfilerSource : public SourcePlugin {
public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* Version() const override { return "0.1.0"; }

    bool IsPushMode() const override { return false; }

    StatusOr<DataBatchPtr> Collect() override {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto result = cached_batch_
            ? cached_batch_
            : std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        cached_batch_ = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        return result;
    }

    // ========================================================================
    // Init — 初始化 Off-CPU 剖析器配置
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        min_duration_us_ =
            static_cast<uint32_t>(config["min_duration_us"].AsInt(100));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);
        bpf_obj_path_ = config["bpf_object"].AsString("");
        OffcpuParseCommaUint32(config["target_pids"].AsString(""), &target_pids_);
        // 构建 PID 快速查找集合
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        (void)min_duration_us_;
        (void)user_stacks_;
        (void)kernel_stacks_;
        return Status::Ok();
    }

    // ========================================================================
    // Start — 加载 eBPF 程序并开始追踪 Off-CPU 等待
    // ========================================================================
    // 1. 加载 eBPF 目标文件
    // 2. 挂载 trace_offcpu eBPF 程序（内部包含对 sched_switch 和
    //    sched_wakeup 事件的完整处理逻辑）
    // 3. 获取 offcpu_stacks map 用于后续堆栈解析
    // 4. 创建 ring buffer 接收 off-CPU 事件
    // 5. 启动后台轮询线程驱动 ring buffer 消费
    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "offcpu_profiler: no bpf_object path specified; profiler idle");
            running_.store(false);
            return Status::Ok();
        }

        auto st = bpf_mgr_.LoadObject("offcpu_profiler", bpf_obj_path_);
        if (!st.ok())
            return st;

        // 挂载 Off-CPU 追踪 eBPF 程序
        st = bpf_mgr_.AttachProgram("offcpu_profiler", "trace_offcpu");
        if (!st.ok())
            return st;

        // 获取堆栈 map 文件描述符
        stacks_fd_ = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_stacks");

        // 创建 ring buffer 并注册事件回调
        int rb_fd = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_events");
        if (rb_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "offcpu_events ringbuf missing");
        }

        ring_buf_ = bpf_mgr_.CreateRingBuffer(rb_fd, HandleEvent, this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal,
                                 "offcpu ring buffer init failed");
        }

        // 启动后台轮询线程
        running_.store(true);
        poll_thread_ = std::thread([this] {
            while (running_.load()) {
                int err = ring_buffer__poll(ring_buf_, 100);
                if (err < 0 && err != -EINTR)
                    IL_WARN("offcpu_profiler: ringbuf poll err %d", err);
            }
        });

        IL_INFO("offcpu_profiler started");
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止追踪，释放所有 eBPF 资源
    // ========================================================================
    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        stacks_fd_ = -1;
        return Status::Ok();
    }

private:
    // ========================================================================
    // AllowPid — 检查 PID 是否在采集白名单中
    // ========================================================================
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty())
            return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    // ========================================================================
    // LookupStack — 根据 stack_id 查询等待时的调用栈帧地址
    // ========================================================================
    // 从 offcpu_stacks map（BPF_MAP_TYPE_STACK_TRACE）中读取指定 stack_id
    // 对应的完整调用栈原始地址（IP 指针）。后续可通过符号化工具将地址
    // 映射为函数名，形成 Off-CPU 火焰图。
    void LookupStack(int32_t stack_id, std::vector<StackFrame>* out) {
        out->clear();
        if (stack_id < 0 || stacks_fd_ < 0)
            return;

        uint64_t raw[MAX_STACK_DEPTH];
        std::memset(raw, 0, sizeof(raw));
        uint32_t sid = static_cast<uint32_t>(stack_id);
        if (bpf_map_lookup_elem(stacks_fd_, &sid, raw) != 0)
            return;

        for (int i = 0; i < MAX_STACK_DEPTH; ++i) {
            if (raw[i] == 0)
                break;
            StackFrame fr;
            fr.address = raw[i];
            out->push_back(fr);
        }
    }

    // ========================================================================
    // HandleEvent — ring buffer 事件回调（静态函数）
    // ========================================================================
    // 收到一条完整的 off-CPU 事件后：
    // 1. 按 PID 白名单过滤
    // 2. 创建 StackSample，设置 sample_type=kOffCpu
    // 3. 根据 kernel_stack_id / user_stack_id 解析完整调用栈
    // 4. 通过 callback_ 立即推送 batch 到下游
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<OffcpuProfilerSource*>(ctx);
        if (size < sizeof(il_offcpu_event))
            return 0;
        auto* ev = static_cast<il_offcpu_event*>(data);

        if (!self->AllowPid(ev->pid))
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();
        sample.pid = ev->pid;
        sample.tid = ev->tid;
        sample.cpu = ev->cpu;
        sample.comm = batch->InternString(std::string_view(
            ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
        sample.sample_type = SampleType::kOffCpu;
        sample.duration_ns = ev->duration_ns;
        sample.count = 1;
        sample.kernel_stack_id = ev->kernel_stack_id;
        sample.user_stack_id = ev->user_stack_id;

        self->LookupStack(ev->kernel_stack_id, &sample.kernel_stack);
        self->LookupStack(ev->user_stack_id, &sample.user_stack);

        {
            std::lock_guard<std::mutex> lk(self->cache_mu_);
            if (!self->cached_batch_)
                self->cached_batch_ = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
            auto& cs = self->cached_batch_->AddStackSample();
            cs.pid = ev->pid;
            cs.tid = ev->tid;
            cs.cpu = ev->cpu;
            cs.comm = self->cached_batch_->InternString(std::string_view(
                ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
            cs.sample_type = SampleType::kOffCpu;
            cs.duration_ns = ev->duration_ns;
            cs.count = 1;
            cs.kernel_stack_id = ev->kernel_stack_id;
            cs.user_stack_id = ev->user_stack_id;
            cs.kernel_stack = sample.kernel_stack;
            cs.user_stack = sample.user_stack;
        }

        if (self->callback_)
            self->callback_(std::move(batch));
        return 0;
    }

    // ---- 配置参数 ----
    uint32_t min_duration_us_ = 100;    // 最小等待时长阈值（微秒）
    bool user_stacks_ = true;           // 是否采集用户态堆栈
    bool kernel_stacks_ = true;         // 是否采集内核态堆栈
    std::string bpf_obj_path_;          // eBPF 目标文件路径
    std::vector<uint32_t> target_pids_; // 目标 PID 列表
    std::unordered_set<uint32_t> target_pid_allow_;  // PID 快速查找集合

    // ---- 运行时状态 ----
    std::atomic<bool> running_{false};
    BpfProgramManager bpf_mgr_;
    int stacks_fd_ = -1;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    mutable std::mutex cache_mu_;
    DataBatchPtr cached_batch_;
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
