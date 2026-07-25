// ============================================================================
// ProcessCpuSource — 按进程/线程的 CPU 利用率数据源（新一代实现）
// ============================================================================
//
// 通过扫描 /proc 目录获取每个进程的 CPU 利用率、内存使用、上下文切换等指标。
//
// 工作流程：
// ==========
// 1. 读取当前整个系统的 CPU jiffy 总量（/proc/stat 第一行）
// 2. 扫描 /proc 获取所有进程的快照（/proc/[pid]/stat 和 /proc/[pid]/status）
// 3. 与上一轮快照对比，计算各进程的 CPU 时间差 → 利用率百分比
// 4. 按 total CPU% 降序排序，保留 Top-N 个进程
// 5. 对高 CPU 进程或指定进程展开线程级详情
//
// 支持过滤：
// ==========
// - include_comm_regex: 只采集名称匹配正则的进程
// - exclude_comm_regex: 排除名称匹配正则的进程
//
// 线程详情展开条件：
// ==================
// - 进程在 thread_detail_pids 白名单中（精确匹配）
// - 进程 CPU 使用率 >= thread_detail_threshold_pct（阈值触发）
//
// 输出指标（每条 Record）：
// ==========================
// 标签: source="process_cpu", type="process", pid, comm
// 字段: cpu_user_pct, cpu_sys_pct, cpu_total_pct, state,
//        num_threads, rss_kB, vsize_kB,
//        voluntary_ctxt_switches, nonvoluntary_ctxt_switches
//
// 线程级记录额外标签: type="thread", tid, parent_comm
// ============================================================================

#pragma once

#include <algorithm>
#include <cstdint>
#include <dirent.h>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/common/logging.h"
#include "core/common/proc_reader.h"
#include "plugin/api/source_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class ProcessCpuSource : public SourcePlugin {
public:
    const char* Name() const override { return "process_cpu"; }
    const char* Version() const override { return "1.0.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(2000));
        top_n_ = static_cast<uint32_t>(config["top_n"].AsInt(50));
        thread_detail_threshold_pct_ = config["thread_detail_threshold_pct"].AsDouble(3.0);

        auto include_re = config["include_comm_regex"].AsString("");
        auto exclude_re = config["exclude_comm_regex"].AsString("");
        if (!include_re.empty() && include_re != ".*") {
            try { include_regex_ = std::regex(include_re); has_include_regex_ = true; }
            catch (const std::exception& e) {
                IL_WARN("process_cpu: invalid include_comm_regex '{}': {}", include_re, e.what());
            }
        }
        if (!exclude_re.empty()) {
            try { exclude_regex_ = std::regex(exclude_re); has_exclude_regex_ = true; }
            catch (const std::exception& e) {
                IL_WARN("process_cpu: invalid exclude_comm_regex '{}': {}", exclude_re, e.what());
            }
        }

        auto pids_str = config["thread_detail_pids"].AsString("");
        if (!pids_str.empty()) {
            std::istringstream iss(pids_str);
            std::string tok;
            while (std::getline(iss, tok, ',')) {
                try { thread_detail_pids_.insert(static_cast<uint32_t>(std::stoul(tok))); }
                catch (...) {}
            }
        }

        prev_total_jiffies_ = proc::ReadTotalCpuJiffies();
        num_cpus_ = proc::CountOnlineCpus();
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        uint64_t cur_total_jiffies = proc::ReadTotalCpuJiffies();
        uint64_t delta_total = cur_total_jiffies - prev_total_jiffies_;
        if (delta_total == 0) delta_total = 1;

        auto raw_processes = proc::ScanProcesses();

        struct RankedProcess {
            proc::ProcessStat snap;
            double cpu_user_pct = 0, cpu_sys_pct = 0, cpu_total_pct = 0;
            bool has_prev = false;
        };

        std::vector<RankedProcess> ranked;
        ranked.reserve(raw_processes.size());

        for (auto& ps : raw_processes) {
            if (has_include_regex_ && !std::regex_search(ps.comm, include_regex_))
                continue;
            if (has_exclude_regex_ && std::regex_search(ps.comm, exclude_regex_))
                continue;

            RankedProcess rp;
            rp.snap = ps;

            auto it = prev_snapshots_.find(ps.pid);
            if (it != prev_snapshots_.end()) {
                auto& prev = it->second;
                uint64_t du = ps.utime > prev.utime ? ps.utime - prev.utime : 0;
                uint64_t ds = ps.stime > prev.stime ? ps.stime - prev.stime : 0;
                rp.cpu_user_pct = 100.0 * static_cast<double>(du) /
                                  static_cast<double>(delta_total) * num_cpus_;
                rp.cpu_sys_pct = 100.0 * static_cast<double>(ds) /
                                 static_cast<double>(delta_total) * num_cpus_;
                rp.cpu_total_pct = rp.cpu_user_pct + rp.cpu_sys_pct;
                rp.has_prev = true;
            }
            ranked.push_back(rp);
        }

        std::sort(ranked.begin(), ranked.end(),
                  [](const RankedProcess& a, const RankedProcess& b) {
                      return a.cpu_total_pct > b.cpu_total_pct;
                  });
        if (ranked.size() > top_n_)
            ranked.resize(top_n_);

        for (auto& rp : ranked) {
            if (!rp.has_prev) continue;
            auto& ps = rp.snap;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("process_cpu")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("process")});
            rec.labels.push_back({batch->InternString("pid"),
                                  batch->InternString(std::to_string(ps.pid))});
            rec.labels.push_back({batch->InternString("comm"),
                                  batch->InternString(ps.comm)});

            rec.SetField(batch->InternString("cpu_user_pct"), rp.cpu_user_pct);
            rec.SetField(batch->InternString("cpu_sys_pct"), rp.cpu_sys_pct);
            rec.SetField(batch->InternString("cpu_total_pct"), rp.cpu_total_pct);
            rec.SetField(batch->InternString("state"),
                         batch->InternString(std::string(1, ps.state)));
            rec.SetField(batch->InternString("num_threads"),
                         static_cast<uint64_t>(ps.num_threads));
            rec.SetField(batch->InternString("rss_kb"),
                         static_cast<uint64_t>(ps.rss_pages * 4));
            rec.SetField(batch->InternString("vsize_kb"), ps.vsize / 1024);
            rec.SetField(batch->InternString("voluntary_ctxt_switches"),
                         ps.voluntary_ctxt_switches);
            rec.SetField(batch->InternString("nonvoluntary_ctxt_switches"),
                         ps.nonvoluntary_ctxt_switches);

            bool expand_threads =
                thread_detail_pids_.count(ps.pid) > 0 ||
                rp.cpu_total_pct >= thread_detail_threshold_pct_;
            if (expand_threads)
                CollectThreads(batch, ps.pid, ps.comm, delta_total);
        }

        prev_snapshots_.clear();
        for (auto& ps : raw_processes)
            prev_snapshots_[ps.pid] = ps;
        prev_total_jiffies_ = cur_total_jiffies;

        return batch;
    }

private:
    struct ThreadSnapshot {
        uint32_t tid = 0;
        std::string comm;
        uint64_t utime = 0, stime = 0;
        char state = '?';
    };

    void CollectThreads(DataBatchPtr& batch, uint32_t pid,
                        const std::string& parent_comm, uint64_t delta_total) {
        std::string task_dir = "/proc/" + std::to_string(pid) + "/task";
        DIR* dir = opendir(task_dir.c_str());
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char* endp;
            long tid = strtol(entry->d_name, &endp, 10);
            if (*endp != '\0' || tid <= 0) continue;

            proc::ProcessStat ts;
            ts.pid = static_cast<uint32_t>(tid);
            if (!proc::ParseProcStat(task_dir + "/" + entry->d_name + "/stat", ts))
                continue;

            double t_user_pct = 0, t_sys_pct = 0;
            auto key = MakeThreadKey(pid, static_cast<uint32_t>(tid));
            bool had_prev = false;

            auto it = prev_thread_snapshots_.find(key);
            if (it != prev_thread_snapshots_.end()) {
                uint64_t du = ts.utime > it->second.utime ? ts.utime - it->second.utime : 0;
                uint64_t ds = ts.stime > it->second.stime ? ts.stime - it->second.stime : 0;
                t_user_pct = 100.0 * static_cast<double>(du) /
                             static_cast<double>(delta_total) * num_cpus_;
                t_sys_pct = 100.0 * static_cast<double>(ds) /
                            static_cast<double>(delta_total) * num_cpus_;
                had_prev = true;
            }

            prev_thread_snapshots_[key] = {
                static_cast<uint32_t>(tid), ts.comm, ts.utime, ts.stime, ts.state};

            if (!had_prev) continue;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("process_cpu")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("thread")});
            rec.labels.push_back({batch->InternString("pid"),
                                  batch->InternString(std::to_string(pid))});
            rec.labels.push_back({batch->InternString("tid"),
                                  batch->InternString(std::to_string(tid))});
            rec.labels.push_back({batch->InternString("comm"),
                                  batch->InternString(ts.comm)});
            rec.labels.push_back({batch->InternString("parent_comm"),
                                  batch->InternString(parent_comm)});
            rec.SetField(batch->InternString("cpu_user_pct"), t_user_pct);
            rec.SetField(batch->InternString("cpu_sys_pct"), t_sys_pct);
            rec.SetField(batch->InternString("cpu_total_pct"), t_user_pct + t_sys_pct);
            rec.SetField(batch->InternString("state"),
                         batch->InternString(std::string(1, ts.state)));
        }
        closedir(dir);
    }

    static uint64_t MakeThreadKey(uint32_t pid, uint32_t tid) {
        return (static_cast<uint64_t>(pid) << 32) | tid;
    }

    uint32_t interval_ms_ = 2000;
    uint32_t top_n_ = 50;
    double thread_detail_threshold_pct_ = 3.0;
    std::unordered_set<uint32_t> thread_detail_pids_;
    int num_cpus_ = 1;

    bool has_include_regex_ = false;
    bool has_exclude_regex_ = false;
    std::regex include_regex_;
    std::regex exclude_regex_;

    uint64_t prev_total_jiffies_ = 0;
    std::unordered_map<uint32_t, proc::ProcessStat> prev_snapshots_;
    std::unordered_map<uint64_t, ThreadSnapshot> prev_thread_snapshots_;
};

IL_REGISTER_SOURCE("process_cpu", ProcessCpuSource);

}  // namespace illuminator
