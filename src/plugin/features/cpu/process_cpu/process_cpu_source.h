// ============================================================================
// ProcessCpuSource — 进程/线程级 CPU 利用率采集源
// ============================================================================
//
// 【功能概述】
//   扫描 /proc 目录下所有进程，计算每个进程的用户态和内核态 CPU 占用率，
//   按 CPU 占比降序排列后保留 Top-N 个进程上报。
//   属于 Tier 1 监控，与 cpu_utilization (系统级) 配合使用。
//
// 【职责边界】
//   本 Source 仅负责进程/线程级 CPU 指标。
//   系统级 CPU 分布由 CpuUtilizationSource (cpu_utilization Feature) 负责。
//
// 【采集内容】
//   定时采集（Collect）：
//     - Top-N 进程的 CPU 占用率（user + sys），按降序排列
//     - 每个进程的状态、线程数、内存占用、上下文切换
//   按需查询（QueryExtra）：
//     - 指定进程的线程级 CPU 详情（遍历 /proc/[pid]/task/）
//
// 【数据格式】
//   每次 Collect() 返回 DataBatch(kMetrics)：
//   - labels[type="process", pid="...", comm="..."] → 进程指标（最多 N 条）
//
// 【差分计算原理】
//   进程的 utime/stime 是 jiffies 累计值。
//   通过 /proc/stat 第一行获取系统总 jiffies 增量作为分母，
//   进程 jiffies 增量作为分子，乘以 CPU 核数得到与 top 命令一致的百分比。
//
// 【配置项】
//   interval_ms              采集间隔 (ms), 默认 2000
//   top_n                    保留 Top-N 进程, 默认 20
//   min_cpu_threshold        最小上报阈值 (%), 默认 0.1
//   comm_filter              进程名正则过滤, 默认空 (全部)
//   exclude_kernel_threads   排除内核线程, 默认 true
// ============================================================================

#pragma once

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/data_batch.h"
#include "core/common/proc_reader.h"
#include "core/common/status.h"
#include "plugin/api/source_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class ProcessCpuSource : public SourcePlugin {
public:
    const char* Name() const override { return "process_cpu"; }
    const char* Version() const override { return "1.0.0"; }

    // ----------------------------------------------------------------
    // Init — 从 ConfigValue 解析配置参数
    // ----------------------------------------------------------------
    Status Init(const ConfigValue& config) override {
        interval_ms_       = static_cast<uint32_t>(config["interval_ms"].AsInt(2000));
        top_n_             = static_cast<int>(config["top_n"].AsInt(20));
        min_cpu_threshold_ = config["min_cpu_threshold"].AsDouble(0.1);
        exclude_kthreads_  = config["exclude_kernel_threads"].AsBool(true);

        auto filter = config["comm_filter"].AsString("");
        if (!filter.empty()) {
            try {
                comm_regex_ = std::regex(filter);
                has_comm_filter_ = true;
            } catch (const std::regex_error&) {
                return Status::Error(StatusCode::kInvalidArgument,
                    "Invalid comm_filter regex: " + filter);
            }
        }

        num_cpus_ = proc::CountOnlineCpus();
        return Status::Ok();
    }

    // 进程扫描开销比 /proc/stat 大，默认 2s 间隔
    uint32_t IntervalMs() const override { return interval_ms_; }

    // ----------------------------------------------------------------
    // Collect — 扫描进程并计算 Top-N CPU 占用
    // ----------------------------------------------------------------
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        uint64_t cur_total = proc::ReadTotalCpuJiffies();
        auto cur_procs = ScanFilteredProcesses();

        // 首次调用：保存基线，返回空批次
        if (!has_prev_) {
            prev_total_jiffies_ = cur_total;
            prev_processes_ = std::move(cur_procs);
            has_prev_ = true;
            return batch;
        }

        uint64_t total_delta = cur_total - prev_total_jiffies_;
        if (total_delta == 0) {
            prev_total_jiffies_ = cur_total;
            return batch;
        }

        // 构建 PID → 前一次快照的映射
        std::unordered_map<uint32_t, const proc::ProcessStat*> prev_map;
        for (const auto& ps : prev_processes_) {
            prev_map[ps.pid] = &ps;
        }

        // 计算每个进程的 CPU 增量百分比
        struct ProcCpu {
            uint32_t pid;
            std::string comm;
            double cpu_user_pct;
            double cpu_sys_pct;
            double cpu_total_pct;
            char state;
            uint32_t num_threads;
            uint64_t rss_kb;
            uint64_t vsize_kb;
            uint64_t vol_csw;
            uint64_t nonvol_csw;
        };
        std::vector<ProcCpu> ranked;

        for (const auto& cur_ps : cur_procs) {
            auto it = prev_map.find(cur_ps.pid);
            if (it == prev_map.end()) continue;
            const auto& prev_ps = *it->second;

            // jiffies 差值 → 百分比（× CPU 核数 = 与 top 一致的多核百分比）
            uint64_t user_delta = cur_ps.utime - prev_ps.utime;
            uint64_t sys_delta  = cur_ps.stime - prev_ps.stime;

            double user_pct = static_cast<double>(user_delta)
                            / static_cast<double>(total_delta) * 100.0
                            * static_cast<double>(num_cpus_);
            double sys_pct  = static_cast<double>(sys_delta)
                            / static_cast<double>(total_delta) * 100.0
                            * static_cast<double>(num_cpus_);
            double total_pct = user_pct + sys_pct;

            // 低于阈值的进程不上报（减少 SSE 流量）
            if (total_pct < min_cpu_threshold_) continue;

            ProcCpu pc;
            pc.pid = cur_ps.pid;
            pc.comm = cur_ps.comm;
            pc.cpu_user_pct = user_pct;
            pc.cpu_sys_pct = sys_pct;
            pc.cpu_total_pct = total_pct;
            pc.state = cur_ps.state;
            pc.num_threads = cur_ps.num_threads;
            pc.rss_kb = static_cast<uint64_t>(cur_ps.rss_pages) * 4;
            pc.vsize_kb = cur_ps.vsize / 1024;
            pc.vol_csw = cur_ps.voluntary_ctxt_switches;
            pc.nonvol_csw = cur_ps.nonvoluntary_ctxt_switches;
            ranked.push_back(std::move(pc));
        }

        // 按 CPU 总占比降序，取 Top-N
        int limit = std::min(static_cast<int>(ranked.size()), top_n_);
        std::partial_sort(ranked.begin(), ranked.begin() + limit, ranked.end(),
            [](const ProcCpu& a, const ProcCpu& b) {
                return a.cpu_total_pct > b.cpu_total_pct;
            });

        // 写入 DataBatch
        for (int i = 0; i < limit; ++i) {
            const auto& pc = ranked[i];
            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("process")});
            rec.labels.push_back({batch->InternString("pid"),
                                  batch->InternString(std::to_string(pc.pid).c_str())});
            rec.labels.push_back({batch->InternString("comm"),
                                  batch->InternString(pc.comm.c_str())});

            auto s = [&](const char* k) { return batch->InternString(k); };
            rec.SetField(s("cpu_user_pct"),  pc.cpu_user_pct);
            rec.SetField(s("cpu_sys_pct"),   pc.cpu_sys_pct);
            rec.SetField(s("cpu_total_pct"), pc.cpu_total_pct);
            rec.SetField(s("state"),
                batch->InternString(std::string(1, pc.state).c_str()));
            rec.SetField(s("num_threads"),  static_cast<uint64_t>(pc.num_threads));
            rec.SetField(s("rss_kb"),       pc.rss_kb);
            rec.SetField(s("vsize_kb"),     pc.vsize_kb);
            rec.SetField(s("vol_csw"),      pc.vol_csw);
            rec.SetField(s("nonvol_csw"),   pc.nonvol_csw);
        }

        // 更新前值
        prev_total_jiffies_ = cur_total;
        prev_processes_ = std::move(cur_procs);
        return batch;
    }

    // ----------------------------------------------------------------
    // Reconfigure — 运行时热更新
    // ----------------------------------------------------------------
    Status Reconfigure(const ConfigValue& params) override {
        if (!params["interval_ms"].IsNull())
            interval_ms_ = static_cast<uint32_t>(params["interval_ms"].AsInt(interval_ms_));
        if (!params["top_n"].IsNull())
            top_n_ = static_cast<int>(params["top_n"].AsInt(top_n_));
        if (!params["min_cpu_threshold"].IsNull())
            min_cpu_threshold_ = params["min_cpu_threshold"].AsDouble(min_cpu_threshold_);
        return Status::Ok();
    }

    // ----------------------------------------------------------------
    // QueryExtra — 线程级 CPU 详情（按需查询）
    // ----------------------------------------------------------------
    // 前端点击某进程后，调用 QueryExtra("threads", {pid=1234})
    // 返回该进程下所有线程的 utime/stime/state。
    StatusOr<std::string> QueryExtra(
        const std::string& query, const QueryParams& params) override {

        if (query == "threads") {
            auto it = params.find("pid");
            if (it == params.end()) {
                return Status::Error(StatusCode::kInvalidArgument, "Missing pid parameter");
            }
            uint32_t pid = static_cast<uint32_t>(std::stoul(it->second));
            return QueryThreads(pid);
        }
        return Status::Error(StatusCode::kUnimplemented, "Unknown query: " + query);
    }

private:
    // ================================================================
    // 线程级查询实现
    // ================================================================
    // 遍历 /proc/<pid>/task/ 目录下所有线程 TID，
    // 解析每个线程的 /proc/<pid>/task/<tid>/stat 文件，
    // 返回包含所有线程 CPU 时间的 JSON。
    std::string QueryThreads(uint32_t pid) {
        std::string task_dir = "/proc/" + std::to_string(pid) + "/task";
        DIR* dir = opendir(task_dir.c_str());
        if (!dir) {
            return R"({"error":"Process not found or no permission"})";
        }

        std::string json = R"({"pid":)" + std::to_string(pid) + R"(,"threads":[)";
        bool first = true;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char* endp;
            long tid = strtol(entry->d_name, &endp, 10);
            if (*endp != '\0' || tid <= 0) continue;

            proc::ProcessStat ts;
            std::string path = task_dir + "/" + entry->d_name + "/stat";
            if (!proc::ParseProcStat(path, ts)) continue;

            if (!first) json += ",";
            first = false;
            json += R"({"tid":)" + std::to_string(tid)
                  + R"(,"comm":")" + ts.comm
                  + R"(","state":")" + std::string(1, ts.state)
                  + R"(","utime":)" + std::to_string(ts.utime)
                  + R"(,"stime":)" + std::to_string(ts.stime)
                  + "}";
        }
        closedir(dir);
        json += "]}";
        return json;
    }

    // ================================================================
    // 进程扫描（含过滤逻辑）
    // ================================================================
    std::vector<proc::ProcessStat> ScanFilteredProcesses() {
        auto all = proc::ScanProcesses();
        if (!exclude_kthreads_ && !has_comm_filter_) return all;

        std::vector<proc::ProcessStat> filtered;
        filtered.reserve(all.size());
        for (auto& ps : all) {
            // 内核线程特征：vsize == 0（除 PID 1 init 外）
            if (exclude_kthreads_ && ps.vsize == 0 && ps.pid != 1) continue;

            if (has_comm_filter_ && !std::regex_search(ps.comm, comm_regex_)) continue;

            proc::ReadCtxtSwitches(ps.pid, ps);
            filtered.push_back(std::move(ps));
        }
        return filtered;
    }

    // ---- 配置参数 ----
    uint32_t interval_ms_ = 2000;
    int top_n_ = 20;
    double min_cpu_threshold_ = 0.1;
    bool exclude_kthreads_ = true;
    bool has_comm_filter_ = false;
    std::regex comm_regex_;
    int num_cpus_ = 1;

    // ---- 差分计算状态 ----
    bool has_prev_ = false;
    uint64_t prev_total_jiffies_ = 0;
    std::vector<proc::ProcessStat> prev_processes_;
};

IL_REGISTER_SOURCE("process_cpu", ProcessCpuSource);

}  // namespace illuminator
