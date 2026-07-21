// ============================================================================
// EbpfSkeletonSource — eBPF Skeleton-based Push Source Template
// ============================================================================
//
// 替代 EbpfRingBufferSource，使用 bpftool gen skeleton 生成的类型安全骨架。
//
// 优势（相比旧的 raw libbpf + 文件路径加载）：
//   - BPF 字节码嵌入二进制，无需外部 .bpf.o 文件
//   - 编译期类型安全的 map/program 访问，消除运行时字符串查找
//   - 支持 const volatile rodata 配置（open → configure → load）
//   - 代码更简洁，无需 BpfProgramManager 间接层
//
// 子类只需提供：
//   1. SkelOps traits（用 IL_DEFINE_SKEL_OPS 宏生成）
//   2. EventCallback() + HandleEvent() 事件回调
//   3. 可选：ConfigureSkeleton() 设置 rodata
//
// 示例用法见 ebpf_io_monitor.h
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "core/engine/pid_manager.h"
#include "core/threading/thread_util.h"
#include "ebpf/loader/bpf_stats_reader.h"
#include "ebpf/loader/feature_probe.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {

// ============================================================================
// IL_DEFINE_SKEL_OPS / IL_DEFINE_SKEL_OPS_WITH_META — 生成 skeleton 操作 traits
//
// 参数：
//   ops_name       — 生成的 traits 结构名
//   skel_prefix    — skeleton 前缀（如 bio_latency 对应 bio_latency_bpf）
//   rb_map         — ring buffer map 成员名（如 bio_events）
//   gate_map       — gate map 成员名（如 collection_gate）
//   thr_name       — poll 线程名字符串
//   meta_stats_map — meta_stats PERCPU_ARRAY map 成员名（可选，见下方）
//
// 生成的 traits 符合 EbpfSkeletonSource 的 SkelOps concept。
//
// 兼容性：
//   - IL_DEFINE_SKEL_OPS_WITH_META 接受 meta_stats_map 参数并导出该 map 的 fd。
//   - IL_DEFINE_SKEL_OPS 保持原来的 5 参数签名，MetaStatsMapFd() 固定返回 -1，
//     供不暴露 meta_stats 的旧版/测试 BPF 程序使用。
// ============================================================================
#define IL_DEFINE_SKEL_OPS_WITH_META(ops_name, skel_prefix, rb_map, gate_map, thr_name, meta_stats_map) \
struct ops_name { \
    using skel_type = struct skel_prefix##_bpf; \
    static skel_type* Open() { return skel_prefix##_bpf__open(); } \
    static int Load(skel_type* s) { return skel_prefix##_bpf__load(s); } \
    static int Attach(skel_type* s) { return skel_prefix##_bpf__attach(s); } \
    static void Destroy(skel_type* s) { skel_prefix##_bpf__destroy(s); } \
    static int RingBufMapFd(skel_type* s) { \
        return bpf_map__fd(s->maps.rb_map); \
    } \
    static int GateMapFd(skel_type* s) { \
        return bpf_map__fd(s->maps.gate_map); \
    } \
    static int MetaStatsMapFd(skel_type* s) { \
        return bpf_map__fd(s->maps.meta_stats_map); \
    } \
    static const char* ThreadName() { return thr_name; } \
}

#define IL_DEFINE_SKEL_OPS(ops_name, skel_prefix, rb_map, gate_map, thr_name) \
struct ops_name { \
    using skel_type = struct skel_prefix##_bpf; \
    static skel_type* Open() { return skel_prefix##_bpf__open(); } \
    static int Load(skel_type* s) { return skel_prefix##_bpf__load(s); } \
    static int Attach(skel_type* s) { return skel_prefix##_bpf__attach(s); } \
    static void Destroy(skel_type* s) { skel_prefix##_bpf__destroy(s); } \
    static int RingBufMapFd(skel_type* s) { \
        return bpf_map__fd(s->maps.rb_map); \
    } \
    static int GateMapFd(skel_type* s) { \
        return bpf_map__fd(s->maps.gate_map); \
    } \
    static int MetaStatsMapFd(skel_type* /*s*/) { return -1; } \
    static const char* ThreadName() { return thr_name; } \
}

// ============================================================================
// EbpfSkeletonSource<SkelOps> — Skeleton Push Source 基类
//
// SkelOps 必须提供：
//   using skel_type = ...;            // skeleton 结构类型
//   static skel_type* Open();         // __open
//   static int Load(skel_type*);      // __load
//   static int Attach(skel_type*);    // __attach
//   static void Destroy(skel_type*);  // __destroy
//   static int RingBufMapFd(skel_type*);    // ring buffer map fd
//   static int GateMapFd(skel_type*);       // gate map fd (-1 if none)
//   static int MetaStatsMapFd(skel_type*);  // meta_stats map fd (-1 if none)
//   static const char* ThreadName();        // poll 线程名
// ============================================================================
template <typename SkelOps>
class EbpfSkeletonSource : public SourcePlugin {
public:
    using skel_type = typename SkelOps::skel_type;

    bool IsPushMode() const override { return true; }
    bool HasBpfProbe() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    MetaStats GetBpfStats() const override {
        return ReadBpfMetaStats(meta_stats_fd_);
    }

    Status Start() override {
        if (!features_.has_bpf_tracing) {
            IL_WARN("{}: kernel lacks BPF tracing, stub mode", Name());
            stub_mode_ = true;
            return Status::Ok();
        }

        skel_ = SkelOps::Open();
        if (!skel_) {
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": failed to open BPF skeleton");
        }

        ConfigureSkeleton(skel_);

        int err = SkelOps::Load(skel_);
        if (err) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": failed to load BPF skeleton (err=" +
                std::to_string(err) + ")");
        }

        err = SkelOps::Attach(skel_);
        if (err) {
            SkelOps::Destroy(skel_);
            skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": failed to attach BPF programs (err=" +
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
                std::string(Name()) + ": failed to create ring buffer");
        }

        gate_map_fd_ = SkelOps::GateMapFd(skel_);
        meta_stats_fd_ = SkelOps::MetaStatsMapFd(skel_);

        running_ = true;
        paused_ = false;
        StartPollThread();
        SetBpfGate(true);

        IL_INFO("{}: started (skeleton mode)", Name());
        return Status::Ok();
    }

    Status Stop() override {
        SetBpfGate(false);
        running_ = false;
        paused_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) { ring_buffer__free(ring_buf_); ring_buf_ = nullptr; }
        if (skel_) { SkelOps::Destroy(skel_); skel_ = nullptr; }
        return Status::Ok();
    }

    Status PauseCollection() override {
        if (stub_mode_) return Status::Ok();
        SetBpfGate(false);
        paused_ = true;
        if (poll_thread_.joinable()) poll_thread_.join();
        IL_INFO("{}: paused (BPF gate closed)", Name());
        return Status::Ok();
    }

    Status ResumeCollection() override {
        if (stub_mode_) return Status::Ok();
        paused_ = false;
        StartPollThread();
        SetBpfGate(true);
        IL_INFO("{}: resumed (BPF gate opened)", Name());
        return Status::Ok();
    }

    // ========================================================================
    // Reconfigure — 运行时更新 PID 过滤列表
    // ========================================================================
    // 检查 params 中的 "target_pids" 和 "target_process_names"，
    // 通过 PidManager 动态更新 BPF PID 过滤 map。
    // 如果 PidManager 未初始化（pid_map_fd 未设置），则返回 kUnimplemented。
    Status Reconfigure(const ConfigValue& params) override {
        if (!pid_manager_) {
            return Status::Error(StatusCode::kUnimplemented,
                std::string(Name()) + ": PidManager not initialized");
        }

        bool changed = false;

        // 解析 target_pids（逗号分隔的数字列表）
        auto pid_str = params["target_pids"].AsString("");
        if (!pid_str.empty()) {
            auto pids = ParseCommaSeparated<uint32_t>(pid_str);
            std::vector<int32_t> pid_list(pids.begin(), pids.end());
            pid_manager_->SetTargetPids(pid_list);
            changed = true;
        }

        // 解析 target_process_names（逗号分隔的进程名列表）
        auto name_str = params["target_process_names"].AsString("");
        if (!name_str.empty()) {
            auto names = ParseCommaSeparated<std::string>(name_str);
            pid_manager_->SetTargetProcessNames(names);
            changed = true;
        }

        // 解析 add_pid / remove_pid（单个 PID 增删）
        auto add_pid_val = params["add_pid"].AsString("");
        if (!add_pid_val.empty()) {
            try {
                int32_t pid = static_cast<int32_t>(std::stoi(add_pid_val));
                pid_manager_->AddPid(pid);
                changed = true;
            } catch (...) {}
        }

        auto remove_pid_val = params["remove_pid"].AsString("");
        if (!remove_pid_val.empty()) {
            try {
                int32_t pid = static_cast<int32_t>(std::stoi(remove_pid_val));
                pid_manager_->RemovePid(pid);
                changed = true;
            } catch (...) {}
        }

        if (changed) {
            IL_INFO("{}: Reconfigure applied (active_pids={})",
                    Name(), pid_manager_->GetActivePids().size());
        }
        return Status::Ok();
    }

protected:
    // 子类可覆写：在 open() 之后、load() 之前配置 rodata
    virtual void ConfigureSkeleton(skel_type* /*skel*/) {}

    virtual ring_buffer_sample_fn EventCallback() const = 0;

    // ---- PidManager 集成 ----
    // 子类在 Start() 成功后调用此方法设置 PID map fd。
    // 调用后 PidManager 被初始化，Reconfigure() 可接受 PID 过滤参数。
    void SetPidMapFd(int fd) {
        if (fd < 0) return;
        pid_manager_ = std::make_unique<PidManager>(fd, Name());
        IL_INFO("{}: PidManager initialized (map_fd={})", Name(), fd);
    }

    // 获取 PidManager（可能为 nullptr，如果 SetPidMapFd 未调用）
    PidManager* GetPidManager() { return pid_manager_.get(); }

    skel_type* skel_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool stub_mode_ = false;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    int gate_map_fd_ = -1;
    int meta_stats_fd_ = -1;
    std::unique_ptr<PidManager> pid_manager_;

private:
    KernelFeatures features_ = ProbeKernelFeatures();

    void SetBpfGate(bool enabled) {
        if (gate_map_fd_ < 0) return;
        uint32_t key = 0;
        uint32_t val = enabled ? 1 : 0;
        bpf_map_update_elem(gate_map_fd_, &key, &val, BPF_ANY);
    }

    void StartPollThread() {
        poll_thread_ = std::thread([this] {
            SetThreadName(SkelOps::ThreadName());
            while (running_ && !paused_)
                ring_buffer__poll(ring_buf_, 100);
        });
    }
};

// 类型别名：EbpfSkeletonPushSource 是 EbpfSkeletonSource 的显式名称，
// 与 EbpfSkeletonPullSource（Pull 聚合模式）形成对称的命名体系。
template <typename SkelOps>
using EbpfSkeletonPushSource = EbpfSkeletonSource<SkelOps>;

}  // namespace illuminator
