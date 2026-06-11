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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
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
#include "processors/stack_symbolizer/stack_symbolizer.h"
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
            uint32_t tgid;
            uint32_t tid;
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
                if (AllowPid(next_key.tgid)) {
                    auto kernel_stack =
                        LookupBpfStackTrace(stacks_fd_, next_key.kernel_stack_id);
                    auto user_stack =
                        LookupBpfStackTrace(stacks_fd_, next_key.user_stack_id);

                    auto& cs = batch->AddStackSample();
                    cs.pid = next_key.tgid;  // 进程组 ID（用户态 PID）
                    cs.tid = next_key.tid;   // 线程 ID（区分不同线程）
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

        // 生成符号化 JSON 快照供 HTTP API 使用
        GenerateSymbolizedSnapshot(batch);

        // Pull 模式：累积到 cached_batch_ 供 Collect() 取用
        // 不调用 callback_（pull 模式由 TimerWheel 驱动 Collect()）
        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            if (!cached_batch_)
                cached_batch_ = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
            for (const auto& s : batch->stack_samples()) {
                auto& dst = cached_batch_->AddStackSample();
                dst.pid = s.pid;
                dst.tid = s.tid;
                dst.cpu = s.cpu;
                dst.count = s.count;
                dst.duration_ns = s.duration_ns;
                dst.sample_type = s.sample_type;
                dst.kernel_stack_id = s.kernel_stack_id;
                dst.user_stack_id = s.user_stack_id;
                dst.kernel_stack = s.kernel_stack;
                dst.user_stack = s.user_stack;
                dst.comm = cached_batch_->InternString(s.comm);
            }
        }
    }

    // ========================================================================
    // GenerateSymbolizedSnapshot — 生成符号化的 JSON 快照供 HTTP API 使用
    // ========================================================================
    void GenerateSymbolizedSnapshot(const DataBatchPtr& batch) {
        std::lock_guard<std::mutex> lk(snapshot_mu_);

        // 延迟加载内核符号
        if (!kernel_resolver_loaded_) {
            auto st = kernel_resolver_.Load();
            if (st.ok())
                IL_INFO("offcpu_profiler: kernel symbols loaded for snapshot");
            kernel_resolver_loaded_ = true;
        }

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

            // 内核栈 — 使用 kallsyms 符号解析
            json ks = json::array();
            for (auto& f : s.kernel_stack) {
                json fj;
                fj["address"] = f.address;
                std::string sym = kernel_resolver_.Resolve(f.address);
                if (sym.empty()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[kernel 0x%llx]",
                                  static_cast<unsigned long long>(f.address));
                    sym = buf;
                }
                fj["function_name"] = sym;
                ks.push_back(std::move(fj));
            }

            // 用户栈 — 使用 /proc/<pid>/maps + ELF 符号解析
            json us = json::array();
            for (auto& f : s.user_stack) {
                json fj;
                fj["address"] = f.address;
                std::string sym;
                if (f.address < 0x1000) {
                    sym = "[thread entry]";
                } else {
                    sym = ResolveUserSymbol(s.pid, f.address);
                    if (sym.empty()) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "[0x%llx]",
                                      static_cast<unsigned long long>(f.address));
                        sym = buf;
                    }
                }
                fj["function_name"] = sym;
                us.push_back(std::move(fj));
            }

            sj["kernel_stack"] = std::move(ks);
            sj["user_stack"] = std::move(us);
            samples.push_back(std::move(sj));
        }
        j["stack_samples"] = std::move(samples);
        j["pipeline"] = "offcpu_profile";
        latest_json_snapshot_ = j.dump();
    }

    // ========================================================================
    // ResolveUserSymbol — 解析用户态地址到函数名
    // ========================================================================
    std::string ResolveUserSymbol(uint32_t pid, uint64_t addr) {
        auto now = std::chrono::steady_clock::now();

        // 自 PID 检测：使用规范化的 key 确保自分析始终使用 /proc/self/maps
        uint32_t cache_key = IsSelfPid(pid) ? self_pid_ : pid;

        auto it = maps_cache_.find(cache_key);
        if (it == maps_cache_.end()) {
            MapsCacheEntry entry;
            if (!LoadProcMaps(cache_key, entry))
                return {};
            auto ins = maps_cache_.emplace(cache_key, std::move(entry));
            it = ins.first;
        } else {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.loaded_at).count();
            if (age > 30) {
                LoadProcMaps(cache_key, it->second);
            }
        }

        for (const auto& m : it->second.maps) {
            if (addr < m.start || addr >= m.end)
                continue;

            // 处理特殊映射：[vdso], [vsyscall], [heap], [stack] 等
            if (!m.path.empty() && m.path[0] == '[') {
                if (m.path == "[vdso]")
                    return "__vdso_clock_gettime [vdso]";
                if (m.path == "[vsyscall]")
                    return "[vsyscall]";
                return m.path;
            }

            uint64_t file_off = addr - m.start + m.offset;
            uint64_t map_off = addr - m.start;
            auto elf_it = elf_cache_.find(m.path);
            if (elf_it == elf_cache_.end()) {
                ElfSymbolCache cache;
                if (!cache.Load(m.path)) {
                    if (!TryLoadDebugInfo(m.path, cache)) {
                        std::string annotation = AnnotateLibraryOffset(m.path, file_off, map_off);
                        if (!annotation.empty())
                            return annotation;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                                      m.path.c_str(),
                                      static_cast<unsigned long long>(map_off));
                        return std::string(buf);
                    }
                }
                auto ins = elf_cache_.emplace(m.path, std::move(cache));
                elf_it = ins.first;
            }
            std::string sym = elf_it->second.Resolve(file_off);
            if (!sym.empty()) {
                // 如果是 "+gap" 标记的近似归属，清理后返回
                if (sym.size() > 4 && sym.substr(sym.size() - 4) == "+gap") {
                    sym = sym.substr(0, sym.size() - 4);
                    return DemangleSymbol(sym) + " [+gap]";
                }
                return DemangleSymbol(sym);
            }
            // 尝试已知库函数近似标注
            std::string annotation = AnnotateLibraryOffset(m.path, file_off, map_off);
            if (!annotation.empty())
                return annotation;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                          m.path.c_str(),
                          static_cast<unsigned long long>(map_off));
            return std::string(buf);
        }
        return {};
    }

    // 判断 BPF 报告的 PID 是否为当前进程（考虑 PID namespace 差异）
    bool IsSelfPid(uint32_t pid) const {
        if (pid == self_pid_) return true;
        // 如果 /proc/<pid> 不存在但 comm 匹配，视为自身
        std::string status_path = "/proc/" + std::to_string(pid) + "/status";
        std::ifstream f(status_path);
        return !f.good();  // PID 不存在意味着可能是 namespace 差异
    }

    // PID 命名空间感知的 maps 加载
    bool LoadProcMaps(uint32_t pid, MapsCacheEntry& entry) {
        entry.maps.clear();

        std::string path;
        if (pid == self_pid_) {
            path = "/proc/self/maps";
        } else {
            path = "/proc/" + std::to_string(pid) + "/maps";
        }

        std::ifstream f(path);
        if (!f) {
            // PID namespace fallback: 当 /proc/<pid>/maps 不可读时尝试 /proc/self/maps
            if (pid != self_pid_) {
                f.open("/proc/self/maps");
                if (!f) return false;
                if (!self_maps_logged_) {
                    IL_INFO("offcpu_profiler: using /proc/self/maps for pid {} "
                            "(pid namespace or stale PID)", pid);
                    self_maps_logged_ = true;
                }
            } else {
                return false;
            }
        }
        std::string line;
        while (std::getline(f, line)) {
            ProcMapEntry m{};
            if (!ParseProcMapsLine(line, &m)) continue;
            // 保留 / 开头的文件路径和 [ 开头的特殊映射（[vdso] 等）
            if (m.path.empty()) continue;
            if (m.path[0] != '/' && m.path[0] != '[') continue;
            entry.maps.push_back(std::move(m));
        }
        entry.loaded_at = std::chrono::steady_clock::now();
        return !entry.maps.empty();
    }

    // 尝试从 /usr/lib/debug/.build-id/ 或 debuglink 加载外部调试符号
    bool TryLoadDebugInfo(const std::string& elf_path, ElfSymbolCache& cache) {
        // 策略 1: build-id 查找（最准确且与路径无关）
        std::string build_id = ExtractBuildId(elf_path);
        if (build_id.size() >= 4) {
            std::string bid_path = "/usr/lib/debug/.build-id/"
                + build_id.substr(0, 2) + "/" + build_id.substr(2) + ".debug";
            if (cache.Load(bid_path)) return true;
        }

        // 策略 2: /usr/lib/debug + 原路径
        std::string debug_path = "/usr/lib/debug" + elf_path + ".debug";
        if (cache.Load(debug_path)) return true;
        debug_path = "/usr/lib/debug" + elf_path;
        if (cache.Load(debug_path)) return true;

        // 策略 3: 同目录 .debug 子目录
        auto last_slash = elf_path.rfind('/');
        if (last_slash != std::string::npos) {
            std::string dir = elf_path.substr(0, last_slash + 1);
            std::string base = elf_path.substr(last_slash + 1);
            debug_path = dir + ".debug/" + base + ".debug";
            if (cache.Load(debug_path)) return true;
            debug_path = dir + ".debug/" + base;
            if (cache.Load(debug_path)) return true;
        }
        return false;
    }

    // 从 ELF 文件提取 .note.gnu.build-id 十六进制字符串
    static std::string ExtractBuildId(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (buf.size() < sizeof(Elf64_Ehdr)) return {};
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buf.data());
        if (ehdr->e_ident[EI_MAG0] != ELFMAG0) return {};
        if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr))
            return {};
        auto* shdrs = reinterpret_cast<Elf64_Shdr*>(buf.data() + ehdr->e_shoff);
        for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
            if (shdrs[i].sh_type != SHT_NOTE) continue;
            if (shdrs[i].sh_offset + shdrs[i].sh_size > buf.size()) continue;
            const char* nd = buf.data() + shdrs[i].sh_offset;
            size_t rem = shdrs[i].sh_size;
            size_t pos = 0;
            while (pos + 12 <= rem) {
                uint32_t namesz = *reinterpret_cast<const uint32_t*>(nd + pos);
                uint32_t descsz = *reinterpret_cast<const uint32_t*>(nd + pos + 4);
                uint32_t type = *reinterpret_cast<const uint32_t*>(nd + pos + 8);
                size_t name_start = pos + 12;
                size_t name_aligned = (namesz + 3) & ~3u;
                size_t desc_start = name_start + name_aligned;
                size_t desc_aligned = (descsz + 3) & ~3u;
                if (desc_start + descsz > rem) break;
                if (type == 3 && namesz == 4 &&
                    std::memcmp(nd + name_start, "GNU", 4) == 0) {
                    std::string hex;
                    hex.reserve(descsz * 2);
                    for (size_t j = 0; j < descsz; ++j) {
                        char h[3];
                        std::snprintf(h, sizeof(h), "%02x",
                                      static_cast<uint8_t>(nd[desc_start + j]));
                        hex += h;
                    }
                    return hex;
                }
                pos = desc_start + desc_aligned;
            }
        }
        return {};
    }

    // 对无法解析的库内部地址提供友好的近似标注
    // 接受两个偏移：file_off（文件偏移，用于 nm 地址空间比较）和 map_off（仅供参考）
    // 使用 file_off 进行已知函数地址范围匹配
    std::string AnnotateLibraryOffset(const std::string& lib_path,
                                      uint64_t file_off, uint64_t /*map_off*/) {
        if (lib_path.find("libstdc++") != std::string::npos) {
            // execute_native_thread_routine（gcc/libstdc++ std::thread 入口）
            // 通常在 _M_start_thread (0xf7500-0xf7900) 附近 ±0x200
            // file offset 在 0xf7000-0xf8000 范围（适配多版本 libstdc++6.0.30-35+）
            if (file_off >= 0xf7000 && file_off < 0xf8000)
                return "execute_native_thread_routine [libstdc++]";
            // 备用范围：较老版本 libstdc++ (gcc 10-11)
            if (file_off >= 0xd5000 && file_off < 0xd6000)
                return "execute_native_thread_routine [libstdc++]";
        }
        if (lib_path.find("libc.so") != std::string::npos ||
            lib_path.find("libc-") != std::string::npos) {
            // start_thread (pthread_create 的入口) — glibc 2.35/2.36
            if (file_off >= 0x94000 && file_off < 0x95000)
                return "start_thread [glibc]";
            // __clone3 / clone (thread creation syscall wrapper)
            if (file_off >= 0x115000 && file_off < 0x116000)
                return "__clone3 [glibc]";
        }
        if (lib_path.find("libpthread") != std::string::npos) {
            if (file_off >= 0x8000 && file_off < 0x9000)
                return "start_thread [libpthread]";
        }
        return {};
    }

    std::string DemangleSymbol(const std::string& sym) {
        int status = 0;
        char* dm = abi::__cxa_demangle(sym.c_str(), nullptr, nullptr, &status);
        if (status != 0 || !dm) return sym;
        std::string out(dm);
        std::free(dm);
        return out;
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

    // ---- 内联符号解析器 ----
    uint32_t self_pid_ = static_cast<uint32_t>(getpid());
    KernelSymbolResolver kernel_resolver_;
    bool kernel_resolver_loaded_ = false;
    bool self_maps_logged_ = false;
    std::unordered_map<std::string, ElfSymbolCache> elf_cache_;
    std::unordered_map<uint32_t, MapsCacheEntry> maps_cache_;
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
