// ============================================================================
// ProcStatReader — 通用 proc 文件系统读取器
// ============================================================================
//
// 从 Linux /proc 文件系统读取系统级 CPU 统计、内存信息和负载均值。
// 这是一个轻量级的 /proc 读取器，直接输出原始累计值（不做差值计算），
// 适合作为其他需要 /proc 数据的上游数据源，或用于简单的系统状态监控。
//
// 与其它 CPU 数据源的区别：
// ===========================
// - cpu_sys_monitor / cpu_utilization：读取 /proc/stat + /proc/loadavg，
//   通过前后快照差值计算利用率百分比 → 适合周期性监控图表
// - proc_stat_reader（本文件）：直接读取 /proc 文件的原始累计值，
//   不做差值计算 → 适合需要原始数据的场景，或一次性系统状态检查
//
// 采集指标：
// ==========
// 1. CPU 统计（/proc/stat 的 cpu* 行，只读第一块连续的 cpu 行）：
//    每条 Record 一个核心，字段为原始 jiffy 累计值：
//    user, nice, system, idle, iowait, irq, softirq, steal
//
// 2. 内存信息（/proc/meminfo）：
//    所有指标在一条 Record 中：
//    mem_total_kb, mem_free_kb, mem_available_kb,
//    buffers_kb, cached_kb, swap_total_kb, swap_free_kb
//
// 3. 负载均值（/proc/loadavg）：
//    load_1m, load_5m, load_15m
//
// 工作原理：
// ==========
// 每次 Collect() 调用时直接读取三个 proc 文件，将原始值填入 DataBatch。
// 不做任何历史对比或计算，完全无状态（除了 interval_ms 配置）。
//
// 配置参数：
// ==========
// - interval_ms：采集间隔（默认 1000ms，仅影响调度频率，不影响输出内容）
// ============================================================================

#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class ProcStatReader : public SourcePlugin {
public:
    const char* Name() const override { return "proc_stat_reader"; }
    const char* Version() const override { return "0.1.0"; }

    // ========================================================================
    // Init — 初始化读取器，配置采集间隔
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(1000));
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    // ========================================================================
    // Collect — 采集一轮 proc 文件的原始数据
    // ========================================================================
    // 依次从三个来源采集数据并填入同一个 batch：
    // 1. /proc/stat → CPU 累计 jiffy 值
    // 2. /proc/meminfo → 内存使用统计
    // 3. /proc/loadavg → 系统负载均值
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        CollectCpuStats(batch);
        CollectMemInfo(batch);
        CollectLoadAvg(batch);

        return batch;
    }

private:
    // ========================================================================
    // CollectCpuStats — 从 /proc/stat 读取 CPU 累计 jiffy 值
    // ========================================================================
    // 读取 /proc/stat 中以 "cpu" 开头的连续行，直到遇到非 cpu 行。
    // 每个 CPU 核心输出原始累计值（不做差值计算）。
    // 注意：此方法只读取第一块连续的 cpu 行，如果后续有其它数据再出现
    // cpu 行（极少见），会停止读取。
    void CollectCpuStats(DataBatchPtr& batch) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.substr(0, 3) != "cpu") break;  // 遇到非 cpu 行停止

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

    // ========================================================================
    // CollectMemInfo — 从 /proc/meminfo 读取内存统计
    // ========================================================================
    // 解析 /proc/meminfo 中的关键内存字段，将多个指标放在同一条 Record 中。
    // 采集的字段：
    //   MemTotal → mem_total_kb（总内存）
    //   MemFree → mem_free_kb（空闲内存）
    //   MemAvailable → mem_available_kb（可用内存，含可回收的缓存）
    //   Buffers → buffers_kb（缓冲区）
    //   Cached → cached_kb（页缓存）
    //   SwapTotal → swap_total_kb（交换空间总量）
    //   SwapFree → swap_free_kb（交换空间空闲量）
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

            // 按字段名匹配
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

    // ========================================================================
    // CollectLoadAvg — 从 /proc/loadavg 读取系统负载均值
    // ========================================================================
    // 读取 1 分钟、5 分钟、15 分钟三个时间窗口的负载均值。
    // 负载均值表示平均活跃进程数（正在运行 + 不可中断等待）。
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
