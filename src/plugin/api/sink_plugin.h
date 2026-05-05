// ============================================================================
// Illuminator SinkPlugin — 数据出口插件抽象
// ============================================================================
//
// Sink（数据出口）是流水线的终点，负责将处理后的数据写入最终目标。
//
// 一个 Pipeline 可以配置多个 Sink，数据会同时写入所有 Sink。
// 例如：同时写入本地 SQLite 数据库、导出 pprof 格式文件、推送到 Prometheus。
//
// 典型 Sink 示例：
// ================
// - ConsoleSink     — 输出到 stdout（调试用）
// - FileExportSink  — 写入 JSONL 文件
// - LocalStorageSink — 写入本地 SQLite 存储
// - PrometheusSink  — 暴露为 Prometheus 指标
// - PprofExportSink — 导出为 pprof/折叠栈格式
// - OtlpExportSink  — 导出到 OpenTelemetry Collector
// - WebSocketSink   — 通过 WebSocket 推送到前端
// ============================================================================

#pragma once

#include "plugin/api/plugin_api.h"

namespace illuminator {

class SinkPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSink; }

    // ---- 写入数据 ----
    // 将一批数据写入目标。接收 shared_ptr 以避免拷贝。
    virtual Status Write(DataBatchPtr batch) = 0;

    // ---- 刷新缓冲 ----
    // 将内部缓冲的数据刷出到目标。
    // 默认空实现：无内部缓冲的 Sink 不需要此操作。
    virtual Status Flush() { return Status::Ok(); }
};

}  // namespace illuminator
