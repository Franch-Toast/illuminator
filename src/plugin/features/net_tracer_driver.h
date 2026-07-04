// NetTracerDriver — eBPF TCP 连接追踪 Feature Driver (Tier 2)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/features/feature_registry.h"
#include "server/sse_handler.h"
#include "plugin/sources/net/ebpf_net_tracer/ebpf_net_tracer.h"

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
        d.supports_push = true; d.supports_pull = false;
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
        auto pipeline = std::make_unique<Pipeline>("net_tracer");
        auto source = std::make_unique<EbpfNetTracer>();
        ConfigValue cfg;
        source->Init(cfg);
        pipeline->SetSource(std::move(source));
        pipeline->AddSink(std::make_unique<SseSink>("net_tracer"));
        return pipeline;
    }
};

REGISTER_FEATURE(NetTracerDriver);

}  // namespace illuminator
