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
#include <string>
#include <thread>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"
#include "ebpf/loader/feature_probe.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {

// ============================================================================
// IL_DEFINE_SKEL_OPS — 生成 skeleton 操作 traits 结构
//
// 参数：
//   ops_name    — 生成的 traits 结构名
//   skel_prefix — skeleton 前缀（如 bio_latency 对应 bio_latency_bpf）
//   rb_map      — ring buffer map 成员名（如 bio_events）
//   gate_map    — gate map 成员名（如 collection_gate）
//   thr_name    — poll 线程名字符串
//
// 生成的 traits 符合 EbpfSkeletonSource 的 SkelOps concept。
// ============================================================================
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
//   static int RingBufMapFd(skel_type*);  // ring buffer map fd
//   static int GateMapFd(skel_type*);     // gate map fd (-1 if none)
//   static const char* ThreadName();      // poll 线程名
// ============================================================================
template <typename SkelOps>
class EbpfSkeletonSource : public SourcePlugin {
public:
    using skel_type = typename SkelOps::skel_type;

    bool IsPushMode() const override { return true; }
    bool HasBpfProbe() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

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

protected:
    // 子类可覆写：在 open() 之后、load() 之前配置 rodata
    virtual void ConfigureSkeleton(skel_type* /*skel*/) {}

    virtual ring_buffer_sample_fn EventCallback() const = 0;

    skel_type* skel_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool stub_mode_ = false;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    int gate_map_fd_ = -1;

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

}  // namespace illuminator
