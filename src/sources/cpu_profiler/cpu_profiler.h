// ============================================================================
// CpuProfilerSource — eBPF CPU 性能剖析器（火焰图数据源）
// ============================================================================
//
// 通过 Linux perf_event 子系统配合 eBPF 程序，在 CPU 上定时采样当前进程的
// 内核和用户态调用栈，生成类似于 `perf record` 的火焰图级别堆栈采样数据。
//
// 采集指标：
// ==========
// 1. 堆栈采样样本（StackSample），每条样本包含：
//    - pid / tid：被采样进程/线程
//    - comm：进程名称
//    - kernel_stack_id / user_stack_id：内核态/用户态堆栈 ID
//    - kernel_stack / user_stack：解析后的完整调用栈帧地址列表
//    - count：该堆栈被采到的次数（聚合模式下）
//    - cpu：采样所在 CPU 核心号（流式模式下）
//
// 2. 样本按 (pid, tid, comm, kernel_stack_id, user_stack_id) 五元组聚合，
//    定位 CPU 热点函数和调用路径。
//
// 工作原理：
// ==========
// 1. 为每个在线 CPU 核心创建一个 perf_event（PERF_COUNT_SW_CPU_CLOCK），
//    设置采样频率（默认 49 Hz）。
// 2. 通过 PERF_EVENT_IOC_SET_BPF ioctl 将 eBPF 程序挂载到 perf_event。
// 3. 每次定时器触发时，内核自动调用 eBPF 程序采集当前任务的调用栈。
// 4. eBPF 程序将堆栈帧地址存入 BPF_MAP_TYPE_STACK_TRACE 的 stacks map，
//    并在 stack_counts map 中递增对应五元组的计数。
// 5. 用户态定期（或按需）从 stack_counts map 中拉取聚合结果。
//
// 两种工作模式：
// ==============
// - aggregated（聚合拉取模式，默认）：
//     后台线程每 aggregate_interval_ms 毫秒从 eBPF map 中快照累加计数，
//     然后清空 map。Collect() 返回最近一次快照的结果。
//     适合周期性轮询场景（配合其它 Pull 模式插件统一调度）。
//
// - stream（流式推送模式，mode="stream"）：
//     通过 BPF ring buffer 实时推送每条采样事件到 HandleStreamEvent，
//     立即构造 batch 并通过 callback_ 推送到下游管道。
//     适合需要实时火焰图或低延迟响应的场景。
//
// 过滤配置：
// ==========
// - target_pids: 逗号分隔的 PID 列表，只采集这些进程
// - target_comms: 逗号分隔的进程名列表，只采集名称匹配的进程
//   （过滤逻辑在 eBPF 内核侧执行，避免无用的数据传输）
//
// 配置参数：
// ==========
// - frequency_hz: 采样频率（默认 49 Hz，大多数 Linux 内核限制非 root 49Hz）
// - aggregate_interval_ms: 聚合间隔（默认 1000ms）
// - stack_depth: 最大堆栈深度（默认 128，受 MAX_STACK_DEPTH 约束）
// - user_stacks: 是否采集用户态堆栈（默认 true）
// - kernel_stacks: 是否采集内核态堆栈（默认 true）
// - mode: 工作模式 "aggregated" 或 "stream"（默认 "aggregated"）
// - bpf_object: eBPF 目标文件路径（必需）
// ============================================================================

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <unistd.h>

#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ============================================================================
// ParseOnlineCpuIds — 解析在线 CPU 核心列表
// ============================================================================
// 读取 /sys/devices/system/cpu/online，解析逗号分隔的范围表示法。
// 例如 "0-3,8-11" → {0,1,2,3,8,9,10,11}
// 返回在线 CPU ID 的 vector，读取失败则返回空。
inline std::vector<int> ParseOnlineCpuIds() {
    std::vector<int> cpus;
    std::ifstream f("/sys/devices/system/cpu/online");
    if (!f)
        return cpus;
    std::string line;
    std::getline(f, line);
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, ',')) {
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(part));
        } else {
            int lo = std::stoi(part.substr(0, dash));
            int hi = std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi; ++c)
                cpus.push_back(c);
        }
    }
    return cpus;
}

// ============================================================================
// ParseCommaSeparatedInts — 解析逗号分隔的整数列表
// ============================================================================
// 将 "123,456,789" 格式的字符串解析为 uint32_t 的 vector。
// 自动去除每个元素前后的空白字符，非法数值会被静默跳过。
inline void ParseCommaSeparatedInts(const std::string& s,
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
// ParseCommaSeparatedStrings — 解析逗号分隔的字符串列表
// ============================================================================
// 将 "nginx,mysqld,redis" 格式的字符串解析为 string 的 vector。
// 自动去除每个元素前后的空白字符。
inline void ParseCommaSeparatedStrings(const std::string& s,
                                       std::vector<std::string>* out) {
    out->clear();
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        if (!token.empty())
            out->push_back(token);
    }
}

// ============================================================================
// PerfEventOpenSys — perf_event_open 系统调用封装
// ============================================================================
// 通过 syscall() 直接调用 perf_event_open，绕过 glibc 封装（部分发行版未提供）。
// 兼容不同的内核头文件宏定义（SYS_perf_event_open 或 __NR_perf_event_open）。
//
// 参数：attr=perf_event 属性配置, pid=目标进程(-1=任意进程),
//       cpu=目标CPU核心, group_fd=组领导fd(-1=新组), flags=标志位
// 返回：成功返回文件描述符，失败返回 -1（errno 已设置）
inline long PerfEventOpenSys(struct perf_event_attr* attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
#if defined(SYS_perf_event_open)
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
#elif defined(__NR_perf_event_open)
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
#else
    (void)attr;
    (void)pid;
    (void)cpu;
    (void)group_fd;
    (void)flags;
    return -1;
#endif
}

// ============================================================================
// CpuProfilerSource 类 — CPU 性能剖析插件主体
// ============================================================================
class CpuProfilerSource : public SourcePlugin {
public:
    // 插件元信息
    const char* Name() const override { return "cpu_profiler"; }
    const char* Version() const override { return "0.2.0"; }

    // 流式模式下返回 true，表示主动推送而非被动轮询
    bool IsPushMode() const override { return stream_mode_; }

    // ========================================================================
    // Collect — 采集方法（聚合模式下的 Pull 入口）
    // ========================================================================
    // 聚合模式下返回最近一次后台线程生成的 batch 快照。
    // 如果 eBPF 未加载或 map 不可用，返回错误。
    // 线程安全：通过 last_batch_mu_ 保护 last_batch_ 的读写。
    StatusOr<DataBatchPtr> Collect() override {
        std::lock_guard<std::mutex> lock(last_batch_mu_);
        if (last_batch_ && !last_batch_->Empty())
            return last_batch_;
        if (counts_fd_ < 0 || stacks_fd_ < 0)
            return Status::Error(StatusCode::kUnavailable, "BPF not loaded");
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        SnapshotAggregatedCounts(batch.get());
        return batch;
    }

    // ========================================================================
    // Init — 初始化插件，读取并缓存所有配置参数
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        frequency_hz_ = static_cast<int>(config["frequency_hz"].AsInt(49));
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(1000));
        stack_depth_ = static_cast<int>(config["stack_depth"].AsInt(128));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);

        auto mode = config["mode"].AsString("aggregated");
        stream_mode_ = (mode == "stream");

        bpf_obj_path_ = config["bpf_object"].AsString("");

        ParseCommaSeparatedInts(config["target_pids"].AsString(""),
                                &target_pids_);
        ParseCommaSeparatedStrings(config["target_comms"].AsString(""),
                                   &target_comms_);

        // 限制最大栈深度
        if (stack_depth_ > MAX_STACK_DEPTH)
            stack_depth_ = MAX_STACK_DEPTH;

        (void)stack_depth_;  // BPF 对象编译时已固定 MAX_STACK_DEPTH；保留此配置 API
        return Status::Ok();
    }

    // ========================================================================
    // Start — 启动 eBPF 程序和 perf_event 采样
    // ========================================================================
    // 1. 加载 eBPF 目标文件并验证必需 maps/programs 存在
    // 2. 遍历所有在线 CPU 核心，为每个核心创建并配置 perf_event
    //    - 使用 PERF_COUNT_SW_CPU_CLOCK 软件事件
    //    - 设置频率采样模式（freq=1, sample_freq=frequency_hz_）
    //    - 根据配置过滤用户态/内核态堆栈
    // 3. 将 eBPF 程序挂载到 perf_event（PERF_EVENT_IOC_SET_BPF）
    // 4. 根据模式启动后台线程（stream → StreamPollLoop / aggregated → AggregatedPullLoop）
    Status Start() override {
        // 没有指定 BPF 目标文件 → 闲置模式，不执行任何采样
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "cpu_profiler: no bpf_object path specified; profiler idle "
                "(install probes and set bpf_object)");
            running_.store(false);
            return Status::Ok();
        }

        // 加载 eBPF 对象
        auto st = bpf_mgr_.LoadObject("cpu_profiler", bpf_obj_path_);
        if (!st.ok())
            return st;

        // 获取堆栈 map 和计数 map 的文件描述符
        int stacks_fd = bpf_mgr_.GetMapFd("cpu_profiler", "stacks");
        int counts_fd = bpf_mgr_.GetMapFd("cpu_profiler", "stack_counts");
        if (stacks_fd < 0 || counts_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: stacks/stack_counts maps missing");
        }
        stacks_fd_ = stacks_fd;
        counts_fd_ = counts_fd;

        // 向 eBPF 侧下发过滤配置（PID 白名单、进程名白名单）
        ApplyFilterMaps();

        // 获取 eBPF 程序的文件描述符（perf_event 回调入口）
        int prog_fd = bpf_mgr_.GetProgFd("cpu_profiler", "on_cpu_sample");
        if (prog_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: on_cpu_sample program missing");
        }

        // 解析在线 CPU 列表
        auto cpus = ParseOnlineCpuIds();
        if (cpus.empty()) {
            IL_WARN("cpu_profiler: could not read online CPUs; defaulting to cpu 0");
            cpus.push_back(0);
        }

        // 为每个 CPU 核心创建 perf_event 并挂载 eBPF 程序
        for (int cpu : cpus) {
            struct perf_event_attr attr = {};
            attr.size = sizeof(attr);
            attr.type = PERF_TYPE_SOFTWARE;                // 软件事件类型
            attr.config = PERF_COUNT_SW_CPU_CLOCK;         // CPU 时钟事件（按时钟周期触发）
            attr.freq = 1;                                 // 使用频率模式（非周期模式）
            attr.sample_freq = static_cast<uint64_t>(frequency_hz_);  // 采样频率
            attr.sample_type = PERF_SAMPLE_CALLCHAIN;      // 需要采集调用链
            attr.disabled = 1;                             // 创建后先禁用，等待 BPF 挂载
            attr.exclude_user = user_stacks_ ? 0 : 1;      // 是否排除用户态
            attr.exclude_kernel = kernel_stacks_ ? 0 : 1;  // 是否排除内核态

            // 打开 perf_event
            int fd = static_cast<int>(
                PerfEventOpenSys(&attr, /*pid=*/-1, cpu, /*group=*/-1,
                                 PERF_FLAG_FD_CLOEXEC));
            if (fd < 0) {
                IL_WARN("cpu_profiler: perf_event_open failed for cpu %d errno=%d",
                        cpu, errno);
                continue;
            }

            // 将 eBPF 程序挂载到 perf_event
            if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
                IL_WARN("cpu_profiler: PERF_EVENT_IOC_SET_BPF failed cpu %d errno=%d",
                        cpu, errno);
                close(fd);
                continue;
            }

            // 启用 perf_event
            if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
                IL_WARN("cpu_profiler: PERF_EVENT_IOC_ENABLE failed cpu %d errno=%d",
                        cpu, errno);
                close(fd);
                continue;
            }

            // 记录成功的 fd 用于后续清理
            perf_fds_.push_back(fd);
        }

        if (perf_fds_.empty()) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: failed to open any perf events");
        }

        running_.store(true);

        // 根据模式启动对应的后台轮询线程
        if (stream_mode_) {
            int rb_fd = bpf_mgr_.GetMapFd("cpu_profiler", "cpu_events");
            if (rb_fd < 0) {
                return Status::Error(StatusCode::kInternal,
                                     "cpu_profiler: cpu_events ringbuf missing");
            }
            ring_buf_ = bpf_mgr_.CreateRingBuffer(rb_fd, HandleStreamEvent, this);
            if (!ring_buf_) {
                return Status::Error(StatusCode::kInternal,
                                     "cpu_profiler: ring buffer init failed");
            }
            poll_thread_ = std::thread([this] { StreamPollLoop(); });
        } else {
            agg_thread_ = std::thread([this] { AggregatedPullLoop(); });
        }

        IL_INFO("cpu_profiler started (%s mode, %zu perf fds)",
                stream_mode_ ? "stream" : "aggregated", perf_fds_.size());
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止采样，清理所有资源
    // ========================================================================
    // 1. 设置 running_ 标志为 false
    // 2. 等待后台线程结束
    // 3. 释放 ring buffer 和关闭所有 perf_event fd
    // 4. 分离所有 eBPF 挂钩点
    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (agg_thread_.joinable())
            agg_thread_.join();

        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }

        for (int fd : perf_fds_) {
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            close(fd);
        }
        perf_fds_.clear();

        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

private:
    // ========================================================================
    // ApplyFilterMaps — 将过滤配置写入 eBPF 侧的配置 map
    // ========================================================================
    // 将 PID 列表和进程名列表写入对应的 eBPF map（target_pids / target_comms），
    // 同时设置 cpu_profiler_cfg 配置 map 中的标志位，告知 eBPF 程序是否启用过滤。
    //
    // 标志位含义：
    //   位 0 (1): stream 模式
    //   位 1 (2): 启用 PID 过滤
    //   位 2 (4): 启用进程名过滤
    void ApplyFilterMaps() {
        int cfg_fd = bpf_mgr_.GetMapFd("cpu_profiler", "cpu_profiler_cfg");
        uint32_t flags =
            (stream_mode_ ? 1u : 0u) |
            (!target_pids_.empty() ? 2u : 0u) |
            (!target_comms_.empty() ? 4u : 0u);
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &flags, BPF_ANY);
        }

        // 写入 PID 白名单
        int pid_fd = bpf_mgr_.GetMapFd("cpu_profiler", "target_pids");
        if (pid_fd >= 0 && !target_pids_.empty()) {
            uint8_t one = 1;
            for (uint32_t pid : target_pids_)
                bpf_map_update_elem(pid_fd, &pid, &one, BPF_ANY);
        }

        // 写入进程名白名单
        int comm_fd = bpf_mgr_.GetMapFd("cpu_profiler", "target_comms");
        if (comm_fd >= 0 && !target_comms_.empty()) {
            uint8_t one = 1;
            for (const auto& name : target_comms_) {
                char key[TASK_COMM_LEN] = {};
                std::memcpy(key, name.c_str(),
                            std::min(name.size(), sizeof(key) - 1));
                bpf_map_update_elem(comm_fd, key, &one, BPF_ANY);
            }
        }
    }

    // ========================================================================
    // StreamPollLoop — 流式模式的后台轮询循环
    // ========================================================================
    // 持续调用 ring_buffer__poll 从 BPF ring buffer 中读取实时事件。
    // 每次 poll 超时 100ms，检查 running_ 标志决定是否退出。
    void StreamPollLoop() {
        while (running_.load()) {
            int err = ring_buffer__poll(ring_buf_, 100);
            if (err < 0 && err != -EINTR)
                IL_WARN("cpu_profiler: ringbuf poll err %d", err);
        }
    }

    // ========================================================================
    // AggregatedPullLoop — 聚合模式的后台轮询循环
    // ========================================================================
    // 每 aggregate_interval_ms_ 毫秒执行一次 FlushAggregatedCounts，
    // 将当前周期内 eBPF 累计的采样计数取出并清空 map。
    void AggregatedPullLoop() {
        using namespace std::chrono_literals;
        while (running_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(aggregate_interval_ms_));
            if (!running_.load())
                break;
            FlushAggregatedCounts();
        }
    }

    // ========================================================================
    // LookupStackFrames — 根据 stack_id 查询堆栈帧地址列表
    // ========================================================================
    // 从 BPF_MAP_TYPE_STACK_TRACE 类型的 stacks map 中查询指定 stack_id
    // 对应的调用栈原始地址数组。每个元素是一条指令地址（IP）。
    //
    // 参数：
    //   stack_id - 堆栈追踪 ID（来自 eBPF bpf_get_stackid() 返回值）
    //   out      - 输出参数，填充 StackFrame 列表
    void LookupStackFrames(int32_t stack_id, std::vector<StackFrame>* out) {
        out->clear();
        if (stack_id < 0 || stacks_fd_ < 0)
            return;

        uint64_t raw[MAX_STACK_DEPTH];
        std::memset(raw, 0, sizeof(raw));
        uint32_t sid = static_cast<uint32_t>(stack_id);
        if (bpf_map_lookup_elem(stacks_fd_, &sid, raw) != 0)
            return;

        for (int i = 0; i < MAX_STACK_DEPTH && i < stack_depth_; ++i) {
            if (raw[i] == 0)
                break;
            StackFrame fr;
            fr.address = raw[i];
            out->push_back(fr);
        }
    }

    // ========================================================================
    // SnapshotAggregatedCounts — 快照 eBPF 聚合计数，生成 batch
    // ========================================================================
    // 遍历 stack_counts map 中的所有条目，为每个 (pid,tid,comm,kstack,ustack)
    // 五元组生成一条 StackSample。同时解析对应的 kernel_stack 和 user_stack。
    void SnapshotAggregatedCounts(DataBatch* batch) {
        if (counts_fd_ < 0)
            return;

        il_stack_key cur{};
        il_stack_key next{};
        std::vector<il_stack_key> keys;

        // 先收集所有 key（不能在遍历时修改 map）
        int err = bpf_map_get_next_key(counts_fd_, nullptr, &cur);
        while (err == 0) {
            keys.push_back(cur);
            err = bpf_map_get_next_key(counts_fd_, &cur, &next);
            cur = next;
        }

        for (const auto& key : keys) {
            uint64_t count = 0;
            if (bpf_map_lookup_elem(counts_fd_, &key, &count) != 0)
                continue;

            // 构造一条堆栈采样样本
            auto& sample = batch->AddStackSample();
            sample.pid = key.pid;
            sample.tid = key.tid;
            sample.comm = batch->InternString(std::string_view(
                key.comm, strnlen(key.comm, TASK_COMM_LEN)));
            sample.count = count;
            sample.sample_type = SampleType::kOnCpu;
            sample.kernel_stack_id = key.kernel_stack_id;
            sample.user_stack_id = key.user_stack_id;

            // 解析内核态和用户态的完整调用栈
            LookupStackFrames(key.kernel_stack_id, &sample.kernel_stack);
            LookupStackFrames(key.user_stack_id, &sample.user_stack);
        }
    }

    // ========================================================================
    // FlushAggregatedCounts — 清空并推送聚合计数
    // ========================================================================
    // 1. 快照当前 eBPF map 中的所有聚合计数
    // 2. 逐个删除已读取的条目（避免重复上报）
    // 3. 将生成的 batch 同时缓存在 last_batch_ 和推送到 callback_
    void FlushAggregatedCounts() {
        if (counts_fd_ < 0 || !callback_)
            return;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        SnapshotAggregatedCounts(batch.get());

        // 删除已读取的条目（eBPF map 遍历删除技巧：
        // 在 while 循环中始终删除 cur，然后向 next 推进）
        il_stack_key cur{};
        il_stack_key next{};
        int err = bpf_map_get_next_key(counts_fd_, nullptr, &cur);
        while (err == 0) {
            il_stack_key to_delete = cur;
            err = bpf_map_get_next_key(counts_fd_, &cur, &next);
            cur = next;
            bpf_map_delete_elem(counts_fd_, &to_delete);
        }

        if (!batch->stack_samples().empty()) {
            {
                std::lock_guard<std::mutex> lock(last_batch_mu_);
                last_batch_ = batch;
            }
            callback_(std::move(batch));
        }
    }

    // ========================================================================
    // HandleStreamEvent — 流式模式的事件回调（静态函数）
    // ========================================================================
    // 当 eBPF 通过 ring buffer 推送一条 cpu_sample_event 时被调用。
    // 直接构造一条 count=1 的 StackSample 并通过 callback_ 立即推送。
    // 每次事件触发时解析一次堆栈帧。
    static int HandleStreamEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<CpuProfilerSource*>(ctx);
        if (size < sizeof(il_cpu_sample_event))
            return 0;
        auto* ev = static_cast<il_cpu_sample_event*>(data);

        if (!self->callback_)
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();
        sample.pid = ev->pid;
        sample.tid = ev->tid;
        sample.cpu = ev->cpu;
        sample.comm = batch->InternString(std::string_view(
            ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
        sample.count = 1;
        sample.sample_type = SampleType::kOnCpu;
        sample.kernel_stack_id = ev->kernel_stack_id;
        sample.user_stack_id = ev->user_stack_id;

        self->LookupStackFrames(ev->kernel_stack_id, &sample.kernel_stack);
        self->LookupStackFrames(ev->user_stack_id, &sample.user_stack);

        self->callback_(std::move(batch));
        return 0;
    }

    // ---- 配置参数 ----
    int frequency_hz_ = 49;                   // 采样频率（Hz）
    uint32_t aggregate_interval_ms_ = 1000;   // 聚合间隔（毫秒）
    int stack_depth_ = MAX_STACK_DEPTH;       // 最大堆栈深度
    bool user_stacks_ = true;                 // 是否采集用户态堆栈
    bool kernel_stacks_ = true;               // 是否采集内核态堆栈
    bool stream_mode_ = false;                // 是否使用流式推送模式

    std::string bpf_obj_path_;                // eBPF 目标文件路径
    std::vector<uint32_t> target_pids_;       // 目标 PID 列表
    std::vector<std::string> target_comms_;   // 目标进程名列表

    // ---- 运行时状态 ----
    std::atomic<bool> running_{false};        // 运行中标志（线程安全）
    BpfProgramManager bpf_mgr_;               // eBPF 程序管理器
    int stacks_fd_ = -1;                      // stacks map 的文件描述符
    int counts_fd_ = -1;                      // stack_counts map 的文件描述符
    std::vector<int> perf_fds_;               // 所有 perf_event 的文件描述符
    struct ring_buffer* ring_buf_ = nullptr;  // BPF ring buffer 句柄
    std::thread poll_thread_;                 // 流式轮询线程
    std::thread agg_thread_;                  // 聚合轮询线程

    mutable std::mutex last_batch_mu_;        // 保护 last_batch_ 的互斥锁
    DataBatchPtr last_batch_;                 // 最近一次聚合结果的缓存
};

// 自动注册到插件注册表
IL_REGISTER_SOURCE("cpu_profiler", CpuProfilerSource);

}  // namespace illuminator
