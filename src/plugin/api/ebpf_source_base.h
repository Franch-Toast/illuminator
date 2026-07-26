// ============================================================================
// EbpfSourceBase — 统一的 eBPF Source 插件基类
// ============================================================================
//
// 借鉴 Linux 驱动框架核心设计：
//   框架 = 不变逻辑（生命周期管理） + ops 回调表（变化点由驱动填充）
//
// 所有 eBPF Source 插件的公共基类，管理：
//   1. Skeleton 生命周期 (open → configure → load → attach → destroy)
//   2. Stub 模式（内核不支持 BPF 时优雅降级）
//   3. MetaStats 自观测统计
//   4. 状态管理 (running / paused / stub)
//   5. Start/Stop/Pause/Resume 标准流程
//   6. 统一的 Collect() 入口：自动消费 ring buffer + 读取 BPF maps
//
// 子类只需填充 hooks（类似 Linux 驱动填写 xxx_ops）：
//   - MakeSkelCallbacks()    — 必须：skeleton 操作函数指针
//   - OnConfigureRodata()    — 可选：load 前写 rodata
//   - OnConfigureMaps()      — 可选：load 后写 BPF maps
//   - OnPostAttach()         — 可选：attach 后额外挂载（如 perf_event）
//   - OnPreDestroy()         — 可选：停止时清理额外资源
//   - GetEventCallback()     — 可选：ring buffer 事件回调（返回非空则消费事件）
//   - CollectFromMaps()      — 可选：从 BPF maps 读聚合数据
//
// PID/Comm 过滤、perf_event 管理等能力通过 bpf_util 工具函数提供，
// 子类在自己的 hook 中按需调用，基类不强制使用。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "ebpf_common/loader/bpf_stats_reader.h"
#include "ebpf_common/loader/feature_probe.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {

// ============================================================================
// IL_SKEL_CALLBACKS — 为子类生成 MakeSkelCallbacks() 和 skel_t 类型别名
// ============================================================================
//
// 用法：
//   class MySource : public EbpfSourceBase {
//       IL_SKEL_CALLBACKS(my_probe_sk);
//       ...
//   };
//
// 展开后提供：
//   - skel_t       类型别名（指向 skeleton 结构体）
//   - skel()       获取类型安全的 skeleton 指针
//   - MakeSkelCallbacks()  返回 SkelCallbacks 函数指针集
// ============================================================================
#define IL_SKEL_CALLBACKS(skel_prefix)                                        \
    using skel_t = struct skel_prefix##_bpf;                                  \
    skel_t* skel() const {                                                    \
        return static_cast<skel_t*>(raw_skel_);                               \
    }                                                                         \
    SkelCallbacks MakeSkelCallbacks() const override {                        \
        return {                                                              \
            .open    = []() -> void* {                                        \
                return static_cast<void*>(skel_prefix##_bpf__open());         \
            },                                                                \
            .load    = [](void* s) -> int {                                   \
                return skel_prefix##_bpf__load(                               \
                    static_cast<skel_prefix##_bpf*>(s));                      \
            },                                                                \
            .attach  = [](void* s) -> int {                                   \
                return skel_prefix##_bpf__attach(                             \
                    static_cast<skel_prefix##_bpf*>(s));                      \
            },                                                                \
            .destroy = [](void* s) {                                          \
                skel_prefix##_bpf__destroy(                                   \
                    static_cast<skel_prefix##_bpf*>(s));                      \
            },                                                                \
        };                                                                    \
    }

class EbpfSourceBase : public SourcePlugin {
public:
    // ================================================================
    // Skeleton 操作回调表（类似 Linux struct xxx_ops）
    // ================================================================
    struct SkelCallbacks {
        void* (*open)();
        int   (*load)(void* skel);
        int   (*attach)(void* skel);
        void  (*destroy)(void* skel);
    };

    // ================================================================
    // SourcePlugin 接口实现
    // ================================================================

    bool HasBpfProbe() const override { return !stub_mode_; }
    bool IsStub() const override { return stub_mode_; }

    MetaStats GetBpfStats() const override {
        return ReadBpfMetaStats(meta_stats_fd_);
    }

    // ================================================================
    // Start — 标准化启动序列
    // ================================================================
    //
    //  1. 内核特性探测 → stub mode if unsupported
    //  2. callbacks.open()
    //  3. OnConfigureRodata(skel)     ← hook
    //  4. callbacks.load(skel)
    //  5. OnConfigureMaps(skel)       ← hook
    //  6. callbacks.attach(skel)
    //  7. OnPostAttach(skel)          ← hook
    //  8. 初始化 ring buffer (if fd >= 0)
    //  9. running_ = true
    //
    Status Start() override {
        if (!features_.has_bpf_tracing) {
            IL_WARN("{}: kernel lacks BPF tracing, stub mode", Name());
            stub_mode_ = true;
            return Status::Ok();
        }

        auto cbs = MakeSkelCallbacks();

        raw_skel_ = cbs.open();
        if (!raw_skel_) {
            IL_WARN("{}: skeleton open failed, stub mode", Name());
            stub_mode_ = true;
            return Status::Ok();
        }

        OnConfigureRodata(raw_skel_);

        int err = cbs.load(raw_skel_);
        if (err) {
            cbs.destroy(raw_skel_);
            raw_skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": load failed (err=" +
                std::to_string(err) + ")");
        }

        OnConfigureMaps(raw_skel_);

        err = cbs.attach(raw_skel_);
        if (err) {
            cbs.destroy(raw_skel_);
            raw_skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                std::string(Name()) + ": attach failed (err=" +
                std::to_string(err) + ")");
        }

        auto post_status = OnPostAttach(raw_skel_);
        if (!post_status.ok()) {
            cbs.destroy(raw_skel_);
            raw_skel_ = nullptr;
            return post_status;
        }

        if (ringbuf_fd_ >= 0) {
            auto event_cb = GetEventCallback();
            if (event_cb) {
                ring_buf_ = ring_buffer__new(ringbuf_fd_, event_cb, this, nullptr);
                if (!ring_buf_) {
                    cbs.destroy(raw_skel_);
                    raw_skel_ = nullptr;
                    return Status::Error(StatusCode::kInternal,
                        std::string(Name()) + ": ring buffer init failed");
                }
            }
        }

        running_.store(true, std::memory_order_release);
        paused_.store(false, std::memory_order_release);

        SetGate(true);
        IL_INFO("{}: started", Name());
        return Status::Ok();
    }

    // ================================================================
    // Stop — 标准化停止序列
    // ================================================================
    Status Stop() override {
        SetGate(false);
        running_.store(false, std::memory_order_release);

        OnPreDestroy();

        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }

        if (raw_skel_) {
            MakeSkelCallbacks().destroy(raw_skel_);
            raw_skel_ = nullptr;
        }

        gate_fd_ = -1;
        meta_stats_fd_ = -1;
        ringbuf_fd_ = -1;

        IL_INFO("{}: stopped", Name());
        return Status::Ok();
    }

    // ================================================================
    // PauseCollection / ResumeCollection
    // ================================================================
    Status PauseCollection() override {
        if (stub_mode_) return Status::Ok();
        SetGate(false);
        paused_.store(true, std::memory_order_release);
        OnPause();
        IL_INFO("{}: paused", Name());
        return Status::Ok();
    }

    Status ResumeCollection() override {
        if (stub_mode_) return Status::Ok();
        paused_.store(false, std::memory_order_release);
        OnResume();
        SetGate(true);
        IL_INFO("{}: resumed", Name());
        return Status::Ok();
    }

    // ================================================================
    // Collect — 统一的数据采集入口
    // ================================================================
    // TimerWheel 定时调用。自动处理两种数据源：
    //   1. ring buffer 事件（如果 GetEventCallback() 返回非空）
    //   2. BPF map 聚合数据（如果子类覆写了 CollectFromMaps()）
    // 两种可以单独使用，也可以在同一个 Source 中共存。
    StatusOr<DataBatchPtr> Collect() override {
        if (stub_mode_) {
            return std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        }

        DataBatchPtr event_batch;
        if (ring_buf_) {
            if (GetEventCallback()) {
                pending_batch_ = MakeEventBatch();
                ring_buffer__consume(ring_buf_);
                event_batch = std::move(pending_batch_);
                pending_batch_ = nullptr;
            } else {
                ring_buffer__consume(ring_buf_);
            }
        }

        auto map_result = CollectFromMaps();

        if (event_batch && !event_batch->Empty()) {
            return event_batch;
        }
        return map_result;
    }

protected:
    // ================================================================
    // 子类必须实现
    // ================================================================

    virtual SkelCallbacks MakeSkelCallbacks() const = 0;

    // ================================================================
    // Skeleton 配置 Hooks — 子类可选覆写
    // ================================================================

    // Phase 1: open() 之后、load() 之前 — 写入 rodata
    virtual void OnConfigureRodata(void* /*skel*/) {}

    // Phase 2: load() 之后、attach() 之前 — 配置 BPF maps
    // 子类应在此设置 ringbuf_fd_、gate_fd_、meta_stats_fd_
    virtual void OnConfigureMaps(void* /*skel*/) {}

    // Phase 3: attach() 之后 — 创建额外挂载点（如 perf_event）
    virtual Status OnPostAttach(void* /*skel*/) { return Status::Ok(); }

    // Phase 4: 停止时 — 清理子类创建的额外资源
    virtual void OnPreDestroy() {}

    // ================================================================
    // Data Hooks — 子类至少实现一个
    // ================================================================

    // ring buffer 事件回调。返回非空时，Collect() 会消费 ring buffer
    // 并将事件打包到 pending_batch_ 中。返回 nullptr 表示不消费事件。
    virtual ring_buffer_sample_fn GetEventCallback() const { return nullptr; }

    // 创建 ring buffer 事件的批量容器（子类可覆写以指定 DataBatch 类型）
    virtual DataBatchPtr MakeEventBatch() {
        return std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    }

    // 从 BPF maps 读取聚合数据（子类可覆写以读取 HashMap 等）
    virtual StatusOr<DataBatchPtr> CollectFromMaps() {
        return std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    }

    // ================================================================
    // Runtime Hooks — 子类可选覆写
    // ================================================================

    virtual void OnPause() {}
    virtual void OnResume() {}

    // ================================================================
    // Map fd 设置器 — 子类在 OnConfigureMaps() 中调用
    // ================================================================

    void SetRingBufFd(int fd) { ringbuf_fd_ = fd; }
    void SetGateFd(int fd) { gate_fd_ = fd; }
    void SetMetaStatsFd(int fd) { meta_stats_fd_ = fd; }

    // ================================================================
    // 状态访问
    // ================================================================

    void* raw_skel_ = nullptr;
    DataBatchPtr pending_batch_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool stub_mode_ = false;

private:
    KernelFeatures features_ = ProbeKernelFeatures();
    struct ring_buffer* ring_buf_ = nullptr;
    int ringbuf_fd_ = -1;
    int gate_fd_ = -1;
    int meta_stats_fd_ = -1;

    void SetGate(bool enabled) {
        if (gate_fd_ < 0) return;
        uint32_t key = 0;
        uint32_t val = enabled ? 1 : 0;
        bpf_map_update_elem(gate_fd_, &key, &val, BPF_ANY);
    }
};

}  // namespace illuminator
