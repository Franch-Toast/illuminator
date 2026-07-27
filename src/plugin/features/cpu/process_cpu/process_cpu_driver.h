// ============================================================================
// ProcessCpuDriver — 进程/线程级 CPU 利用率 Feature 驱动
// ============================================================================
//
// 【职责】
//   将 ProcessCpuSource 组装成完整的 Pipeline，注册到 FeatureBus。
//   Tier 1 监控 Feature，与 cpu_utilization (系统级) 配合使用。
//
// 【Pipeline 结构】
//   ProcessCpuSource → AsyncChannel → ProcessThread → SseSink
//
// 【前端展示】
//   在 CPU 页面的 "Processes" 子标签页中展示，包括：
//   - Top-N 进程表（可排序、可搜索）
//   - 点击进程 → 展开线程级 CPU 详情（通过 QueryExtra）
// ============================================================================

#pragma once

#include <memory>
#include <string>

#include "core/engine/feature_driver.h"
#include "plugin/features/cpu/process_cpu/process_cpu_source.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class ProcessCpuDriver : public FeatureDriver {
public:
    const char* Name() const override { return "process_cpu"; }
    const char* DisplayName() const override { return "Process CPU"; }
    const char* Version() const override { return "1.0.0"; }
    const char* Category() const override { return "cpu"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor desc;
        desc.name = Name();
        desc.display_name = DisplayName();
        desc.description = "进程/线程级 CPU 利用率监控，基于 /proc/[pid]/stat 采集 Top-N 进程";
        desc.category = Category();
        desc.version = Version();
        desc.tier = Tier();
        desc.model = DataModelType::kTimeSeries;
        desc.supports_pause = true;
        desc.supports_configure = true;
        desc.has_bpf_probe = false;

        desc.params = {
            {"interval_ms", "采集间隔 (ms)", ParamType::kInteger, false, "2000",
             "进程扫描频率，建议 >= 1000ms", 500, 10000, {}},
            {"top_n", "Top-N 进程数", ParamType::kInteger, false, "20",
             "保留 CPU 占用最高的前 N 个进程", 5, 100, {}},
            {"min_cpu_threshold", "最小上报阈值 (%)", ParamType::kNumber, false, "0.1",
             "CPU 占比低于此值的进程不上报", 0, 0, {}},
            {"comm_filter", "进程名过滤", ParamType::kString, false, "",
             "正则表达式，仅上报名称匹配的进程", 0, 0, {}},
            {"exclude_kernel_threads", "排除内核线程", ParamType::kBoolean, false, "true",
             "排除 vsize=0 的内核线程", 0, 0, {}},
        };

        return desc;
    }

    std::string ConfigSchema() const override {
        return R"JSON({
  "type": "object",
  "properties": {
    "interval_ms":        {"type":"integer","default":2000,"minimum":500,"maximum":10000,"description":"采集间隔 (ms)"},
    "top_n":              {"type":"integer","default":20,"minimum":5,"maximum":100,"description":"Top-N 进程数"},
    "min_cpu_threshold":  {"type":"number","default":0.1,"minimum":0,"maximum":100,"description":"最小上报阈值 (%)"},
    "comm_filter":        {"type":"string","default":"","description":"进程名正则过滤"},
    "exclude_kernel_threads": {"type":"boolean","default":true,"description":"排除内核线程"}
  }
})JSON";
    }

    std::string GetConfig() const override { return config_json_; }
    Status SetConfig(const std::string& json_config) override {
        config_json_ = json_config;
        return Status::Ok();
    }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& /*infra*/) override {
        auto pipeline = MakePipeline("process_cpu");

        auto source = std::make_unique<ProcessCpuSource>();
        ConfigValue cfg;
        cfg.Set("interval_ms",              int64_t(2000));
        cfg.Set("top_n",                    int64_t(20));
        cfg.Set("min_cpu_threshold",        "0.1");
        cfg.Set("exclude_kernel_threads",   "true");
        source->Init(cfg);

        pipeline->SetSource(std::move(source));
        pipeline->AddSink(MakeSseSink(Name()));
        return pipeline;
    }

private:
    std::string config_json_ = "{}";
};

}  // namespace illuminator
