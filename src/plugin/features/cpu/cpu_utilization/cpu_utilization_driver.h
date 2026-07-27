// ============================================================================
// CpuUtilizationDriver — CPU 利用率 Feature 驱动
// ============================================================================
//
// 【职责】
//   将 CpuUtilizationSource 组装成完整的 Pipeline，注册到 FeatureBus。
//   Tier 1 监控 Feature，系统启动时通过 ProbeAll() 自动激活。
//
// 【Pipeline 结构】
//   CpuUtilizationSource → AsyncChannel → ProcessThread → SseSink
//   无 Processor 和 Aggregator（数据直通，不做额外转换或聚合）。
//
// 【配置传递链路】
//   REST API → FeatureDriver::SetConfig(json) → Pipeline::Reconfigure()
//           → Source::Reconfigure() → 热更新采集参数
//
// 【前端自动发现】
//   Describe() 返回 FeatureDescriptor，前端通过 /api/v2/features 获取
//   Feature 元数据，自动渲染配置面板和图表页面。
// ============================================================================

#pragma once

#include <memory>
#include <string>

#include "core/engine/feature_driver.h"
#include "plugin/features/cpu/cpu_utilization/cpu_utilization_source.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class CpuUtilizationDriver : public FeatureDriver {
public:
    // ---- Feature 元数据 ----
    const char* Name() const override { return "cpu_utilization"; }
    const char* DisplayName() const override { return "CPU Utilization"; }
    const char* Version() const override { return "1.0.0"; }
    const char* Category() const override { return "cpu"; }

    // Tier 1 = 基础监控，ProbeAll() 时自动启动
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    // ---- 前端自动发现描述符 ----
    FeatureDescriptor Describe() const override {
        FeatureDescriptor desc;
        desc.name = Name();
        desc.display_name = DisplayName();
        desc.description = "系统级 CPU 利用率监控，基于 /proc/stat 零侵入采集";
        desc.category = Category();
        desc.version = Version();
        desc.tier = Tier();
        desc.model = DataModelType::kTimeSeries;
        desc.supports_pause = true;
        desc.supports_configure = true;
        desc.has_bpf_probe = false;

        // 配置参数声明，前端可据此自动渲染配置表单
        desc.params = {
            {"interval_ms", "采集间隔 (ms)", ParamType::kInteger, false, "1000",
             "数据采集频率，越小越实时但开销越大", 200, 10000, {}},
            {"collect_per_core", "采集每核数据", ParamType::kBoolean, false, "true",
             "是否为每个 CPU 核心输出独立的利用率数据", 0, 0, {}},
            {"collect_frequency", "采集 CPU 频率", ParamType::kBoolean, false, "false",
             "是否读取 sysfs 获取各核当前运行频率", 0, 0, {}},
            {"ema_alpha", "EMA 平滑系数", ParamType::kNumber, false, "0",
             "指数移动平均系数，0=不平滑，0.3=中等平滑", 0, 0, {}},
        };

        return desc;
    }

    // ---- JSON Schema（供前端自动渲染配置表单） ----
    std::string ConfigSchema() const override {
        return R"JSON({
  "type": "object",
  "properties": {
    "interval_ms":        {"type":"integer","default":1000,"minimum":200,"maximum":10000,"description":"采集间隔 (ms)"},
    "collect_per_core":   {"type":"boolean","default":true,"description":"采集每核心数据"},
    "collect_frequency":  {"type":"boolean","default":false,"description":"采集 CPU 频率"},
    "ema_alpha":          {"type":"number","default":0,"minimum":0,"maximum":1,"description":"EMA 平滑系数"}
  }
})JSON";
    }

    // ---- 配置读取/写入 ----
    std::string GetConfig() const override {
        return config_json_;
    }

    Status SetConfig(const std::string& json_config) override {
        config_json_ = json_config;
        // 将 JSON 解析并传递给 Pipeline 执行 Reconfigure
        // 简单实现：直接存储 JSON，具体解析交给 Source::Reconfigure
        return Status::Ok();
    }

protected:
    // ================================================================
    // BuildPipeline — 组装 Feature 的数据处理流水线
    // ================================================================
    // Pipeline 结构：Source → (no Processor) → SseSink
    // 即 CpuUtilizationSource 采集的数据直接经 SSE 推送给前端
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& /*infra*/) override {
        auto pipeline = MakePipeline("cpu_utilization");

        // 创建 Source 并初始化默认配置
        auto source = std::make_unique<CpuUtilizationSource>();
        ConfigValue cfg;
        cfg.Set("interval_ms",        int64_t(1000));
        cfg.Set("collect_per_core",    "true");
        cfg.Set("collect_frequency",   "false");
        cfg.Set("ema_alpha",          "0");
        source->Init(cfg);

        pipeline->SetSource(std::move(source));

        // SSE Sink：将 DataBatch 序列化为 JSON 通过 Server-Sent Events 推送
        pipeline->AddSink(MakeSseSink(Name()));

        return pipeline;
    }

private:
    std::string config_json_ = "{}";
};

}  // namespace illuminator
