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

    // 初始化：读取配置参数，进行首次快照采集以建立基线
    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_per_core_ = config["collect_per_core"].AsBool(true);
        collect_freq_ = config["collect_frequency"].AsBool(false);
        ema_alpha_ = config["ema_alpha"].AsDouble(0.0);
        ReadSnapshot(prev_);  // 首次快照用于后续差值计算
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    // 采集方法：读取当前快照，与上一次对比生成指标
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        Snapshot cur;
        ReadSnapshot(cur);

        if (has_prev_) {
            // 只在有历史快照时才计算利用率（需要差值）
            EmitCpuUtilization(batch, cur);
            EmitSystemCounters(batch, cur);
        }
        EmitLoadAvg(batch);
        EmitRunQueue(batch, cur);
        if (collect_freq_) EmitCpuFrequency(batch);

        prev_ = cur;       // 保存当前快照作为下次的基线
        has_prev_ = true;
        return batch;
    }

private:
    // ---- /proc/stat 中每核的 CPU 时间统计 ----
    // 这些值是系统启动以来的累计 jiffy 数（通常 100 jiffies/秒）
    struct CoreJiffies {
        std::string name;  // "cpu"（总计）或 "cpu0", "cpu1"...（单核）
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

        uint64_t Total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
        // 忙时间 = 总时间 - 空闲 - IO等待（io等待时 CPU 实际是空闲的）
        uint64_t Busy() const { return Total() - idle - iowait; }
    };

    struct Snapshot {
        std::vector<CoreJiffies> cores;  // 所有 CPU 核的 jiffy 统计
        uint64_t ctxt = 0;              // 上下文切换累计数
        uint64_t intr = 0;              // 中断累计数
        uint32_t procs_running = 0;     // 可运行状态的进程数
        uint32_t procs_blocked = 0;     // 阻塞状态的进程数
    };

    // EMA 状态：记录每个核的平滑值
    struct EmaState {
        double user = 0, system = 0, idle = 0, iowait = 0, busy = 0;
        bool initialized = false;
    };

    // ---- 从 /proc/stat 读取当前系统快照 ----
    void ReadSnapshot(Snapshot& snap) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            // 匹配 "cpu "（总计）或 "cpu0", "cpu1"...（单核）
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

    // ---- 输出 CPU 利用率指标 ----
    // 通过计算前后快照的 jiffy 差值得到各维度百分比
    void EmitCpuUtilization(DataBatchPtr& batch, const Snapshot& cur) {
        // 确保 EMA 状态数组与核心数一致
        if (ema_states_.size() < cur.cores.size())
            ema_states_.resize(cur.cores.size());

        for (size_t i = 0; i < cur.cores.size(); ++i) {
            bool is_total = (cur.cores[i].name == "cpu");
            if (!is_total && !collect_per_core_) continue;  // 只留总计
            if (i >= prev_.cores.size()) break;

            auto& c = cur.cores[i];
            auto& p = prev_.cores[i];
            if (c.name != p.name) continue;

            uint64_t dt = c.Total() - p.Total();  // 时间间隔内的总 jiffy 数
            if (dt == 0) dt = 1;

            // 计算单项百分比 = delta / total_delta * 100
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

            // ---- EMA 指数移动平均平滑 ----
            if (ema_alpha_ > 0.0 && i < ema_states_.size()) {
                auto& ema = ema_states_[i];
                if (!ema.initialized) {
                    // 首次直接赋当前值
                    ema.user = user_pct;
                    ema.system = sys_pct;
                    ema.idle = idle_pct;
                    ema.iowait = iowait_pct;
                    ema.busy = busy_pct;
                    ema.initialized = true;
                } else {
                    // EMA 公式：新值 = α × 当前值 + (1-α) × 旧EMA
                    ema.user = ema_alpha_ * user_pct + (1.0 - ema_alpha_) * ema.user;
                    ema.system = ema_alpha_ * sys_pct + (1.0 - ema_alpha_) * ema.system;
                    ema.idle = ema_alpha_ * idle_pct + (1.0 - ema_alpha_) * ema.idle;
                    ema.iowait = ema_alpha_ * iowait_pct + (1.0 - ema_alpha_) * ema.iowait;
                    ema.busy = ema_alpha_ * busy_pct + (1.0 - ema_alpha_) * ema.busy;
                }
            }

            // 构造记录并填充所有指标
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

            // 附加 EMA 平滑值
            if (ema_alpha_ > 0.0 && i < ema_states_.size() && ema_states_[i].initialized) {
                auto& ema = ema_states_[i];
                rec.SetField(batch->InternString("user_pct_ema"), ema.user);
                rec.SetField(batch->InternString("system_pct_ema"), ema.system);
                rec.SetField(batch->InternString("busy_pct_ema"), ema.busy);
            }
        }
    }

    // ---- 输出每秒上下文切换和中断次数 ----
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

    // ---- 输出系统负载均值 ----
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

    // ---- 输出运行队列统计 ----
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

    // ---- 输出 CPU 当前频率 ----
    void EmitCpuFrequency(DataBatchPtr& batch) {
        int cpu_idx = 0;
        while (true) {
            std::string path = "/sys/devices/system/cpu/cpu" +
                               std::to_string(cpu_idx) + "/cpufreq/scaling_cur_freq";
            std::ifstream file(path);
            if (!file.is_open()) break;  // CPU 不存在或无 cpufreq 驱动

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
                         static_cast<double>(freq_khz) / 1000.0);  // kHz → MHz
            ++cpu_idx;
        }
    }

    // 配置参数
    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    bool collect_freq_ = false;
    double ema_alpha_ = 0.0;

    // 状态变量
    bool has_prev_ = false;
    Snapshot prev_;                       // 上一次采集的快照
    std::vector<EmaState> ema_states_;    // 每个核的 EMA 状态
};

// 自动注册到 PluginRegistry
IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);

}  // namespace illuminator
