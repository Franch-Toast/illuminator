// OffcpuProfilerDriver — Off-CPU 分析器 Feature Driver (Tier 3)

#pragma once

#include "core/engine/feature_driver.h"
#include "features/feature_registry.h"
#include "processors/stack_symbolizer/stack_symbolizer.h"
#include "server/sse_handler.h"
#include "sources/sched/offcpu_profiler/offcpu_profiler.h"

namespace illuminator {

class OffcpuProfilerDriver : public FeatureDriver {
public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* DisplayName() const override { return "Off-CPU Profiler"; }
    const char* Category() const override { return "scheduler"; }
    DriverTier Tier() const override { return DriverTier::kProfiling; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "offcpu_profiler"; d.display_name = "Off-CPU Profiler";
        d.description = "eBPF-based off-CPU profiling with block stacks";
        d.category = "scheduler"; d.version = "1.0.0";
        d.tier = DriverTier::kProfiling;
        d.model = DataModelType::kProfile;
        d.supports_pause = true; d.supports_configure = true;
        d.has_bpf_probe = true; d.session_required = true;
        return d;
    }

    std::string ConfigSchema() const override {
        return R"({
  "type": "object",
  "properties": {
    "min_block_us": { "type": "integer", "default": 1000, "minimum": 1 },
    "max_block_us": { "type": "integer", "default": 1000000000 },
    "target_pids": { "type": "array", "items": { "type": "integer" } }
  },
  "required": ["target_pids"]
})";
    }

    Status Reconfigure(const ConfigValue& params) override {
        if (!pipeline_ || !pipeline_->GetSource()) {
            return Status::Error(StatusCode::kUnavailable, "not running");
        }
        return pipeline_->GetSource()->Reconfigure(params);
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("offcpu_profiler");
        auto source = std::make_unique<OffcpuProfilerSource>();
        ConfigValue cfg;
        source->Init(cfg);
        pipeline->SetSource(std::move(source));

        auto symbolizer = std::make_unique<StackSymbolizerProcessor>();
        ConfigValue sym_cfg;
        sym_cfg.Set("demangle", "true");
        sym_cfg.Set("kernel_symbols", "true");
        symbolizer->Init(sym_cfg);
        pipeline->AddProcessor(std::move(symbolizer));

        pipeline->AddSink(std::make_unique<SseSink>("offcpu_profiler"));
        return pipeline;
    }
};

REGISTER_FEATURE(OffcpuProfilerDriver);

}  // namespace illuminator
