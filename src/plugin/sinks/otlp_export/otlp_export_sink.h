// =============================================================================
// 文件：otlp_export_sink.h
// 模块：Illuminator 数据出口 - OTLP 指标导出
// 描述：
//   将 Record 数据转换为 OpenTelemetry Protocol (OTLP) JSON 格式。
//   当前版本将所有指标组织为一个 resourceMetrics 批量 JSON 载荷，
//   暂存于内存中供 API 访问。完整的 gRPC/HTTP POST 实现需后续完善。
//   协议版本：OTLP v1 metrics (JSON encoding)。
// =============================================================================

#pragma once

#include <sstream>
#include <string>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// OtlpExportSink: OpenTelemetry OTLP 导出 Sink
// 将 Record 中的字段值转为 OTLP JSON 格式的 Metric Gauge DataPoint。
// 每个 Record 的 labels 映射为 OTLP attributes。
// 配置参数:
//   endpoint - OTLP 接收端 URL，默认 http://localhost:4318/v1/metrics
// 当前阶段:
//   - 构建 JSON 载荷并暂存内存
//   - 可通过 API 获取最后一条载荷和发送计数
//   - 实际 HTTP POST 调用尚未实现（需引入 HTTP 客户端库）
class OtlpExportSink : public SinkPlugin {
public:
    const char* Name() const override { return "otlp_export"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsStub() const override { return true; }

    // 从配置中解析 OTLP 端点 URL
    // 参数:
    //   config - 配置项，包含 endpoint 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        endpoint_ = config["endpoint"].AsString("http://localhost:4318/v1/metrics");
        return Status::Ok();
    }

    // 将 DataBatch 转换为 OTLP JSON 载荷并暂存
    // 载荷结构:
    //   {
    //     "resourceMetrics": [{
    //       "scopeMetrics": [{
    //         "metrics": [
    //           { "name": "<field_name>",
    //             "gauge": { "dataPoints": [{
    //               "timeUnixNano": <timestamp>,
    //               "asDouble": <value>,
    //               "attributes": [{"key":"<label_key>","value":{"stringValue":"<label_value>"}}]
    //             }]}
    //           }
    //         ]
    //       }]
    //     }]
    //   }
    // 参数:
    //   batch - 包含 Record 的 DataBatch
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!batch || batch->Empty()) return Status::Ok();

        // 手工拼接 OTLP JSON 载荷
        std::ostringstream json;
        json << "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[";

        bool first = true;
        for (auto& rec : batch->records()) {
            for (auto& [key, val] : rec.fields) {
                if (!first) json << ",";
                // Gauge 指标 DataPoint
                json << "{\"name\":\"" << key << "\",\"gauge\":{\"dataPoints\":[{";
                json << "\"timeUnixNano\":" << TimestampToNanos(rec.timestamp);
                json << ",\"asDouble\":" << ExtractDouble(val);

                // 将 Record 的标签转换为 OTLP attributes
                if (!rec.labels.empty()) {
                    json << ",\"attributes\":[";
                    bool label_first = true;
                    for (auto& l : rec.labels) {
                        if (!label_first) json << ",";
                        json << "{\"key\":\"" << l.key
                             << "\",\"value\":{\"stringValue\":\"" << l.value << "\"}}";
                        label_first = false;
                    }
                    json << "]";
                }

                json << "}]}}";
                first = false;
            }
        }

        json << "]}]}]}";

        last_payload_ = json.str();
        payloads_built_++;

        return Status::Ok();
    }

    // 获取最近一次生成的 OTLP JSON 载荷（供 API 调试或手动发送）
    const std::string& LastPayload() const { return last_payload_; }

    uint64_t PayloadsBuilt() const { return payloads_built_; }

private:
    // 将 FieldValue 统一转换为 double 数值
    // 非数值类型返回 0（OTLP gauge 仅支持 asDouble/asInt）。
    // 参数:
    //   val - variant 字段值
    // 返回:
    //   double 类型数值
    static double ExtractDouble(const FieldValue& val) {
        struct Vis {
            double operator()(std::monostate) const { return 0; }
            double operator()(bool v) const { return v ? 1.0 : 0.0; }
            double operator()(int64_t v) const { return static_cast<double>(v); }
            double operator()(uint64_t v) const { return static_cast<double>(v); }
            double operator()(double v) const { return v; }
            double operator()(std::string_view) const { return 0; }
        };
        return std::visit(Vis{}, val);
    }

    std::string endpoint_;
    std::string last_payload_;
    uint64_t payloads_built_ = 0;
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("otlp_export", OtlpExportSink);

}  // namespace illuminator
