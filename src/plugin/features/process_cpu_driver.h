// ProcessCpuDriver — 按进程 CPU 利用率 Feature Driver (Tier 1)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/features/feature_registry.h"
#include "server/sse_handler.h"
#include "plugin/sources/cpu/process_cpu/process_cpu.h"

namespace illuminator {

class ProcessCpuDriver : public FeatureDriver {
public:
    const char* Name() const override { return "process_cpu"; }
    const char* DisplayName() const override { return "Process CPU"; }
    const char* Category() const override { return "cpu"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "process_cpu"; d.display_name = "Process CPU";
        d.description = "Per-process CPU usage (top-N by utilization)";
        d.category = "cpu"; d.version = "1.0.0";
        d.tier = DriverTier::kMonitoring;
        d.model = DataModelType::kTimeSeries;
        d.supports_pause = true; d.supports_configure = true;
        return d;
    }

    std::string ConfigSchema() const override {
        return R"({
  "type": "object",
  "properties": {
    "interval_ms": { "type": "integer", "default": 2000, "minimum": 500 },
    "top_n": { "type": "integer", "default": 20, "minimum": 5 },
    "include_comm_regex": { "type": "string", "default": "" },
    "exclude_comm_regex": { "type": "string", "default": "" }
  }
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
        auto pipeline = std::make_unique<Pipeline>("process_cpu");
        auto source = std::make_unique<ProcessCpuSource>();
        ConfigValue cfg;
        cfg["interval_ms"] = ConfigValue(static_cast<int64_t>(2000));
        cfg["top_n"] = ConfigValue(static_cast<int64_t>(20));
        source->Init(cfg);
        pipeline->SetSource(std::move(source));
        pipeline->AddSink(std::make_unique<SseSink>("process_cpu"));
        return pipeline;
    }
};

REGISTER_FEATURE(ProcessCpuDriver);

}  // namespace illuminator
