// ============================================================================
// CpuUtilizationDriver — CPU 利用率 Feature Driver
// ============================================================================
//
// Tier 1 (Monitoring): 纯 procfs 读取，极低开销。
// 构建一条 Pipeline: CpuUtilizationSource → (no processor) → [SseSink]
//
// JSON Schema 参数:
//   - interval_ms: 采集间隔 (默认 1000)
//   - collect_per_core: 是否按核心拆分 (默认 true)
//   - ema_alpha: EMA 平滑系数 (默认 0)
// ============================================================================

#pragma once

#include <nlohmann/json.hpp>

#include "core/engine/feature_driver.h"
#include "plugin/infra/feature_registry.h"
#include "server/sse_handler.h"
#include "plugin/features/cpu/cpu_utilization/cpu_utilization_source.h"

namespace illuminator {

class CpuUtilizationDriver : public FeatureDriver {
public:
    const char* Name() const override { return "cpu_utilization"; }
    const char* DisplayName() const override { return "CPU Utilization"; }
    const char* Version() const override { return "2.0.0"; }
    const char* Category() const override { return "cpu"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor desc;
        desc.name = "cpu_utilization";
        desc.display_name = "CPU Utilization";
        desc.description = "System CPU utilization metrics (user/system/idle/iowait)";
        desc.category = "cpu";
        desc.version = "2.0.0";
        desc.tier = DriverTier::kMonitoring;
        desc.model = DataModelType::kTimeSeries;
        desc.supports_pause = true;
        desc.supports_configure = true;
        return desc;
    }

    std::string ConfigSchema() const override {
        return R"json({
  "type": "object",
  "properties": {
    "interval_ms": { "type": "integer", "default": 1000, "minimum": 100, "maximum": 60000, "description": "Collection interval (ms)" },
    "collect_per_core": { "type": "boolean", "default": true, "description": "Collect per-core breakdown" },
    "ema_alpha": { "type": "number", "default": 0, "minimum": 0, "maximum": 1, "description": "EMA smoothing factor (0=disabled)" }
  }
})json";
    }

    std::string GetConfig() const override {
        return "{\"interval_ms\":" + std::to_string(interval_ms_) +
               ",\"collect_per_core\":" + (collect_per_core_ ? "true" : "false") +
               ",\"ema_alpha\":" + std::to_string(ema_alpha_) + "}";
    }

    Status SetConfig(const std::string& json_config) override {
        try {
            auto j = nlohmann::json::parse(json_config);
            ConfigValue params;
            if (j.contains("interval_ms") && j["interval_ms"].is_number_integer()) {
                interval_ms_ = j["interval_ms"].get<uint32_t>();
                params["interval_ms"] = ConfigValue(static_cast<int64_t>(interval_ms_));
            }
            if (j.contains("collect_per_core") && j["collect_per_core"].is_boolean()) {
                collect_per_core_ = j["collect_per_core"].get<bool>();
                params["collect_per_core"] = ConfigValue(collect_per_core_);
            }
            if (j.contains("ema_alpha") && j["ema_alpha"].is_number()) {
                ema_alpha_ = j["ema_alpha"].get<double>();
                params["ema_alpha"] = ConfigValue(ema_alpha_);
            }
            return Reconfigure(params);
        } catch (const std::exception& e) {
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("Invalid JSON config: ") + e.what());
        }
    }

    Status Reconfigure(const ConfigValue& params) override {
        if (!pipeline_) {
            return Status::Ok();
        }
        return pipeline_->Reconfigure(params);
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("cpu_utilization");

        auto source = std::make_unique<CpuUtilizationSource>();
        ConfigValue cfg;
        cfg["interval_ms"] = ConfigValue(static_cast<int64_t>(interval_ms_));
        cfg["collect_per_core"] = ConfigValue(collect_per_core_);
        cfg["ema_alpha"] = ConfigValue(ema_alpha_);
        source->Init(cfg);

        pipeline->SetSource(std::move(source));
        pipeline->AddSink(std::make_unique<SseSink>("cpu_utilization"));
        return pipeline;
    }

private:
    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    double ema_alpha_ = 0.0;
};

REGISTER_FEATURE(CpuUtilizationDriver);

}  // namespace illuminator
