// ============================================================================
// Illuminator SourcePlugin — 数据源插件抽象
// ============================================================================
//
// Source（数据源）是流水线中数据的生产者，负责从系统获取原始观测数据。
//
// 支持两种数据采集模式：
// ========================
// 1. Pull（拉取）模式 — 默认
//    流水线按照固定间隔调用 Collect() 主动获取数据。
//    适用场景：轮询 /proc、/sys 文件系统获取系统指标
//    特点：简单可靠，但可能存在采集间隔内的数据盲区
//
// 2. Push（推送）模式 — 覆盖 IsPushMode() 返回 true
//    数据源通过 SetCallback() 设置的回调函数主动推送数据到流水线。
//    适用场景：eBPF RingBuffer/PertEvent 事件驱动的数据采集
//    特点：实时性高，事件发生时立即传递，无轮询延时
//
// 关键方法：
// =========
// - Collect():    Pull 模式专用，收集一批数据返回给流水线
// - SetCallback(): Push 模式专用，设置推送数据的回调函数
// - IsPushMode(): 标识当前使用哪种模式（默认 false = Pull）
// - IntervalMs(): Pull 模式的采集间隔（默认 1000ms = 1秒）
// ============================================================================

#pragma once

#include <functional>
#include "plugin/api/plugin_api.h"

namespace illuminator {

// 数据传递回调函数类型：接收一个 DataBatch shared_ptr
using SourceCallback = std::function<void(DataBatchPtr)>;

class SourcePlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSource; }

    // ---- Push 模式配置 ----
    // 设置数据推送的回调函数（由 Pipeline 在初始化时调用）
    void SetCallback(SourceCallback cb) { callback_ = std::move(cb); }

    // ---- Pull 模式采集 ----
    // 收集一批数据。Pull 模式下由 Pipeline 的采集线程定期调用。
    // 默认实现返回 Unimplemented 错误，提示子类需要覆写此方法。
    virtual StatusOr<DataBatchPtr> Collect() {
        return Status::Error(StatusCode::kUnimplemented, "Pull mode not implemented");
    }

    // ---- 模式切换 ----
    // 返回 true 表示此 Source 工作在 Push（流式）模式
    // 返回 false 表示此 Source 工作在 Pull（拉取）模式
    virtual bool IsPushMode() const { return false; }

    // ---- 采集间隔 ----
    // Pull 模式下的采集间隔（毫秒），默认每秒一次
    virtual uint32_t IntervalMs() const { return 1000; }

protected:
    // 回调函数：子类在 Push 模式下调用此函数将数据推送到流水线
    SourceCallback callback_;
};

}  // namespace illuminator
