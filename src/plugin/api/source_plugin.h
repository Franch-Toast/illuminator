// ============================================================================
// Illuminator SourcePlugin — 数据源插件抽象
// ============================================================================
//
// Source（数据源）是流水线中数据的生产者，负责从系统获取原始观测数据。
//
// 采集模型：
// =========
// TimerWheel 按 IntervalMs() 周期性调用 Collect() 获取数据。
// 所有数据源共用同一条路径，无论底层是 /proc 文件、eBPF ring buffer
// 还是 BPF map 聚合——对 Pipeline 来说只有一个统一的 Collect() 入口。
//
// 关键方法：
// =========
// - Collect():       采集一批数据返回给流水线
// - IntervalMs():    采集间隔（默认 1000ms）
// - PauseCollection() / ResumeCollection(): 暂停/恢复采集
// - Reconfigure():   运行时热更新参数
// ============================================================================

#pragma once

#include <string>
#include <unordered_map>
#include "plugin/api/plugin_api.h"

namespace illuminator {

using QueryParams = std::unordered_map<std::string, std::string>;

// ============================================================================
// MetaStats — eBPF 侧自观测累计计数器
// ============================================================================
// 由 eBPF 探针的 meta_stats PERCPU_ARRAY map 汇总而来，用于在用户态展示
// ring buffer 溢出、PID 过滤丢弃等情况。
struct MetaStats {
    uint64_t total_events = 0;
    uint64_t buffer_full = 0;
    uint64_t dropped = 0;
    uint64_t filtered = 0;
};

class SourcePlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSource; }

    virtual StatusOr<DataBatchPtr> Collect() {
        return Status::Error(StatusCode::kUnimplemented, "Collect not implemented");
    }

    virtual bool HasBpfProbe() const { return false; }
    virtual uint32_t IntervalMs() const { return 1000; }

    virtual MetaStats GetBpfStats() const { return MetaStats{}; }

    // 反压通知：active=true 进入反压，Source 应降频；false 解除。
    virtual void OnBackpressure(bool /*active*/) {}

    virtual StatusOr<std::string> QueryExtra(
        const std::string& /*query*/, const QueryParams& /*params*/) {
        return Status::Error(StatusCode::kUnimplemented, "no extra queries");
    }

    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Error(StatusCode::kUnimplemented, "reconfigure not supported");
    }

    // 暂停/恢复接口 — FeatureDriver::Pause()/Resume() 调用。
    // eBPF 子类覆写：关闭 BPF gate 等。非 eBPF 源默认空操作。
    virtual Status PauseCollection() { return Status::Ok(); }
    virtual Status ResumeCollection() { return Status::Ok(); }

    virtual bool IsStub() const { return false; }
};

}  // namespace illuminator
