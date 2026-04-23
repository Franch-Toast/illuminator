#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// Reads system metrics from /proc (Linux procfs).
// Collects CPU utilization, memory usage, and basic system stats.
class ProcStatReader : public SourcePlugin {
public:
    const char* Name() const override { return "proc_stat_reader"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        CollectCpuStats(batch);
        CollectMemInfo(batch);
        CollectLoadAvg(batch);

        return batch;
    }

private:
    void CollectCpuStats(DataBatchPtr& batch) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.substr(0, 3) != "cpu") break;

            std::istringstream iss(line);
            std::string cpu_name;
            uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
            iss >> cpu_name >> user >> nice >> system >> idle
                >> iowait >> irq >> softirq >> steal;

            auto& rec = batch->AddRecord();
            auto name = batch->InternString(cpu_name);
            rec.labels.push_back({batch->InternString("cpu"), name});
            rec.SetField(batch->InternString("user"), user);
            rec.SetField(batch->InternString("nice"), nice);
            rec.SetField(batch->InternString("system"), system);
            rec.SetField(batch->InternString("idle"), idle);
            rec.SetField(batch->InternString("iowait"), iowait);
            rec.SetField(batch->InternString("irq"), irq);
            rec.SetField(batch->InternString("softirq"), softirq);
            rec.SetField(batch->InternString("steal"), steal);
        }
    }

    void CollectMemInfo(DataBatchPtr& batch) {
        std::ifstream file("/proc/meminfo");
        if (!file.is_open()) return;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({
            batch->InternString("type"),
            batch->InternString("memory")
        });

        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string key;
            uint64_t value;
            iss >> key >> value;

            if (key == "MemTotal:") {
                rec.SetField(batch->InternString("mem_total_kb"), value);
            } else if (key == "MemFree:") {
                rec.SetField(batch->InternString("mem_free_kb"), value);
            } else if (key == "MemAvailable:") {
                rec.SetField(batch->InternString("mem_available_kb"), value);
            } else if (key == "Buffers:") {
                rec.SetField(batch->InternString("buffers_kb"), value);
            } else if (key == "Cached:") {
                rec.SetField(batch->InternString("cached_kb"), value);
            } else if (key == "SwapTotal:") {
                rec.SetField(batch->InternString("swap_total_kb"), value);
            } else if (key == "SwapFree:") {
                rec.SetField(batch->InternString("swap_free_kb"), value);
            }
        }
    }

    void CollectLoadAvg(DataBatchPtr& batch) {
        std::ifstream file("/proc/loadavg");
        if (!file.is_open()) return;

        double load1, load5, load15;
        file >> load1 >> load5 >> load15;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({
            batch->InternString("type"),
            batch->InternString("loadavg")
        });
        rec.SetField(batch->InternString("load_1m"), load1);
        rec.SetField(batch->InternString("load_5m"), load5);
        rec.SetField(batch->InternString("load_15m"), load15);
    }

    uint32_t interval_ms_ = 1000;
};

IL_REGISTER_SOURCE("proc_stat_reader", ProcStatReader);

}  // namespace illuminator
