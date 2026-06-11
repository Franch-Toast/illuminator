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
// 3. 当前为 Pull 模式（IsPushMode=false），通过 ring buffer 收集事件
//    并在 Collect() 时返回（适合定期轮询场景）。
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
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "ebpf/include/bpf_compat.h"

#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "core/threading/thread_util.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "ebpf/loader/stack_trace_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ============================================================================
// OffcpuProfilerSource 类 — Off-CPU 剖析插件主体
// ============================================================================
class OffcpuProfilerSource : public SourcePlugin {
public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* Version() const override { return "0.1.0"; }

    bool IsPushMode() const override { return false; }
    bool HasBpfProbe() const override { return !stub_mode_; }
    bool IsStub() const override { return stub_mode_; }

    StatusOr<DataBatchPtr> Collect() override {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto result = cached_batch_
            ? cached_batch_
            : std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        cached_batch_ = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        return result;
    }

    // HTTP API 查询入口：返回最近一轮聚合数据的 JSON 快照
    StatusOr<std::string> QueryExtra(
        const std::string& query, const QueryParams& /*params*/) override {
        if (query == "snapshot") {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            return latest_json_snapshot_;
        }
        return Status::Error(StatusCode::kUnimplemented, "unknown query");
    }

    // ========================================================================
    // Init — 初始化 Off-CPU 剖析器配置
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        min_duration_us_ =
            static_cast<uint32_t>(config["min_duration_us"].AsInt(10000));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);
        start_delay_seconds_ =
            static_cast<int>(config["start_delay_seconds"].AsInt(3));
        bpf_obj_path_ = config["bpf_object"].AsString("");
        target_pids_ = ParseCommaSeparated<uint32_t>(
            config["target_pids"].AsString(""));
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);

        // 解析进程名白名单
        auto comms_str = config["target_comms"].AsString("");
        target_comms_.clear();
        if (!comms_str.empty()) {
            auto parts = ParseCommaSeparated<std::string>(comms_str);
            for (auto& s : parts) {
                if (!s.empty()) target_comms_.push_back(s);
            }
        }

        return Status::Ok();
    }

    // ========================================================================
    // Start — 加载 eBPF 程序并开始追踪 Off-CPU 等待
    // ========================================================================
    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("offcpu_profiler: no bpf_object path; idle mode");
            stub_mode_ = true;
            running_.store(false);
            return Status::Ok();
        }

        auto st = bpf_mgr_.LoadObject("offcpu_profiler", bpf_obj_path_);
        if (!st.ok())
            return st;

        // 写入运行时配置到 offcpu_cfg BPF map（3 个 slot）
        cfg_fd_ = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_cfg");
        if (cfg_fd_ >= 0) {
            // 启动时先写 flags 但 bit4=0（禁用），等延迟后再启用
            uint32_t k0 = 0;
            uint32_t cfg_flags = (user_stacks_ ? 1u : 0u)
                               | (kernel_stacks_ ? 2u : 0u)
                               | (!target_pid_allow_.empty() ? 4u : 0u)
                               | (!target_comms_.empty() ? 8u : 0u);
            cfg_flags_base_ = cfg_flags;
            // 暂不设 bit4，探针已 attach 但不采集
            bpf_map_update_elem(cfg_fd_, &k0, &cfg_flags, BPF_ANY);

            // slot 1+2: min_duration_ns (64-bit split into two 32-bit)
            uint64_t min_ns = static_cast<uint64_t>(min_duration_us_) * 1000ULL;
            uint32_t k1 = 1, k2 = 2;
            uint32_t lo = static_cast<uint32_t>(min_ns & 0xFFFFFFFF);
            uint32_t hi = static_cast<uint32_t>(min_ns >> 32);
            bpf_map_update_elem(cfg_fd_, &k1, &lo, BPF_ANY);
            bpf_map_update_elem(cfg_fd_, &k2, &hi, BPF_ANY);

            IL_INFO("offcpu_profiler: cfg_flags=0x{:x} min_duration={}us "
                    "(user_stacks={}, kernel_stacks={}, pid_filter={}, "
                    "comm_filter={}) — will enable after {}s delay",
                    cfg_flags, min_duration_us_, user_stacks_, kernel_stacks_,
                    !target_pid_allow_.empty(), !target_comms_.empty(),
                    start_delay_seconds_);
        }

        // 写入目标 PID 到 offcpu_target_pids BPF map
        if (!target_pid_allow_.empty()) {
            int pids_fd = bpf_mgr_.GetMapFd("offcpu_profiler",
                                            "offcpu_target_pids");
            if (pids_fd >= 0) {
                uint8_t val = 1;
                for (uint32_t pid : target_pid_allow_) {
                    bpf_map_update_elem(pids_fd, &pid, &val, BPF_ANY);
                }
                IL_INFO("offcpu_profiler: loaded {} target PIDs into BPF map",
                        target_pid_allow_.size());
            }
        }

        // 写入目标进程名到 offcpu_target_comms BPF map
        if (!target_comms_.empty()) {
            int comms_fd = bpf_mgr_.GetMapFd("offcpu_profiler",
                                             "offcpu_target_comms");
            if (comms_fd >= 0) {
                uint8_t val = 1;
                for (const auto& comm : target_comms_) {
                    char key[16] = {};
                    std::memcpy(key, comm.c_str(),
                                std::min(comm.size(), sizeof(key) - 1));
                    bpf_map_update_elem(comms_fd, key, &val, BPF_ANY);
                }
                IL_INFO("offcpu_profiler: loaded {} target comms into BPF map",
                        target_comms_.size());
            }
        }

        // 挂载 Off-CPU 追踪 eBPF 程序
        st = bpf_mgr_.AttachProgram("offcpu_profiler", "trace_offcpu");
        if (!st.ok())
            return st;

        stacks_fd_ = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_stacks");

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

        running_.store(true);
        poll_thread_ = std::thread([this] {
            SetThreadName("offcpu-poll");
            while (running_.load()) {
                int err = ring_buffer__poll(ring_buf_, 100);
                if (err < 0 && err != -EINTR)
                    IL_WARN("offcpu_profiler: ringbuf poll err {}", err);
            }
        });

        // 启动延迟线程：等待系统稳定后再启用 BPF 采集（参考 perf_ebpf start_delay_seconds）
        delay_thread_ = std::thread([this] {
            SetThreadName("offcpu-delay");
            for (int i = 0; i < start_delay_seconds_ && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (running_.load() && cfg_fd_ >= 0) {
                uint32_t k0 = 0;
                uint32_t enabled_flags = cfg_flags_base_ | 16u;  // 设置 bit4 启用
                bpf_map_update_elem(cfg_fd_, &k0, &enabled_flags, BPF_ANY);
                IL_INFO("offcpu_profiler: enabled after {}s delay "
                        "(flags=0x{:x})", start_delay_seconds_, enabled_flags);
            }
        });

        IL_INFO("offcpu_profiler started (min_duration={}us, "
                "target_pids={}, target_comms={}, delay={}s)",
                min_duration_us_, target_pid_allow_.size(),
                target_comms_.size(), start_delay_seconds_);
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止追踪，释放所有 eBPF 资源
    // ========================================================================
    Status Stop() override {
        running_.store(false);
        if (delay_thread_.joinable())
            delay_thread_.join();
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        stacks_fd_ = -1;
        cfg_fd_ = -1;
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
    // HandleEvent — ring buffer 信号回调（静态函数）
    // ========================================================================
    // 聚合模式：BPF 层仅发送低频信号（pid=0），收到后批量读取 offcpu_stats map
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<OffcpuProfilerSource*>(ctx);
        if (size < sizeof(il_offcpu_event))
            return 0;
        auto* ev = static_cast<il_offcpu_event*>(data);

        if (ev->pid == 0) {
            // 聚合信号：触发 batch 读取
            self->ReadAndClearStats();
            return 0;
        }

        // 兼容旧模式（理论上不应触发）
        return 0;
    }

    // ========================================================================
    // ReadAndClearStats — 批量读取 offcpu_stats BPF map 并转换为 DataBatch
    // ========================================================================
    void ReadAndClearStats() {
        int stats_fd = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_stats");
        if (stats_fd < 0) return;

        struct offcpu_stat_key {
            uint32_t pid;
            int32_t kernel_stack_id;
            int32_t user_stack_id;
        };
        struct offcpu_stat_val {
            uint64_t total_ns;
            uint32_t count;
            uint32_t cpu;
            char comm[16];
        };

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        offcpu_stat_key key = {}, next_key = {};
        offcpu_stat_val val = {};

        while (bpf_map_get_next_key(stats_fd, &key, &next_key) == 0) {
            if (bpf_map_lookup_elem(stats_fd, &next_key, &val) == 0) {
                if (AllowPid(next_key.pid)) {
                    auto kernel_stack =
                        LookupBpfStackTrace(stacks_fd_, next_key.kernel_stack_id);
                    auto user_stack =
                        LookupBpfStackTrace(stacks_fd_, next_key.user_stack_id);

                    auto& cs = batch->AddStackSample();
                    cs.pid = next_key.pid;
                    cs.tid = next_key.pid;
                    cs.cpu = val.cpu;
                    cs.comm = batch->InternString(std::string_view(
                        val.comm, strnlen(val.comm, 16)));
                    cs.sample_type = SampleType::kOffCpu;
                    cs.duration_ns = val.total_ns;
                    cs.count = val.count;
                    cs.kernel_stack_id = next_key.kernel_stack_id;
                    cs.user_stack_id = next_key.user_stack_id;
                    cs.kernel_stack = kernel_stack;
                    cs.user_stack = user_stack;
                }
            }
            bpf_map_delete_elem(stats_fd, &next_key);
            key = next_key;
        }

        if (batch->stack_samples().empty()) return;

        // 生成 JSON 快照供 HTTP API 使用（在 symbolizer 之前，只有地址）
        {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            json j;
            json samples = json::array();
            for (auto& s : batch->stack_samples()) {
                json sj;
                sj["pid"] = s.pid;
                sj["tid"] = s.tid;
                sj["cpu"] = s.cpu;
                sj["count"] = s.count;
                sj["duration_ns"] = s.duration_ns;
                sj["comm"] = std::string(s.comm.data(), s.comm.size());
                sj["sample_type"] = static_cast<int>(s.sample_type);
                json ks = json::array(), us = json::array();
                for (auto& f : s.kernel_stack)
                    ks.push_back({{"address", f.address}});
                for (auto& f : s.user_stack)
                    us.push_back({{"address", f.address}});
                sj["kernel_stack"] = std::move(ks);
                sj["user_stack"] = std::move(us);
                samples.push_back(std::move(sj));
            }
            j["stack_samples"] = std::move(samples);
            j["pipeline"] = "offcpu_profile";
            latest_json_snapshot_ = j.dump();
        }

        // Pull 模式：复制样本到 cached_batch_（需要重新 intern 字符串）
        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            if (!cached_batch_)
                cached_batch_ = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
            for (auto& s : batch->stack_samples()) {
                auto& dst = cached_batch_->AddStackSample();
                dst.pid = s.pid;
                dst.tid = s.tid;
                dst.cpu = s.cpu;
                dst.count = s.count;
                dst.duration_ns = s.duration_ns;
                dst.sample_type = s.sample_type;
                dst.kernel_stack_id = s.kernel_stack_id;
                dst.user_stack_id = s.user_stack_id;
                dst.kernel_stack = std::move(s.kernel_stack);
                dst.user_stack = std::move(s.user_stack);
                // 重新 intern comm 到目标 batch 的 Arena
                dst.comm = cached_batch_->InternString(s.comm);
            }
        }

        // Push 模式：callback 推送
        if (callback_) {
            callback_(std::move(batch));
        }
    }

    bool stub_mode_ = false;
    uint32_t min_duration_us_ = 10000;
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    int start_delay_seconds_ = 3;
    std::string bpf_obj_path_;
    std::vector<uint32_t> target_pids_;
    std::unordered_set<uint32_t> target_pid_allow_;
    std::vector<std::string> target_comms_;

    // ---- 运行时状态 ----
    std::atomic<bool> running_{false};
    BpfProgramManager bpf_mgr_;
    int stacks_fd_ = -1;
    int cfg_fd_ = -1;
    uint32_t cfg_flags_base_ = 0;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    std::thread delay_thread_;
    mutable std::mutex cache_mu_;
    DataBatchPtr cached_batch_;
    mutable std::mutex snapshot_mu_;
    std::string latest_json_snapshot_ = "{\"stack_samples\":[]}";
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
