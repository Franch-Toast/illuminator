// IoMonitorDriver — eBPF I/O 延迟监控 Feature Driver (Tier 2)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "server/sse_handler.h"
#include "plugin/features/io/io_monitor/io_monitor_source.h"

namespace illuminator {

class IoMonitorDriver : public FeatureDriver {
public:
    const char* Name() const override { return "io_monitor"; }
    const char* DisplayName() const override { return "I/O Monitor"; }
    const char* Category() const override { return "io"; }
    DriverTier Tier() const override { return DriverTier::kTracing; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "io_monitor"; d.display_name = "I/O Monitor";
        d.description = "eBPF block I/O latency tracing";
        d.category = "io"; d.version = "1.0.0";
        d.tier = DriverTier::kTracing;
        d.model = DataModelType::kTrace;
        d.supports_push = true; d.supports_pull = false;
        d.supports_pause = true; d.has_bpf_probe = true;
        return d;
    }

    std::string ConfigSchema() const override {
        return R"({
  "type": "object",
  "properties": {
    "min_latency_us": { "type": "integer", "default": 0, "minimum": 0 },
    "include_pids": { "type": "array", "items": { "type": "integer" } }
  }
})";
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("io_monitor");
        auto source = std::make_unique<EbpfIoMonitor>();
        ConfigValue cfg;
        source->Init(cfg);
        pipeline->SetSource(std::move(source));
        pipeline->AddSink(std::make_unique<SseSink>("io_monitor"));
        return pipeline;
    }
};

REGISTER_FEATURE(IoMonitorDriver);

}  // namespace illuminator
