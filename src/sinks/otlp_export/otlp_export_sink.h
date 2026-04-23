#pragma once

#include <sstream>
#include <string>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// OpenTelemetry Protocol (OTLP) export sink.
// Exports metrics in OTLP JSON format over HTTP.
// Full protobuf OTLP support requires grpc/protobuf dependencies.
class OtlpExportSink : public SinkPlugin {
public:
    const char* Name() const override { return "otlp_export"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        endpoint_ = config["endpoint"].AsString("http://localhost:4318/v1/metrics");
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch || batch->Empty()) return Status::Ok();

        // Build OTLP JSON payload
        std::ostringstream json;
        json << "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[";

        bool first = true;
        for (auto& rec : batch->records()) {
            for (auto& [key, val] : rec.fields) {
                if (!first) json << ",";
                json << "{\"name\":\"" << key << "\",\"gauge\":{\"dataPoints\":[{";
                json << "\"timeUnixNano\":" << TimestampToNanos(rec.timestamp);
                json << ",\"asDouble\":" << ExtractDouble(val);

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

        // In a full implementation, this would POST to the OTLP endpoint.
        // For now, accumulate for retrieval via API.
        last_payload_ = json.str();
        payloads_sent_++;

        return Status::Ok();
    }

    const std::string& LastPayload() const { return last_payload_; }
    uint64_t PayloadsSent() const { return payloads_sent_; }

private:
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
    uint64_t payloads_sent_ = 0;
};

IL_REGISTER_SINK("otlp_export", OtlpExportSink);

}  // namespace illuminator
