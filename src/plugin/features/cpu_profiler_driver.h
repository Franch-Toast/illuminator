// CpuProfilerDriver — CPU 性能剖析器 Feature Driver (Tier 3)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/features/feature_registry.h"
#include "plugin/processors/stack_symbolizer/stack_symbolizer.h"
#include "server/sse_handler.h"
#include "plugin/sources/cpu/cpu_profiler/cpu_profiler.h"

namespace illuminator {

class CpuProfilerDriver : public FeatureDriver {
public:
    const char* Name() const override { return "cpu_profiler"; }
    const char* DisplayName() const override { return "CPU Profiler"; }
    const char* Category() const override { return "cpu"; }
    DriverTier Tier() const override { return DriverTier::kProfiling; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "cpu_profiler"; d.display_name = "CPU Profiler";
        d.description = "eBPF-based CPU profiling with stack traces";
        d.category = "cpu"; d.version = "1.0.0";
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
    "sample_freq": { "type": "integer", "default": 49, "minimum": 1, "maximum": 999 },
    "duration_sec": { "type": "integer", "default": 30, "minimum": 1 },
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
        auto pipeline = std::make_unique<Pipeline>("cpu_profiler");
        auto source = std::make_unique<CpuProfilerSource>();
        ConfigValue cfg;
        cfg.Set("frequency_hz", int64_t{49});
        source->Init(cfg);
        pipeline->SetSource(std::move(source));

        auto symbolizer = std::make_unique<StackSymbolizerProcessor>();
        ConfigValue sym_cfg;
        sym_cfg.Set("demangle", "true");
        sym_cfg.Set("kernel_symbols", "true");
        symbolizer->Init(sym_cfg);
        pipeline->AddProcessor(std::move(symbolizer));

        pipeline->AddSink(std::make_unique<SseSink>("cpu_profiler"));
        return pipeline;
    }
};

REGISTER_FEATURE(CpuProfilerDriver);

}  // namespace illuminator
