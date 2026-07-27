// ============================================================================
// CpuUtilizationSource — 系统级 CPU 利用率采集源
// ============================================================================
//
// 【功能概述】
//   从 /proc/stat 和 /proc/loadavg 采集系统整体的 CPU 利用率指标。
//   属于 Tier 1 监控（零侵入、低开销 < 0.1% CPU），
//   通过 TimerWheel 定时调用 Collect() 拉取数据。
//
// 【职责边界】
//   本 Source 仅负责系统级 CPU 指标。进程/线程级 CPU 采集由
//   独立的 ProcessCpuSource (process_cpu Feature) 负责。
//
// 【采集内容】
//   1. 系统级 CPU 分布：user / nice / system / idle / iowait / irq / softirq / steal
//   2. 系统计数器速率：上下文切换/s、中断/s
//   3. 运行队列状态：procs_running、procs_blocked
//   4. 系统负载：load_1m / load_5m / load_15m
//   5. Per-Core 数据（可选）：每个 CPU 核心的独立利用率
//   6. CPU 频率（可选）：每核当前运行频率 (MHz)
//
// 【数据格式】
//   每次 Collect() 返回一个 DataBatch(kMetrics)，内含多条 Record：
//   - labels[type="system_total"]  → 系统总计指标（1 条）
//   - labels[type="cpu_core", cpu="cpuN"] → 单核指标（N 条，可选）
//
// 【差分计算原理】
//   /proc/stat 中的 jiffies 是自系统启动以来的累计值。
//   需要保存前一次快照 (prev_)，通过 (cur - prev) / (total_cur - total_prev)
//   计算时间窗口内的百分比。首次调用时无前值可用，返回空批次。
//
// 【配置项】
//   interval_ms          采集间隔 (ms), 默认 1000
//   collect_per_core     是否采集每核数据, 默认 true
//   collect_frequency    是否采集 CPU 频率, 默认 false
//   ema_alpha            EMA 平滑系数, 默认 0 (不平滑)
// ============================================================================

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "core/common/data_batch.h"
#include "core/common/proc_reader.h"
#include "core/common/status.h"
#include "plugin/api/source_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class CpuUtilizationSource : public SourcePlugin {
public:
    const char* Name() const override { return "cpu_utilization"; }
    const char* Version() const override { return "1.0.0"; }

    // ----------------------------------------------------------------
    // Init — 从 ConfigValue 解析配置参数
    // ----------------------------------------------------------------
    Status Init(const ConfigValue& config) override {
        interval_ms_      = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_per_core_ = config["collect_per_core"].AsBool(true);
        collect_freq_     = config["collect_frequency"].AsBool(false);
        ema_alpha_        = config["ema_alpha"].AsDouble(0.0);
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    // ----------------------------------------------------------------
    // Collect — 核心采集入口
    // ----------------------------------------------------------------
    // 每次调用读取当前 /proc/stat 快照，与前一次快照做差分，
    // 计算各项百分比和速率指标，打包成 DataBatch 返回。
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        // 读取当前 CPU 快照
        auto cur = proc::ReadCpuSnapshot();
        if (cur.cores.empty()) {
            return Status::Error(StatusCode::kInternal, "Failed to read /proc/stat");
        }

        auto now_ts = std::chrono::steady_clock::now();

        // 首次调用：保存快照，返回空批次（无法计算差分）
        if (!has_prev_) {
            prev_ = cur;
            prev_time_ = now_ts;
            has_prev_ = true;
            return batch;
        }

        // 计算实际经过的墙钟时间（秒），用于速率计算
        double dt_sec = std::chrono::duration<double>(now_ts - prev_time_).count();
        if (dt_sec <= 0) dt_sec = static_cast<double>(interval_ms_) / 1000.0;

        // ① 系统总计指标（/proc/stat 第一行 "cpu" 总计）
        EmitSystemTotal(batch, cur, dt_sec);

        // ② Per-Core 指标（可选）
        if (collect_per_core_) {
            EmitPerCore(batch, cur);
        }

        // ③ 系统负载（/proc/loadavg），追加到 system_total 记录
        EmitLoadAvg(batch);

        // 保存当前快照供下次差分使用
        prev_ = cur;
        prev_time_ = now_ts;
        return batch;
    }

    // ----------------------------------------------------------------
    // Reconfigure — 运行时热更新配置
    // ----------------------------------------------------------------
    Status Reconfigure(const ConfigValue& params) override {
        if (!params["interval_ms"].IsNull())
            interval_ms_ = static_cast<uint32_t>(params["interval_ms"].AsInt(interval_ms_));
        if (!params["collect_per_core"].IsNull())
            collect_per_core_ = params["collect_per_core"].AsBool(collect_per_core_);
        if (!params["collect_frequency"].IsNull())
            collect_freq_ = params["collect_frequency"].AsBool(collect_freq_);
        if (!params["ema_alpha"].IsNull())
            ema_alpha_ = params["ema_alpha"].AsDouble(ema_alpha_);
        return Status::Ok();
    }

private:
    // ================================================================
    // 系统总计指标输出
    // ================================================================
    // 读取 /proc/stat 第一行 "cpu" 的总计 jiffies，
    // 与前一次快照做差分，计算各分量的百分比。
    void EmitSystemTotal(DataBatchPtr& batch, const proc::CpuSnapshot& cur, double dt_sec) {
        const auto& c = cur.cores[0];  // cores[0] = "cpu" 总计行
        const auto& p = prev_.cores[0];
        uint64_t dt = c.Total() - p.Total();
        if (dt == 0) return;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("system_total")});

        // 各分量百分比 = (cur_field - prev_field) / total_delta * 100
        auto pct = [&](uint64_t cv, uint64_t pv) -> double {
            return static_cast<double>(cv - pv) / static_cast<double>(dt) * 100.0;
        };

        double user   = pct(c.user,    p.user);
        double nice   = pct(c.nice,    p.nice);
        double system = pct(c.system,  p.system);
        double idle   = pct(c.idle,    p.idle);
        double iowait = pct(c.iowait,  p.iowait);
        double irq    = pct(c.irq,     p.irq);
        double softirq= pct(c.softirq, p.softirq);
        double steal  = pct(c.steal,   p.steal);
        double busy   = 100.0 - idle - iowait;

        // EMA 平滑（可选）：平滑掉瞬时抖动
        if (ema_alpha_ > 0 && has_ema_) {
            user    = Ema(user,    ema_user_);
            nice    = Ema(nice,    ema_nice_);
            system  = Ema(system,  ema_system_);
            idle    = Ema(idle,    ema_idle_);
            iowait  = Ema(iowait,  ema_iowait_);
            busy    = 100.0 - idle - iowait;
        }
        ema_user_ = user; ema_nice_ = nice; ema_system_ = system;
        ema_idle_ = idle; ema_iowait_ = iowait;
        has_ema_ = true;

        // 写入字段
        auto s = [&](const char* k) { return batch->InternString(k); };
        rec.SetField(s("user_pct"),    user);
        rec.SetField(s("nice_pct"),    nice);
        rec.SetField(s("system_pct"),  system);
        rec.SetField(s("idle_pct"),    idle);
        rec.SetField(s("iowait_pct"), iowait);
        rec.SetField(s("irq_pct"),     irq);
        rec.SetField(s("softirq_pct"), softirq);
        rec.SetField(s("steal_pct"),   steal);
        rec.SetField(s("busy_pct"),    busy);

        // 上下文切换和中断速率 = (cur_counter - prev_counter) / 时间间隔
        if (dt_sec > 0) {
            rec.SetField(s("ctxt_per_sec"),
                static_cast<double>(cur.ctxt - prev_.ctxt) / dt_sec);
            rec.SetField(s("intr_per_sec"),
                static_cast<double>(cur.intr - prev_.intr) / dt_sec);
        }

        // 运行队列状态（即时快照值，不做差分）
        rec.SetField(s("procs_running"), static_cast<uint64_t>(cur.procs_running));
        rec.SetField(s("procs_blocked"), static_cast<uint64_t>(cur.procs_blocked));
    }

    // ================================================================
    // Per-Core 指标输出
    // ================================================================
    // 为每个 CPU 核心输出独立的利用率 Record。
    // cores[0] 是总计行，cores[1..N] 对应 cpu0..cpuN-1。
    void EmitPerCore(DataBatchPtr& batch, const proc::CpuSnapshot& cur) {
        auto freqs = collect_freq_ ? proc::ReadCpuFrequenciesMhz() : std::vector<double>{};

        for (size_t i = 1; i < cur.cores.size() && i < prev_.cores.size(); ++i) {
            const auto& c = cur.cores[i];
            const auto& p = prev_.cores[i];
            uint64_t dt = c.Total() - p.Total();
            if (dt == 0) continue;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("cpu_core")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString(c.name.c_str())});

            auto pct = [&](uint64_t cv, uint64_t pv) -> double {
                return static_cast<double>(cv - pv) / static_cast<double>(dt) * 100.0;
            };

            auto s = [&](const char* k) { return batch->InternString(k); };
            rec.SetField(s("user_pct"),    pct(c.user,   p.user));
            rec.SetField(s("system_pct"),  pct(c.system, p.system));
            rec.SetField(s("idle_pct"),    pct(c.idle,   p.idle));
            rec.SetField(s("iowait_pct"), pct(c.iowait, p.iowait));
            rec.SetField(s("busy_pct"),
                100.0 - pct(c.idle, p.idle) - pct(c.iowait, p.iowait));

            // CPU 频率（索引 i-1，因为 cores[0] 是总计行不是核心）
            if (collect_freq_ && (i - 1) < freqs.size()) {
                rec.SetField(s("freq_mhz"), freqs[i - 1]);
            }
        }
    }

    // ================================================================
    // 系统负载输出
    // ================================================================
    // 读取 /proc/loadavg，将 1/5/15 分钟负载追加到 system_total Record 中。
    void EmitLoadAvg(DataBatchPtr& batch) {
        auto la = proc::ReadLoadAvg();
        if (!batch->records().empty()) {
            auto& rec = batch->records().front();
            auto s = [&](const char* k) { return batch->InternString(k); };
            rec.SetField(s("load_1m"),  la.load1);
            rec.SetField(s("load_5m"),  la.load5);
            rec.SetField(s("load_15m"), la.load15);
        }
    }

    // EMA 指数移动平均：alpha * new_value + (1 - alpha) * old_value
    double Ema(double value, double prev_ema) const {
        return ema_alpha_ * value + (1.0 - ema_alpha_) * prev_ema;
    }

    // ---- 配置参数 ----
    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    bool collect_freq_ = false;
    double ema_alpha_ = 0.0;

    // ---- 差分计算状态 ----
    bool has_prev_ = false;
    proc::CpuSnapshot prev_;
    std::chrono::steady_clock::time_point prev_time_;

    // ---- EMA 平滑状态 ----
    bool has_ema_ = false;
    double ema_user_ = 0, ema_nice_ = 0, ema_system_ = 0;
    double ema_idle_ = 0, ema_iowait_ = 0;
};

IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);

}  // namespace illuminator
