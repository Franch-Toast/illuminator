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
#include <string>
#include <unordered_map>
#include "plugin/api/plugin_api.h"

namespace illuminator {

using SourceCallback = std::function<void(DataBatchPtr)>;
using QueryParams = std::unordered_map<std::string, std::string>;

class SourcePlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSource; }

    void SetCallback(SourceCallback cb) { callback_ = std::move(cb); }

    virtual StatusOr<DataBatchPtr> Collect() {
        return Status::Error(StatusCode::kUnimplemented, "Pull mode not implemented");
    }

    virtual bool IsPushMode() const { return false; }
    virtual bool HasBpfProbe() const { return false; }
    virtual uint32_t IntervalMs() const { return 1000; }

    // 反压通知：当下游处理速度跟不上时，Pipeline 会调用此方法。
    // active=true 表示进入反压状态，Source 应降低采集频率或丢弃低优先级数据。
    // active=false 表示反压解除，Source 可恢复正常速率。
    virtual void OnBackpressure(bool /*active*/) {}

    // 插件专属查询 API — 消除 HTTP 处理器中对 dynamic_cast 的依赖。
    // 子类通过重写暴露自定义数据端点（如历史数据、事件列表）。
    virtual StatusOr<std::string> QueryExtra(
        const std::string& /*query*/, const QueryParams& /*params*/) {
        return Status::Error(StatusCode::kUnimplemented, "no extra queries");
    }

    // 运行时不重启流水线即可动态修改过滤参数（如 target_pids）。
    // 当用户在 UI 中切换目标进程时调用。
    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Error(StatusCode::kUnimplemented, "reconfigure not supported");
    }

    // Push 源暂停/恢复接口 — FeatureDriver::Pause()/Resume() 会调用这些方法。
    // Pull 源默认空操作（定时器取消即可暂停采集）。
    // Push 源（IsPushMode()=true）应覆写：关闭 BPF gate、停止 poll 线程等。
    virtual Status PauseCollection() { return Status::Ok(); }
    virtual Status ResumeCollection() { return Status::Ok(); }

    virtual bool IsStub() const { return false; }

protected:
    SourceCallback callback_;
};

}  // namespace illuminator
