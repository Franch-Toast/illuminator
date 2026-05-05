// ============================================================================
// CpuSysMonitor — 系统级 CPU 监控数据源（旧版实现）
// ============================================================================
//
// 从 /proc/stat、/proc/loadavg、sysfs 三个内核接口采集系统级 CPU 指标。
// 这是第一代系统 CPU 监控实现，已被 CpuUtilizationSource（cpu_utilization）
// 取代，后者新增了 EMA 指数移动平均平滑功能。
//
// 新旧对比：
// ==========
// - cpu_sys_monitor（本文件）：基础 CPU 利用率、负载均值、频率（旧版）
// - cpu_utilization（新版）：在上述基础上增加了 EMA 平滑、更丰富的标签体系
//
// 采集指标：
// ==========
// 1. CPU 利用率明细（每个核心 + 总计）：
//    user_pct, nice_pct, system_pct, idle_pct, iowait_pct,
//    irq_pct, softirq_pct, steal_pct, busy_pct
//
// 2. 系统级计数器：
//    context_switches_per_sec（上下文切换/秒）
//    interrupts_per_sec（中断/秒）
//
// 3. 负载均值：
//    load_1m, load_5m, load_15m（来自 /proc/loadavg）
//
// 4. 运行队列：
//    procs_running（运行中进程数）, procs_blocked（阻塞中进程数）
//
// 5. CPU 频率（可选）：
//    freq_mhz（来自 /sys/devices/system/cpu/*/cpufreq/scaling_cur_freq）
//
// 工作原理：
// ==========
// 1. 每次 Collect() 调用时读取 /proc/stat、/proc/loadavg 的快照
// 2. 与上一轮的快照对比，计算各指标的差值 → 利用率百分比
// 3. /proc/stat 中 CPU 字段承诺为启动以来的累计值（jiffies），
//    差值法计算避免了对瞬时值的依赖
//
// 配置参数：
// ==========
// - interval_ms：采集间隔（默认 1000ms）
// - collect_per_core：是否输出每个核心的详细指标（默认 true）
// - collect_frequency：是否采集 CPU 频率（默认 false）
// ============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class CpuSysMonitor : public SourcePlugin {
public:
    const char* Name() const override { return "cpu_sys_monitor"; }
    const char* Version() const override { return "1.0.0"; }

    // ========================================================================
    // Init — 初始化监控器，读取首次快照作为基线
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_freq_ = config["collect_frequency"].AsBool(false);
        collect_per_core_ = config["collect_per_core"].AsBool(true);
        // 首次读取快照作为后续差值的基准
        ReadSnapshot(prev_);
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    // ========================================================================
    // Collect — 采集一轮系统 CPU 指标
    // ========================================================================
    // 读取当前快照，与上一轮快照对比生成所有指标。
    // 首次采集（has_prev_=false）时不会输出利用率和计数器
    // （因为没有基线可算差值），但会输出负载均值和运行队列。
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

        prev_ = cur;        // 保存当前快照作为下次基线
        has_prev_ = true;
        return batch;
    }

private:
    // ---- /proc/stat 中每个 CPU 核心的 jiffy 统计结构 ----
    struct CoreJiffies {
        std::string name;  // "cpu"（总计）或 "cpu0", "cpu1"...（单核）
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

        // 总 jiffy 数（所有状态时间之和）= user+nice+system+idle+iowait+irq+softirq+steal
        uint64_t Total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
        // 忙 jiffy 数 = 总时间 - 空闲 - IO等待（IO等待时 CPU 实际空闲）
        uint64_t Busy() const { return Total() - idle - iowait; }
    };

    // ---- /proc/stat 的完整快照结构 ----
    struct Snapshot {
        std::vector<CoreJiffies> cores;  // 所有 CPU 核心的 jiffy 统计
        uint64_t ctxt = 0;              // 上下文切换累计数
        uint64_t intr = 0;              // 中断累计数
        uint32_t procs_running = 0;     // 可运行（R 状态）进程数
        uint32_t procs_blocked = 0;     // 阻塞（D 或等待状态）进程数
    };

    // ========================================================================
    // ReadSnapshot — 从 /proc/stat 读取系统状态快照
    // ========================================================================
    // 解析 /proc/stat 中的以下行：
    // - cpuxxxx：CPU 核心的 jiffy 统计（每核一行）
    // - ctxt：系统启动以来累计的上下文切换次数
    // - intr：系统启动以来累计的中断次数
    // - procs_running：当前可运行进程数
    // - procs_blocked：当前阻塞进程数
    void ReadSnapshot(Snapshot& snap) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            // 匹配 "cpu " 或 "cpu0"..."cpuN"
            if (line.compare(0, 3, "cpu") == 0 &&
                (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
                CoreJiffies j;
                std::istringstream iss(line);
                // /proc/stat CPU 行格式：
                // cpu  user nice system idle iowait irq softirq steal
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

    // ========================================================================
    // EmitCpuUtilization — 输出 CPU 利用率指标
    // ========================================================================
    // 通过与上一轮快照对比，计算每个核心的各维度利用率百分比。
    // collect_per_core_=false 时只输出总计（"cpu"）行。
    void EmitCpuUtilization(DataBatchPtr& batch, const Snapshot& cur) {
        for (size_t i = 0; i < cur.cores.size(); ++i) {
            bool is_total = (cur.cores[i].name == "cpu");
            if (!is_total && !collect_per_core_) continue;
            if (i >= prev_.cores.size()) break;

            auto& c = cur.cores[i];
            auto& p = prev_.cores[i];
            if (c.name != p.name) continue;

            uint64_t dt = c.Total() - p.Total();  // 间隔内的总 jiffy 数
            if (dt == 0) dt = 1;

            // Lambda：计算某类时间的百分比 = delta(source) / delta(total) * 100
            auto pct = [dt](uint64_t cv, uint64_t pv) -> double {
                return 100.0 * static_cast<double>(cv - pv) / static_cast<double>(dt);
            };

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("cpu_sys_monitor")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString(is_total ? "cpu_total" : "cpu_core")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString(c.name)});

            rec.SetField(batch->InternString("user_pct"), pct(c.user, p.user));
            rec.SetField(batch->InternString("nice_pct"), pct(c.nice, p.nice));
            rec.SetField(batch->InternString("system_pct"), pct(c.system, p.system));
            rec.SetField(batch->InternString("idle_pct"), pct(c.idle, p.idle));
            rec.SetField(batch->InternString("iowait_pct"), pct(c.iowait, p.iowait));
            rec.SetField(batch->InternString("irq_pct"), pct(c.irq, p.irq));
            rec.SetField(batch->InternString("softirq_pct"), pct(c.softirq, p.softirq));
            rec.SetField(batch->InternString("steal_pct"), pct(c.steal, p.steal));
            rec.SetField(batch->InternString("busy_pct"),
                         100.0 * static_cast<double>(c.Busy() - p.Busy()) / static_cast<double>(dt));
        }
    }

    // ========================================================================
    // EmitSystemCounters — 输出每秒上下文切换和中断次数
    // ========================================================================
    // 将 ctxt 和 intr 的累计值之差除以间隔秒数得到每秒速率。
    void EmitSystemCounters(DataBatchPtr& batch, const Snapshot& cur) {
        double elapsed_sec = interval_ms_ / 1000.0;
        if (elapsed_sec <= 0) elapsed_sec = 1.0;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_sys_monitor")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("system_counters")});

        if (prev_.ctxt > 0) {
            rec.SetField(batch->InternString("context_switches_per_sec"),
                         static_cast<double>(cur.ctxt - prev_.ctxt) / elapsed_sec);
        }
        if (prev_.intr > 0) {
            rec.SetField(batch->InternString("interrupts_per_sec"),
                         static_cast<double>(cur.intr - prev_.intr) / elapsed_sec);
        }
    }

    // ========================================================================
    // EmitLoadAvg — 输出系统负载均值（1/5/15 分钟）
    // ========================================================================
    void EmitLoadAvg(DataBatchPtr& batch) {
        std::ifstream file("/proc/loadavg");
        if (!file.is_open()) return;

        double load1, load5, load15;
        file >> load1 >> load5 >> load15;

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_sys_monitor")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("loadavg")});
        rec.SetField(batch->InternString("load_1m"), load1);
        rec.SetField(batch->InternString("load_5m"), load5);
        rec.SetField(batch->InternString("load_15m"), load15);
    }

    // ========================================================================
    // EmitRunQueue — 输出当前运行队列长度
    // ========================================================================
    // procs_running：处于 TASK_RUNNING 状态（在运行队列等待或正在运行）
    // procs_blocked：处于 D 状态或不可中断睡眠
    void EmitRunQueue(DataBatchPtr& batch, const Snapshot& cur) {
        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("source"),
                              batch->InternString("cpu_sys_monitor")});
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("runqueue")});
        rec.SetField(batch->InternString("procs_running"),
                     static_cast<uint64_t>(cur.procs_running));
        rec.SetField(batch->InternString("procs_blocked"),
                     static_cast<uint64_t>(cur.procs_blocked));
    }

    // ========================================================================
    // EmitCpuFrequency — 输出各 CPU 核心的当前频率
    // ========================================================================
    // 从 sysfs 读取每个 CPU 核心的 cpufreq scaling_cur_freq（kHz），
    // 转换为 MHz 输出。遇到不存在的核心或缺少 cpufreq 驱动时退出循环。
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
                                  batch->InternString("cpu_sys_monitor")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("cpu_freq")});
            rec.labels.push_back({batch->InternString("cpu"),
                                  batch->InternString("cpu" + std::to_string(cpu_idx))});
            rec.SetField(batch->InternString("freq_mhz"),
                         static_cast<double>(freq_khz) / 1000.0);  // kHz → MHz
            ++cpu_idx;
        }
    }

    // ---- 配置参数 ----
    uint32_t interval_ms_ = 1000;     // 采集间隔（毫秒）
    bool collect_freq_ = false;       // 是否采集 CPU 频率
    bool collect_per_core_ = true;    // 是否按核心拆分输出
    bool has_prev_ = false;           // 是否有历史快照可供对比
    Snapshot prev_;                   // 上一次采集的快照
};

IL_REGISTER_SOURCE("cpu_sys_monitor", CpuSysMonitor);

}  // namespace illuminator
