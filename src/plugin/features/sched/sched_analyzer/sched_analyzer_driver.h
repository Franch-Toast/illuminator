// SchedAnalyzerDriver — eBPF 调度分析 Feature Driver (Tier 2)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "plugin/features/sched/sched_analyzer/sched_analyzer_source.h"

namespace illuminator {

class SchedAnalyzerDriver : public FeatureDriver {
public:
    const char* Name() const override { return "sched_analyzer"; }
    const char* DisplayName() const override { return "Scheduler Analyzer"; }
    const char* Category() const override { return "scheduler"; }
    DriverTier Tier() const override { return DriverTier::kTracing; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor d;
        d.name = "sched_analyzer"; d.display_name = "Scheduler Analyzer";
        d.description = "eBPF scheduler event tracing and analysis";
        d.category = "scheduler"; d.version = "1.0.0";
        d.tier = DriverTier::kTracing;
        d.model = DataModelType::kTrace;
        d.supports_pull = true;
        d.supports_pause = true; d.supports_configure = true;
        d.has_bpf_probe = true;
        return d;
    }

    std::string ConfigSchema() const override {
        return R"({
  "type": "object",
  "properties": {
    "detailed_mode": { "type": "boolean", "default": false },
    "target_pids": { "type": "array", "items": { "type": "integer" }, "format": "pid_list" },
    "target_process_names": { "type": "array", "items": { "type": "string" }, "format": "pid_list" }
  }
})";
    }

    Status Reconfigure(const ConfigValue& params) override {
        if (!pipeline_) {
            return Status::Error(StatusCode::kUnavailable, "not running");
        }
        return pipeline_->Reconfigure(params);
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = MakePipeline("sched_analyzer");
        auto source = std::make_unique<SchedAnalyzerSource>();
        ConfigValue cfg;
        source->Init(cfg);
        pipeline->SetSource(std::move(source));
        pipeline->AddSink(MakeSseSink(Name()));
        return pipeline;
    }
};

REGISTER_FEATURE(SchedAnalyzerDriver);

}  // namespace illuminator
