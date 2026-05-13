// ============================================================================
// CpuUtilizationSource — 统一 CPU 利用率数据源（新一代实现）
// ============================================================================
//
// 从 /proc/stat、/proc/loadavg、sysfs 三个来源采集系统级 CPU 指标。
// 相比旧版 cpu_sys_monitor，新增了 EMA 指数移动平均平滑和 CPU 频率采集。
//
// 采集指标：
// ==========
// 1. CPU 利用率明细（按核心）：
//    user_pct, nice_pct, system_pct, idle_pct, iowait_pct,
//    irq_pct, softirq_pct, steal_pct, busy_pct
//    （busy = total - idle - iowait，即真正在干活的 CPU 时间占比）
//
// 2. CPU 利用率 EMA 平滑值（可选，配置 ema_alpha > 0）：
//    user_pct_ema, system_pct_ema, busy_pct_ema
//    （用于消除瞬时毛刺，类似 top 命令的显示效果）
//
// 3. 系统级计数器：
//    context_switches_per_sec（每秒上下文切换次数）
//    interrupts_per_sec（每秒中断次数）
//
// 4. 负载均值：load_1m, load_5m, load_15m（来自 /proc/loadavg）
//
// 5. 运行队列：procs_running（运行中进程数）, procs_blocked（阻塞进程数）
//
// 6. CPU 频率：freq_mhz（可选，来自 /sys/devices/system/cpu/*/cpufreq/scaling_cur_freq）
//
// 配置参数：
// ==========
// - interval_ms: 采集间隔（默认 1000ms）
// - collect_per_core: 是否按核心拆分（默认 true）
// - collect_frequency: 是否采集 CPU 频率（默认 false，root 可能受限）
// - ema_alpha: EMA 平滑系数（默认 0 = 不平滑，通常设 0.3）
//
// 使用 EMA 的计算公式：
//   ema_new = alpha * current + (1 - alpha) * ema_old
// alpha 越大越灵敏，越小越平滑
// ============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/common/proc_reader.h"
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
        prev_ = proc::ReadCpuSnapshot();
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        auto cur = proc::ReadCpuSnapshot();

        if (has_prev_) {
            EmitCpuUtilization(batch, cur);
            EmitSystemCounters(batch, cur);
        }
        auto la = proc::ReadLoadAvg();
        EmitLoadAvg(batch, la);
        EmitRunQueue(batch, cur);
        if (collect_freq_) EmitCpuFrequency(batch);

        prev_ = cur;
        has_prev_ = true;
        return batch;
    }

private:
    struct EmaState {
        double user = 0, system = 0, idle = 0, iowait = 0, busy = 0;
        bool initialized = false;
    };

    void EmitCpuUtilization(DataBatchPtr& batch, const proc::CpuSnapshot& cur) {
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
                    ema = {user_pct, sys_pct, idle_pct, iowait_pct, busy_pct, true};
                } else {
                    double a = ema_alpha_, b = 1.0 - a;
                    ema.user = a * user_pct + b * ema.user;
                    ema.system = a * sys_pct + b * ema.system;
                    ema.idle = a * idle_pct + b * ema.idle;
                    ema.iowait = a * iowait_pct + b * ema.iowait;
                    ema.busy = a * busy_pct + b * ema.busy;
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

    void EmitSystemCounters(DataBatchPtr& batch, const proc::CpuSnapshot& cur) {
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

    void EmitLoadAvg(DataBatchPtr& batch, const proc::LoadAvg& la) {
        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_utilization")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("loadavg")});
        rec.SetField(batch->InternString("load_1m"), la.load1);
        rec.SetField(batch->InternString("load_5m"), la.load5);
        rec.SetField(batch->InternString("load_15m"), la.load15);
    }

    void EmitRunQueue(DataBatchPtr& batch, const proc::CpuSnapshot& cur) {
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
        auto freqs = proc::ReadCpuFrequenciesMhz();
        for (size_t i = 0; i < freqs.size(); ++i) {
            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_utilization")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("cpu_freq")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString("cpu" + std::to_string(i))});
            rec.SetField(batch->InternString("freq_mhz"), freqs[i]);
        }
    }

    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    bool collect_freq_ = false;
    double ema_alpha_ = 0.0;

    bool has_prev_ = false;
    proc::CpuSnapshot prev_;
    std::vector<EmaState> ema_states_;
};

// 自动注册到 PluginRegistry
IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);

}  // namespace illuminator
