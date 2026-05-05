// ============================================================================
// CpuSysStats — CPU 系统统计（简化版）
// ============================================================================
//
// 简化的系统级 CPU 统计数据源，从 /proc/stat 和 /proc/loadavg 采集
// 各 CPU 核心的利用率分解和负载均值。相比 CpuSysMonitor 进一步简化：
// 不保留运行队列指标，采用更轻量的数据结构（pair<string, CoreJiffies>）。
//
// 采集指标：
// ==========
// 1. CPU 利用率明细（每个核心 + 总计）：
//    user_pct, nice_pct, system_pct, idle_pct, iowait_pct,
//    irq_pct, softirq_pct, steal_pct
//
// 2. 系统计数器（可选）：
//    context_switches_per_sec, interrupts_per_sec
//
// 3. 负载均值：
//    load_1m, load_5m, load_15m
//
// 4. CPU 频率（可选）：
//    freq_mhz（每个核心的当前工作频率）
//
// 工作原理：
// ==========
// 1. CollectCpuJiffies() 读取 /proc/stat 各核心的 jiffy 计数
// 2. 首次采集（first_collect_=true）只记录基线，不输出指标
// 3. 后续采集通过 FindCore() 查找同名核心的前后 jiffy 差值计算百分比
// 4. 独立的 CollectContextSwitches / CollectLoadAvg / CollectCpuFrequency
//    分别从不同来源采集各自指标
//
// 配置参数：
// ==========
// - interval_ms：采集间隔（默认 1000ms）
// - collect_frequency：是否采集 CPU 频率（默认 false）
// - collect_interrupts：是否采集中断和上下文切换（默认 false）
// ============================================================================

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

    // ========================================================================
    // Init — 初始化简化版 CPU 统计采集器
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        collect_freq_ = config["collect_frequency"].AsBool(false);
        collect_interrupts_ = config["collect_interrupts"].AsBool(false);
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    // ========================================================================
    // Collect — 采集一轮 CPU 统计指标
    // ========================================================================
    // 每次调用依次采集：CPU jiffy → 上下文切换 → 负载均值 → CPU 频率（可选）。
    // 首次采集仅记录基线（不输出指标），后续采集输出各项百分比。
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
    // ---- CPU 核心 jiffy 统计（不含名称，名称为外层 pair） ----
    struct CpuCoreJiffies {
        uint64_t user = 0, nice = 0, system = 0, idle = 0;
        uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
        uint64_t Total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
    };

    // ---- 完整 jiffy 快照（核心列表 + 系统计数器） ----
    struct CpuJiffies {
        std::vector<std::pair<std::string, CpuCoreJiffies>> cores;
        uint64_t ctxt = 0;  // 上下文切换累计数
        uint64_t intr = 0;  // 中断累计数
    };

    // ========================================================================
    // CollectCpuJiffies — 从 /proc/stat 读取 CPU jiffy 统计并输出指标
    // ========================================================================
    // 解析每个 CPU 核心行和 ctxt/intr 行存入 snapshot。
    // 非首次采集时，与上一轮的同名核心对比，计算各维度利用率百分比。
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

        // 首次采集只记录基线，不输出指标
        if (first_collect_) return;

        // 对比新老快照，输出各核心利用率
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

            // Lambda：计算某类时间的利用率百分比
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

    // ========================================================================
    // CollectContextSwitches — 采集上下文切换和中断速率
    // ========================================================================
    // 重新读取 /proc/stat 中的 ctxt 和 intr 行（与 CollectCpuJiffies 独立），
    // 计算与上一轮的差值除以时间间隔得到每秒速率。
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

    // ========================================================================
    // CollectLoadAvg — 从 /proc/loadavg 采集系统负载均值
    // ========================================================================
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

    // ========================================================================
    // CollectCpuFrequency — 从 sysfs 采集各核心当前工作频率
    // ========================================================================
    // 读取 /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq，
    // 将 kHz 转为 MHz 输出。遇到不存在的核心时退出。
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

    // ========================================================================
    // FindCore — 在快照中按名称查找核心的 jiffy 统计
    // ========================================================================
    // 静态辅助函数，遍历快照的 cores 列表查找指定名称的核心。
    // 返回找到的 CpuCoreJiffies 指针，未找到返回 nullptr。
    static const CpuCoreJiffies* FindCore(const CpuJiffies& snap,
                                           const std::string& name) {
        for (auto& [n, j] : snap.cores) {
            if (n == name) return &j;
        }
        return nullptr;
    }

    // ---- 配置参数 ----
    uint32_t interval_ms_ = 1000;       // 采集间隔（毫秒）
    bool collect_freq_ = false;         // 是否采集 CPU 频率
    bool collect_interrupts_ = false;   // 是否采集中断计数
    bool first_collect_ = true;         // 是否首次采集（首次只记录基线）
    CpuJiffies prev_;                   // 上一次采集的快照
};

IL_REGISTER_SOURCE("cpu_sys_stats", CpuSysStats);

}  // namespace illuminator
