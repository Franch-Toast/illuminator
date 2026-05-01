#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class CpuUtilizationSource : public SourcePlugin {
public:
    const char* Name() const override { return "cpu_utilization"; }
    const char* Version() const override { return "1.0.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_per_core_ = config["collect_per_core"].AsBool(true);
        collect_freq_ = config["collect_frequency"].AsBool(false);
        ema_alpha_ = config["ema_alpha"].AsDouble(0.0);
        ReadSnapshot(prev_);
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        Snapshot cur;
        ReadSnapshot(cur);

        if (has_prev_) {
            EmitCpuUtilization(batch, cur);
            EmitSystemCounters(batch, cur);
        }
        EmitLoadAvg(batch);
        EmitRunQueue(batch, cur);
        if (collect_freq_) EmitCpuFrequency(batch);

        prev_ = cur;
        has_prev_ = true;
        return batch;
    }

private:
    struct CoreJiffies {
        std::string name;
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

        uint64_t Total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
        uint64_t Busy() const { return Total() - idle - iowait; }
    };

    struct Snapshot {
        std::vector<CoreJiffies> cores;
        uint64_t ctxt = 0;
        uint64_t intr = 0;
        uint32_t procs_running = 0;
        uint32_t procs_blocked = 0;
    };

    struct EmaState {
        double user = 0, system = 0, idle = 0, iowait = 0, busy = 0;
        bool initialized = false;
    };

    void ReadSnapshot(Snapshot& snap) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.compare(0, 3, "cpu") == 0 &&
                (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
                CoreJiffies j;
                std::istringstream iss(line);
                iss >> j.name >> j.user >> j.nice >> j.system >> j.idle
                    >> j.iowait >> j.irq >> j.softirq >> j.steal;
                snap.cores.push_back(j);
            } else if (line.compare(0, 4, "ctxt") == 0) {
                std::istringstream iss(line.substr(5));
                iss >> snap.ctxt;
            } else if (line.compare(0, 4, "intr") == 0) {
                std::istringstream iss(line.substr(5));
                iss >> snap.intr;
            } else if (line.compare(0, 13, "procs_running") == 0) {
                std::istringstream iss(line.substr(14));
                iss >> snap.procs_running;
            } else if (line.compare(0, 13, "procs_blocked") == 0) {
                std::istringstream iss(line.substr(14));
                iss >> snap.procs_blocked;
            }
        }
    }

    void EmitCpuUtilization(DataBatchPtr& batch, const Snapshot& cur) {
        if (ema_states_.size() < cur.cores.size())
            ema_states_.resize(cur.cores.size());

        for (size_t i = 0; i < cur.cores.size(); ++i) {
            bool is_total = (cur.cores[i].name == "cpu");
            if (!is_total && !collect_per_core_) continue;
            if (i >= prev_.cores.size()) break;

            auto& c = cur.cores[i];
            auto& p = prev_.cores[i];
            if (c.name != p.name) continue;

            uint64_t dt = c.Total() - p.Total();
            if (dt == 0) dt = 1;

            auto pct = [dt](uint64_t cv, uint64_t pv) -> double {
                return 100.0 * static_cast<double>(cv - pv) / static_cast<double>(dt);
            };

            double user_pct = pct(c.user, p.user);
            double nice_pct = pct(c.nice, p.nice);
            double sys_pct = pct(c.system, p.system);
            double idle_pct = pct(c.idle, p.idle);
            double iowait_pct = pct(c.iowait, p.iowait);
            double irq_pct = pct(c.irq, p.irq);
            double softirq_pct = pct(c.softirq, p.softirq);
            double steal_pct = pct(c.steal, p.steal);
            double busy_pct = 100.0 * static_cast<double>(c.Busy() - p.Busy()) /
                              static_cast<double>(dt);

            if (ema_alpha_ > 0.0 && i < ema_states_.size()) {
                auto& ema = ema_states_[i];
                if (!ema.initialized) {
                    ema.user = user_pct;
                    ema.system = sys_pct;
                    ema.idle = idle_pct;
                    ema.iowait = iowait_pct;
                    ema.busy = busy_pct;
                    ema.initialized = true;
                } else {
                    ema.user = ema_alpha_ * user_pct + (1.0 - ema_alpha_) * ema.user;
                    ema.system = ema_alpha_ * sys_pct + (1.0 - ema_alpha_) * ema.system;
                    ema.idle = ema_alpha_ * idle_pct + (1.0 - ema_alpha_) * ema.idle;
                    ema.iowait = ema_alpha_ * iowait_pct + (1.0 - ema_alpha_) * ema.iowait;
                    ema.busy = ema_alpha_ * busy_pct + (1.0 - ema_alpha_) * ema.busy;
                }
            }

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_utilization")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString(is_total ? "cpu_total" : "cpu_core")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString(c.name)});

            rec.SetField(batch->InternString("user_pct"), user_pct);
            rec.SetField(batch->InternString("nice_pct"), nice_pct);
            rec.SetField(batch->InternString("system_pct"), sys_pct);
            rec.SetField(batch->InternString("idle_pct"), idle_pct);
            rec.SetField(batch->InternString("iowait_pct"), iowait_pct);
            rec.SetField(batch->InternString("irq_pct"), irq_pct);
            rec.SetField(batch->InternString("softirq_pct"), softirq_pct);
            rec.SetField(batch->InternString("steal_pct"), steal_pct);
            rec.SetField(batch->InternString("busy_pct"), busy_pct);

            if (ema_alpha_ > 0.0 && i < ema_states_.size() && ema_states_[i].initialized) {
                auto& ema = ema_states_[i];
                rec.SetField(batch->InternString("user_pct_ema"), ema.user);
                rec.SetField(batch->InternString("system_pct_ema"), ema.system);
                rec.SetField(batch->InternString("busy_pct_ema"), ema.busy);
            }
        }
    }

    void EmitSystemCounters(DataBatchPtr& batch, const Snapshot& cur) {
        double elapsed_sec = interval_ms_ / 1000.0;
        if (elapsed_sec <= 0) elapsed_sec = 1.0;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_utilization")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("system_counters")});

        rec.SetField(batch->InternString("context_switches_per_sec"),
                     static_cast<double>(cur.ctxt - prev_.ctxt) / elapsed_sec);
        rec.SetField(batch->InternString("interrupts_per_sec"),
                     static_cast<double>(cur.intr - prev_.intr) / elapsed_sec);
    }

    void EmitLoadAvg(DataBatchPtr& batch) {
        std::ifstream file("/proc/loadavg");
        if (!file.is_open()) return;

        double load1, load5, load15;
        file >> load1 >> load5 >> load15;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_utilization")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("loadavg")});
        rec.SetField(batch->InternString("load_1m"), load1);
        rec.SetField(batch->InternString("load_5m"), load5);
        rec.SetField(batch->InternString("load_15m"), load15);
    }

    void EmitRunQueue(DataBatchPtr& batch, const Snapshot& cur) {
        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_utilization")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("runqueue")});
        rec.SetField(batch->InternString("procs_running"),
                     static_cast<uint64_t>(cur.procs_running));
        rec.SetField(batch->InternString("procs_blocked"),
                     static_cast<uint64_t>(cur.procs_blocked));
    }

    void EmitCpuFrequency(DataBatchPtr& batch) {
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
                                  batch->InternString("cpu_utilization")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("cpu_freq")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString("cpu" + std::to_string(cpu_idx))});
            rec.SetField(batch->InternString("freq_mhz"),
                         static_cast<double>(freq_khz) / 1000.0);
            ++cpu_idx;
        }
    }

    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    bool collect_freq_ = false;
    double ema_alpha_ = 0.0;
    bool has_prev_ = false;
    Snapshot prev_;
    std::vector<EmaState> ema_states_;
};

IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);

}  // namespace illuminator
