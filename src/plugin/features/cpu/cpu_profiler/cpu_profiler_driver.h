// CpuProfilerDriver — CPU 性能剖析器 Feature Driver (Tier 3)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "plugin/processors/stack_symbolizer/stack_symbolizer.h"
#include "server/sse_handler.h"
#include "plugin/features/cpu/cpu_profiler/cpu_profiler_source.h"

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
    "sample_freq": { "type": "integer", "default": 49, "minimum": 1, "maximum": 999, "x-requires-restart": true },
    "duration_sec": { "type": "integer", "default": 30, "minimum": 1 },
    "target_pids": { "type": "array", "items": { "type": "integer" }, "format": "pid_list" },
    "target_process_names": { "type": "array", "items": { "type": "string" }, "format": "pid_list" }
  },
  "required": ["target_pids"]
})";
    }

    Status Reconfigure(const ConfigValue& params) override {
        if (!pipeline_) {
            return Status::Error(StatusCode::kUnavailable, "not running");
        }

        bool requires_restart = false;

        // sample_freq 写入 BPF rodata，运行时不可修改，需要重启生效
        if (!params["sample_freq"].AsString("").empty()) {
            config_.Set("sample_freq", params["sample_freq"].AsInt(
                config_["sample_freq"].AsInt(49)));
            requires_restart = true;
        }

        // 持久化可在运行时生效的过滤参数
        auto pid_str = params["target_pids"].AsString("");
        if (!pid_str.empty()) {
            config_.Set("target_pids", pid_str);
        }
        auto comm_str = params["target_process_names"].AsString(
            params["target_comms"].AsString(""));
        if (!comm_str.empty()) {
            config_.Set("target_process_names", comm_str);
        }

        auto status = pipeline_->Reconfigure(params);
        if (!IsReconfigureContinueCode(status.code())) return status;
        if (status.code() == StatusCode::kRequiresRestart) requires_restart = true;

        if (requires_restart) {
            return Status(StatusCode::kRequiresRestart,
                          "sample_freq is stored in BPF rodata and requires restart");
        }
        return status;
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("cpu_profiler");
        auto source = std::make_unique<CpuProfilerSource>();
        if (config_.IsNull()) {
            config_.Set("sample_freq", int64_t{49});
            config_.Set("duration_sec", int64_t{30});
            config_.Set("target_pids", std::string{});
            config_.Set("target_process_names", std::string{});
        }
        source->Init(config_);
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

private:
    ConfigValue config_;
};

REGISTER_FEATURE(CpuProfilerDriver);

}  // namespace illuminator
