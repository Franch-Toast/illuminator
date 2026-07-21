// ============================================================================
// EbpfSkeletonPullSource — eBPF Skeleton-based Pull (Aggregation) Source Template
// ============================================================================
//
// 与 EbpfSkeletonSource（Push 模式）互补，为 Pull 模式的 eBPF 探针提供统一模板。
//
// Push vs Pull 模式的区别：
// ==========================
// Push（EbpfSkeletonSource）：
//   - BPF 层通过 ring buffer 发射完整事件
//   - 用户态 poll 线程接收事件后直接推送给 pipeline callback
//   - IsPushMode() = true
//   - 适用场景：io_monitor, sched_tracer, net_tracer
//
// Pull（本模板）：
//   - BPF 层在内核 map 中聚合数据（如按 stack+pid 累计时长/计数）
//   - Ring buffer 仅作为低频信号通道（通知用户态"有新数据"）
//   - 用户态收到信号后批量读取 BPF stats map → 缓存到 cached_batch_
//   - Pipeline TimerWheel 定期调用 Collect() 取走缓存
//   - IsPushMode() = false
//   - 适用场景：offcpu_profiler, cpu_profiler, sched_analyzer
//
// 模板提供的标准化能力：
// =======================
// 1. Skeleton 生命周期（open → configure → load → attach → destroy）
// 2. 多 bit 配置 gate（offcpu_cfg 风格的 bit-flag 控制）
// 3. 延迟启动（启动后等待系统稳定再开启 BPF 采集）
// 4. PID/进程名/PID Namespace 过滤管理
// 5. Ring buffer poll 线程（用于接收信号）
// 6. 缓存管理（cached_batch_ + Collect()）
// 7. Pause/Resume 通过 enable bit 控制
// 8. Reconfigure 动态更新过滤器
//
// 子类需提供：
// ============
// 1. SkelOps traits（用 IL_DEFINE_SKEL_OPS / IL_DEFINE_SKEL_OPS_WITH_META 宏）
// 2. EventCallback() — ring buffer 信号回调
// 3. ReadAndClearStats() — 批量读取 BPF stats map 并填充缓存
// 4. MakeEmptyBatch() — 创建空的 DataBatch（指定类型）
// 5. IsCacheEmpty() — 判断缓存是否为空
// 6. GetXxxMapFd() — 提供 skeleton 中各 BPF map 的 fd
//
// 可选覆写：
// ==========
// - ConfigureSkeleton() — open() 后 load() 前写入 rodata
// - ConfigureAfterLoad() — load() 后 attach() 前配置 maps
// - InitExtra() — Init() 中解析子类专有配置
// - OnReconfigureExtra() — Reconfigure 中处理子类专有参数
//
// 示例用法见 offcpu_profiler.h
// ============================================================================

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "core/threading/thread_util.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_stats_reader.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {

// ============================================================================
// EbpfSkeletonPullSource<SkelOps> — Skeleton Pull Source 基类
//
// SkelOps 必须提供（与 Push 模板相同的 concept）：
//   using skel_type = ...;
//   static skel_type* Open();
//   static int Load(skel_type*);
//   static int Attach(skel_type*);
//   static void Destroy(skel_type*);
//   static int RingBufMapFd(skel_type*);
//   static int GateMapFd(skel_type*);       // Pull 模式下作为 cfg map（多 bit）
//   static int MetaStatsMapFd(skel_type*);
//   static const char* ThreadName();
// ============================================================================
template <typename SkelOps>
class EbpfSkeletonPullSource : public SourcePlugin {
public:
    using skel_type = typename SkelOps::skel_type;

    bool IsPushMode() const override { return false; }
    bool HasBpfProbe() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    MetaStats GetBpfStats() const override {
        return ReadBpfMetaStats(meta_stats_fd_);
    }

    // ========================================================================
    // Init — 解析通用 Pull 配置 + 子类专有配置
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);
        start_delay_seconds_ =
            static_cast<int>(config["start_delay_seconds"].AsInt(3));

        target_pids_ = ParseCommaSeparated<uint32_t>(
            config["target_pids"].AsString(""));

        auto comms_str = config["target_process_names"].AsString(
            config["target_comms"].AsString(""));
        target_comms_.clear();
        if (!comms_str.empty()) {
            auto parts = ParseCommaSeparated<std::string>(comms_str);
            for (auto& s : parts)
                if (!s.empty()) target_comms_.push_back(s);
        }

        return InitExtra(config);
    }

    // ========================================================================
    // Collect — Pull 模式核心：返回缓存的 DataBatch
    // ========================================================================
    StatusOr<DataBatchPtr> Collect() override {
        if (!stub_mode_) {
            bool need_fallback = false;
            {
                std::lock_guard<std::mutex> lk(cache_mu_);
                need_fallback = IsCacheEmpty();
            }
            if (need_fallback) {
                ReadAndClearStats();
            }
        }

        std::lock_guard<std::mutex> lk(cache_mu_);
        auto result = cached_batch_
            ? cached_batch_
            : MakeEmptyBatch();
        cached_batch_ = MakeEmptyBatch();
        return result;
    }

    // ========================================================================
    // Start — 加载 eBPF Skeleton 并启动采集
    // ========================================================================
    Status Start() override {
        skel_ = SkelOps::Open();
        if (!skel_) {
            IL_WARN("{}: skeleton open failed; stub mode", Name());
            stub_mode_ = true;
            running_.store(false);
            return Status::Ok();
        }

        ConfigureSkeleton(skel_);

        int err = SkelOps::Load(skel_);
        if (err) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": load failed (err=" +
                std::to_string(err) + ")");
        }

        cfg_fd_ = SkelOps::GateMapFd(skel_);
        meta_stats_fd_ = SkelOps::MetaStatsMapFd(skel_);
        stacks_fd_ = GetStacksMapFd();
        pids_fd_ = GetTargetPidsMapFd();
        comms_fd_ = GetTargetCommsMapFd();
        pidns_fd_ = GetPidnsMapFd();

        if (cfg_fd_ >= 0) {
            cfg_flags_base_ = BuildCfgFlags();
            WriteCfgFlags(cfg_flags_base_);
            IL_INFO("{}: cfg_flags=0x{:x} (user_stacks={}, kernel_stacks={}, "
                    "pid_filter={}, comm_filter={}) — enable after {}s delay",
                    Name(), cfg_flags_base_, user_stacks_, kernel_stacks_,
                    !target_pids_.empty(), !target_comms_.empty(),
                    start_delay_seconds_);
        }

        ConfigurePidNamespace();
        WritePidFilter();
        WriteCommFilter();
        ConfigureAfterLoad(skel_);

        err = SkelOps::Attach(skel_);
        if (err) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": attach failed (err=" +
                std::to_string(err) + ")");
        }

        int rb_fd = SkelOps::RingBufMapFd(skel_);
        if (rb_fd < 0) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": ring buffer map not found");
        }

        ring_buf_ = ring_buffer__new(rb_fd, EventCallback(), this, nullptr);
        if (!ring_buf_) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": ring buffer init failed");
        }

        running_.store(true);
        StartPollThread();
        StartDelayThread();

        IL_INFO("{}: started (pull mode, delay={}s, pids={}, comms={})",
                Name(), start_delay_seconds_,
                target_pids_.size(), target_comms_.size());
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止采集，释放所有 eBPF 资源
    // ========================================================================
    Status Stop() override {
        running_.store(false);
        if (delay_thread_.joinable()) delay_thread_.join();
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) { ring_buffer__free(ring_buf_); ring_buf_ = nullptr; }
        stacks_fd_ = cfg_fd_ = pids_fd_ = comms_fd_ = pidns_fd_ = -1;
        meta_stats_fd_ = -1;
        if (skel_) { SkelOps::Destroy(skel_); skel_ = nullptr; }
        return Status::Ok();
    }

    // ========================================================================
    // PauseCollection / ResumeCollection — 通过 cfg bit4 控制 BPF gate
    // ========================================================================
    Status PauseCollection() override {
        if (stub_mode_ || cfg_fd_ < 0) return Status::Ok();
        WriteCfgFlags(cfg_flags_base_ & ~kEnableBit);
        IL_INFO("{}: paused (cleared enable bit)", Name());
        return Status::Ok();
    }

    Status ResumeCollection() override {
        if (stub_mode_ || cfg_fd_ < 0) return Status::Ok();
        WriteCfgFlags(cfg_flags_base_ | kEnableBit);
        IL_INFO("{}: resumed (set enable bit)", Name());
        return Status::Ok();
    }

    // ========================================================================
    // Reconfigure — 运行时更新 PID/进程名过滤器
    // ========================================================================
    Status Reconfigure(const ConfigValue& params) override {
        auto pid_str = params["target_pids"].AsString("");
        auto comm_str = params["target_process_names"].AsString(
            params["target_comms"].AsString(""));

        target_pids_ = ParseCommaSeparated<uint32_t>(pid_str);

        target_comms_.clear();
        if (!comm_str.empty()) {
            auto parts = ParseCommaSeparated<std::string>(comm_str);
            for (auto& s : parts)
                if (!s.empty()) target_comms_.push_back(s);
        }

        ClearAndRewritePidMap();
        ClearAndRewriteCommMap();
        ConfigurePidNamespace();

        if (cfg_fd_ >= 0) {
            cfg_flags_base_ = BuildCfgFlags();
            WriteCfgFlags(cfg_flags_base_ | kEnableBit);
        }

        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            cached_batch_ = MakeEmptyBatch();
        }

        auto extra_st = OnReconfigureExtra(params);

        IL_INFO("{}: reconfigured (pids={}, comms={})",
                Name(), target_pids_.size(), target_comms_.size());
        return extra_st;
    }

protected:
    // ---- 子类必须实现 ----
    virtual ring_buffer_sample_fn EventCallback() const = 0;
    virtual void ReadAndClearStats() = 0;
    virtual DataBatchPtr MakeEmptyBatch() const = 0;
    virtual bool IsCacheEmpty() const = 0;

    // ---- 子类可选覆写 ----
    virtual void ConfigureSkeleton(skel_type* /*skel*/) {}
    virtual void ConfigureAfterLoad(skel_type* /*skel*/) {}
    virtual Status InitExtra(const ConfigValue& /*config*/) {
        return Status::Ok();
    }
    virtual Status OnReconfigureExtra(const ConfigValue& /*params*/) {
        return Status::Ok();
    }

    // ---- Map FD 提供者（子类覆写以暴露 skeleton 中的 BPF maps）----
    virtual int GetStacksMapFd() const { return -1; }
    virtual int GetTargetPidsMapFd() const { return -1; }
    virtual int GetTargetCommsMapFd() const { return -1; }
    virtual int GetPidnsMapFd() const { return -1; }

    // ---- 状态 ----
    skel_type* skel_ = nullptr;
    std::atomic<bool> running_{false};
    bool stub_mode_ = false;
    int stacks_fd_ = -1;
    int meta_stats_fd_ = -1;
    int cfg_fd_ = -1;
    int pids_fd_ = -1;
    int comms_fd_ = -1;
    int pidns_fd_ = -1;
    uint32_t cfg_flags_base_ = 0;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    std::thread delay_thread_;
    mutable std::mutex cache_mu_;
    DataBatchPtr cached_batch_;

    // ---- 配置 ----
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    int start_delay_seconds_ = 3;
    std::vector<uint32_t> target_pids_;
    std::vector<std::string> target_comms_;

private:
    // cfg map bit 布局：bit0=user_stacks, bit1=kernel_stacks,
    // bit2=pid_filter, bit3=comm_filter, bit4=global_enable
    static constexpr uint32_t kEnableBit = 16u;

    uint32_t BuildCfgFlags() const {
        return (user_stacks_ ? 1u : 0u)
             | (kernel_stacks_ ? 2u : 0u)
             | (!target_pids_.empty() ? 4u : 0u)
             | (!target_comms_.empty() ? 8u : 0u);
    }

    void WriteCfgFlags(uint32_t flags) {
        if (cfg_fd_ < 0) return;
        uint32_t k = 0;
        bpf_map_update_elem(cfg_fd_, &k, &flags, BPF_ANY);
    }

    // ---- PID Namespace 配置 ----
    void ConfigurePidNamespace() {
        if (pidns_fd_ < 0) return;
        struct stat st = {};
        if (::stat("/proc/self/ns/pid", &st) != 0) {
            IL_WARN("{}: stat(/proc/self/ns/pid) failed: {}",
                    Name(), strerror(errno));
            return;
        }
        il_pidns_config cfg = {};
        cfg.dev = static_cast<uint64_t>(st.st_dev);
        cfg.ino = static_cast<uint64_t>(st.st_ino);
        uint32_t k = 0;
        if (bpf_map_update_elem(pidns_fd_, &k, &cfg, BPF_ANY) == 0) {
            IL_INFO("{}: pidns configured (dev={}, ino={})",
                    Name(), cfg.dev, cfg.ino);
        }
    }

    // ---- PID 过滤 map 写入 ----
    void WritePidFilter() {
        if (pids_fd_ < 0 || target_pids_.empty()) return;
        uint8_t val = 1;
        for (uint32_t pid : target_pids_)
            bpf_map_update_elem(pids_fd_, &pid, &val, BPF_ANY);
        IL_INFO("{}: loaded {} target PIDs", Name(), target_pids_.size());
    }

    void WriteCommFilter() {
        if (comms_fd_ < 0 || target_comms_.empty()) return;
        uint8_t val = 1;
        for (const auto& comm : target_comms_) {
            char key[16] = {};
            std::memcpy(key, comm.c_str(),
                        std::min(comm.size(), sizeof(key) - 1));
            bpf_map_update_elem(comms_fd_, key, &val, BPF_ANY);
        }
        IL_INFO("{}: loaded {} target comms", Name(), target_comms_.size());
    }

    // ---- 清空并重写 BPF filter maps ----
    void ClearAndRewritePidMap() {
        if (pids_fd_ < 0) return;
        uint32_t cur{}, next{};
        std::vector<uint32_t> old_keys;
        int err = bpf_map_get_next_key(pids_fd_, nullptr, &cur);
        while (err == 0) {
            old_keys.push_back(cur);
            err = bpf_map_get_next_key(pids_fd_, &cur, &next);
            cur = next;
        }
        for (auto k : old_keys)
            bpf_map_delete_elem(pids_fd_, &k);

        uint8_t val = 1;
        for (uint32_t pid : target_pids_)
            bpf_map_update_elem(pids_fd_, &pid, &val, BPF_ANY);
    }

    void ClearAndRewriteCommMap() {
        if (comms_fd_ < 0) return;
        char cur_key[16] = {}, next_key[16] = {};
        std::vector<std::string> old_comms;
        int err = bpf_map_get_next_key(comms_fd_, nullptr, cur_key);
        while (err == 0) {
            old_comms.emplace_back(cur_key,
                strnlen(cur_key, sizeof(cur_key)));
            err = bpf_map_get_next_key(comms_fd_, cur_key, next_key);
            std::memcpy(cur_key, next_key, 16);
        }
        for (auto& c : old_comms) {
            char k[16] = {};
            std::memcpy(k, c.c_str(), std::min(c.size(), sizeof(k) - 1));
            bpf_map_delete_elem(comms_fd_, k);
        }

        uint8_t val = 1;
        for (const auto& comm : target_comms_) {
            char key[16] = {};
            std::memcpy(key, comm.c_str(),
                        std::min(comm.size(), sizeof(key) - 1));
            bpf_map_update_elem(comms_fd_, key, &val, BPF_ANY);
        }
    }

    // ---- 线程管理 ----
    void StartPollThread() {
        poll_thread_ = std::thread([this] {
            SetThreadName(SkelOps::ThreadName());
            while (running_.load()) {
                int err = ring_buffer__poll(ring_buf_, 100);
                if (err < 0 && err != -EINTR)
                    IL_WARN("{}: ringbuf poll err {}", Name(), err);
            }
        });
    }

    void StartDelayThread() {
        delay_thread_ = std::thread([this] {
            SetThreadName(std::string(SkelOps::ThreadName()) + "-dly");
            for (int i = 0; i < start_delay_seconds_ && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (running_.load() && cfg_fd_ >= 0) {
                uint32_t flags = cfg_flags_base_ | kEnableBit;
                WriteCfgFlags(flags);
                IL_INFO("{}: enabled after {}s delay (flags=0x{:x})",
                        Name(), start_delay_seconds_, flags);
            }
        });
    }
};

}  // namespace illuminator
