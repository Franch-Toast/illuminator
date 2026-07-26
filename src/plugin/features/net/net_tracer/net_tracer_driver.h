// NetTracerDriver — eBPF TCP 连接追踪 Feature Driver (Tier 2)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "plugin/features/net/net_tracer/net_tracer_source.h"

namespace illuminator {

class NetTracerDriver : public FeatureDriver {
public:
    const char* Name() const override { return "net_tracer"; }
    const char* DisplayName() const override { return "Network Tracer"; }
    const char* Category() const override { return "network"; }
    DriverTier Tier() const override { return DriverTier::kTracing; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "net_tracer"; d.display_name = "Network Tracer";
        d.description = "eBPF TCP connection lifecycle tracing";
        d.category = "network"; d.version = "1.0.0";
        d.tier = DriverTier::kTracing;
        d.model = DataModelType::kTrace;
        d.supports_pull = true;
        d.supports_pause = true; d.has_bpf_probe = true;
        return d;
    }

    std::string ConfigSchema() const override {
        return R"({
  "type": "object",
  "properties": {
    "include_ports": { "type": "array", "items": { "type": "integer" } },
    "exclude_ports": { "type": "array", "items": { "type": "integer" } }
  }
})";
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = MakePipeline("net_tracer");
        auto source = std::make_unique<EbpfNetTracer>();
        ConfigValue cfg;
        source->Init(cfg);
        pipeline->SetSource(std::move(source));
        pipeline->AddSink(MakeSseSink(Name()));
        return pipeline;
    }
};

REGISTER_FEATURE(NetTracerDriver);

}  // namespace illuminator
