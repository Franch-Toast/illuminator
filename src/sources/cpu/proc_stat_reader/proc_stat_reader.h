// ProcStatReader — lightweight /proc raw data reader for `top` command
#pragma once

#include <string>

#include "core/common/proc_reader.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class ProcStatReader : public SourcePlugin {
public:
    const char* Name() const override { return "proc_stat_reader"; }
    const char* Version() const override { return "0.2.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        auto snap = proc::ReadCpuSnapshot();
        for (auto& core : snap.cores) {
            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString(core.name)});
            rec.SetField(batch->InternString("user"), core.user);
            rec.SetField(batch->InternString("nice"), core.nice);
            rec.SetField(batch->InternString("system"), core.system);
            rec.SetField(batch->InternString("idle"), core.idle);
            rec.SetField(batch->InternString("iowait"), core.iowait);
            rec.SetField(batch->InternString("irq"), core.irq);
            rec.SetField(batch->InternString("softirq"), core.softirq);
            rec.SetField(batch->InternString("steal"), core.steal);
        }

        // Memory info
        {
            std::ifstream file("/proc/meminfo");
            if (file.is_open()) {
                auto& rec = batch->AddRecord();
                rec.labels.push_back({batch->InternString("type"),
                                      batch->InternString("memory")});
                std::string line;
                while (std::getline(file, line)) {
                    std::istringstream iss(line);
                    std::string key;
                    uint64_t value;
                    iss >> key >> value;
                    if (key == "MemTotal:")
                        rec.SetField(batch->InternString("mem_total_kb"), value);
                    else if (key == "MemFree:")
                        rec.SetField(batch->InternString("mem_free_kb"), value);
                    else if (key == "MemAvailable:")
                        rec.SetField(batch->InternString("mem_available_kb"), value);
                    else if (key == "Buffers:")
                        rec.SetField(batch->InternString("buffers_kb"), value);
                    else if (key == "Cached:")
                        rec.SetField(batch->InternString("cached_kb"), value);
                    else if (key == "SwapTotal:")
                        rec.SetField(batch->InternString("swap_total_kb"), value);
                    else if (key == "SwapFree:")
                        rec.SetField(batch->InternString("swap_free_kb"), value);
                }
            }
        }

        auto la = proc::ReadLoadAvg();
        {
            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("loadavg")});
            rec.SetField(batch->InternString("load_1m"), la.load1);
            rec.SetField(batch->InternString("load_5m"), la.load5);
            rec.SetField(batch->InternString("load_15m"), la.load15);
        }

        return batch;
    }

private:
    uint32_t interval_ms_ = 1000;
};

IL_REGISTER_SOURCE("proc_stat_reader", ProcStatReader);

}  // namespace illuminator
