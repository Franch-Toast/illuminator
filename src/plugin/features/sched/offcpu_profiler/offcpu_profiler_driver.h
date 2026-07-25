// OffcpuProfilerDriver — Off-CPU 分析器 Feature Driver (Tier 3)

#pragma once

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "plugin/processors/stack_symbolizer/stack_symbolizer.h"
#include "server/sse_handler.h"
#include "plugin/features/sched/offcpu_profiler/offcpu_profiler_source.h"

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
    "min_block_us": { "type": "integer", "default": 1000, "minimum": 1, "x-requires-restart": true },
    "max_block_us": { "type": "integer", "default": 1000000000 },
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

        // min_block_us 写入 BPF rodata，运行时不可修改，需要重启生效
        if (!params["min_block_us"].AsString("").empty()) {
            config_.Set("min_block_us", params["min_block_us"].AsInt(
                config_["min_block_us"].AsInt(1000)));
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
                          "min_block_us is stored in BPF rodata and requires restart");
        }
        return status;
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("offcpu_profiler");
        auto source = std::make_unique<OffcpuProfilerSource>();
        if (config_.IsNull()) {
            config_.Set("min_block_us", int64_t{1000});
            config_.Set("max_block_us", int64_t{1000000000});
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

        pipeline->AddSink(std::make_unique<SseSink>("offcpu_profiler"));
        return pipeline;
    }

private:
    ConfigValue config_;
};

REGISTER_FEATURE(OffcpuProfilerDriver);

}  // namespace illuminator
