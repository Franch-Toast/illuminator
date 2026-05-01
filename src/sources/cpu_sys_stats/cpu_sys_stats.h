#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class CpuSysStats : public SourcePlugin {
public:
    const char* Name() const override { return "cpu_sys_stats"; }
    const char* Version() const override { return "0.2.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_freq_ = config["collect_frequency"].AsBool(false);
        collect_interrupts_ = config["collect_interrupts"].AsBool(false);
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        CpuJiffies current;
        CollectCpuJiffies(batch, current);
        CollectContextSwitches(batch);
        CollectLoadAvg(batch);
        if (collect_freq_) CollectCpuFrequency(batch);

        prev_ = current;
        first_collect_ = false;
        return batch;
    }

private:
    struct CpuCoreJiffies {
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
        uint64_t Total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
    };

    struct CpuJiffies {
        std::vector<std::pair<std::string, CpuCoreJiffies>> cores;
        uint64_t ctxt = 0;
        uint64_t intr = 0;
    };

    void CollectCpuJiffies(DataBatchPtr& batch, CpuJiffies& snapshot) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.compare(0, 3, "cpu") == 0 && (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
                std::istringstream iss(line);
                std::string cpu_name;
                CpuCoreJiffies j;
                iss >> cpu_name >> j.user >> j.nice >> j.system >> j.idle
                    >> j.iowait >> j.irq >> j.softirq >> j.steal;
                snapshot.cores.emplace_back(cpu_name, j);
            } else if (line.compare(0, 4, "ctxt") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> snapshot.ctxt;
            } else if (line.compare(0, 4, "intr") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> snapshot.intr;
            }
        }

        if (first_collect_) return;

        for (size_t i = 0; i < snapshot.cores.size(); ++i) {
            auto& [name, cur] = snapshot.cores[i];
            const CpuCoreJiffies* prev_core = FindCore(prev_, name);
            if (!prev_core) continue;

            uint64_t delta_total = cur.Total() - prev_core->Total();
            if (delta_total == 0) delta_total = 1;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_sys_stats")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString(name)});

            auto pct = [&](uint64_t cur_val, uint64_t prev_val) -> double {
                return 100.0 * static_cast<double>(cur_val - prev_val) /
                       static_cast<double>(delta_total);
            };

            rec.SetField(batch->InternString("user_pct"), pct(cur.user, prev_core->user));
            rec.SetField(batch->InternString("nice_pct"), pct(cur.nice, prev_core->nice));
            rec.SetField(batch->InternString("system_pct"), pct(cur.system, prev_core->system));
            rec.SetField(batch->InternString("idle_pct"), pct(cur.idle, prev_core->idle));
            rec.SetField(batch->InternString("iowait_pct"), pct(cur.iowait, prev_core->iowait));
            rec.SetField(batch->InternString("irq_pct"), pct(cur.irq, prev_core->irq));
            rec.SetField(batch->InternString("softirq_pct"), pct(cur.softirq, prev_core->softirq));
            rec.SetField(batch->InternString("steal_pct"), pct(cur.steal, prev_core->steal));
        }
    }

    void CollectContextSwitches(DataBatchPtr& batch) {
        if (first_collect_) return;

        CpuJiffies dummy;
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        uint64_t ctxt = 0, intr = 0;
        std::string line;
        while (std::getline(file, line)) {
            if (line.compare(0, 4, "ctxt") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> ctxt;
            } else if (line.compare(0, 4, "intr") == 0) {
                std::istringstream iss(line);
                std::string key;
                iss >> key >> intr;
            }
        }

        if (prev_.ctxt > 0) {
            double elapsed_sec = interval_ms_ / 1000.0;
            if (elapsed_sec <= 0) elapsed_sec = 1.0;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_sys_stats")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("system_counters")});

            double ctxt_per_sec = static_cast<double>(ctxt - prev_.ctxt) / elapsed_sec;
            double intr_per_sec = static_cast<double>(intr - prev_.intr) / elapsed_sec;
            rec.SetField(batch->InternString("context_switches_per_sec"), ctxt_per_sec);
            rec.SetField(batch->InternString("interrupts_per_sec"), intr_per_sec);
        }
    }

    void CollectLoadAvg(DataBatchPtr& batch) {
        std::ifstream file("/proc/loadavg");
        if (!file.is_open()) return;

        double load1, load5, load15;
        file >> load1 >> load5 >> load15;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_sys_stats")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("loadavg")});
        rec.SetField(batch->InternString("load_1m"), load1);
        rec.SetField(batch->InternString("load_5m"), load5);
        rec.SetField(batch->InternString("load_15m"), load15);
    }

    void CollectCpuFrequency(DataBatchPtr& batch) {
        int cpu_idx = 0;
        while (true) {
            std::string path = "/sys/devices/system/cpu/cpu" +
                               std::to_string(cpu_idx) + "/cpufreq/scaling_cur_freq";
            std::ifstream file(path);
            if (!file.is_open()) break;

            uint64_t freq_khz;
            file >> freq_khz;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_sys_stats")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("cpu_freq")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString("cpu" + std::to_string(cpu_idx))});
            rec.SetField(batch->InternString("freq_mhz"),
                         static_cast<double>(freq_khz) / 1000.0);
            ++cpu_idx;
        }
    }

    static const CpuCoreJiffies* FindCore(const CpuJiffies& snap,
                                           const std::string& name) {
        for (auto& [n, j] : snap.cores) {
            if (n == name) return &j;
        }
        return nullptr;
    }

    uint32_t interval_ms_ = 1000;
    bool collect_freq_ = false;
    bool collect_interrupts_ = false;
    bool first_collect_ = true;
    CpuJiffies prev_;
};

IL_REGISTER_SOURCE("cpu_sys_stats", CpuSysStats);

}  // namespace illuminator
