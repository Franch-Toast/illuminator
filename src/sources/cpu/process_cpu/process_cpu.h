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
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class ProcessCpuSource : public SourcePlugin {
public:
    const char* Name() const override { return "process_cpu"; }
    const char* Version() const override { return "1.0.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = static_cast<uint32_t>(config["interval_ms"].AsInt(2000));
        top_n_ = static_cast<uint32_t>(config["top_n"].AsInt(50));
        thread_detail_threshold_pct_ = config["thread_detail_threshold_pct"].AsDouble(3.0);

        // 正则过滤配置
        auto include_re = config["include_comm_regex"].AsString("");
        auto exclude_re = config["exclude_comm_regex"].AsString("");
        if (!include_re.empty() && include_re != ".*") {
            try {
                include_regex_ = std::regex(include_re);
                has_include_regex_ = true;
            } catch (...) {}  // 正则编译失败则忽略
        }
        if (!exclude_re.empty()) {
            try {
                exclude_regex_ = std::regex(exclude_re);
                has_exclude_regex_ = true;
            } catch (...) {}
        }

        // 线程详情白名单 PID 列表
        auto pids_str = config["thread_detail_pids"].AsString("");
        if (!pids_str.empty()) {
            std::istringstream iss(pids_str);
            std::string tok;
            while (std::getline(iss, tok, ',')) {
                try { thread_detail_pids_.insert(static_cast<uint32_t>(std::stoul(tok))); }
                catch (...) {}
            }
        }

        ReadTotalCpuJiffies(prev_total_jiffies_);  // 首次采集基线
        num_cpus_ = CountOnlineCpus();
        return Status::Ok();
    }

    uint32_t IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

        // 读取当前和上一轮的 CPU jiffy 总量
        uint64_t cur_total_jiffies = 0;
        ReadTotalCpuJiffies(cur_total_jiffies);
        uint64_t delta_total = cur_total_jiffies - prev_total_jiffies_;
        if (delta_total == 0) delta_total = 1;

        // 扫描所有进程
        std::vector<ProcessSnapshot> processes;
        ScanProcesses(processes);

        // 计算排名
        struct RankedProcess {
            ProcessSnapshot snap;
            double cpu_user_pct = 0;
            double cpu_sys_pct = 0;
            double cpu_total_pct = 0;
            bool has_prev = false;
        };

        std::vector<RankedProcess> ranked;
        ranked.reserve(processes.size());

        for (auto& proc : processes) {
            // 正则过滤
            if (has_include_regex_ && !std::regex_search(proc.comm, include_regex_))
                continue;
            if (has_exclude_regex_ && std::regex_search(proc.comm, exclude_regex_))
                continue;

            RankedProcess rp;
            rp.snap = proc;

            // CPU% = (进程 cpu_time 差值) / (总 cpu_time 差值) * 核数 * 100
            // 乘以核数是为了让多核系统上结果总和可超过 100%（正常）
            auto it = prev_snapshots_.find(proc.pid);
            if (it != prev_snapshots_.end()) {
                auto& prev = it->second;
                uint64_t du = proc.utime > prev.utime ? proc.utime - prev.utime : 0;
                uint64_t ds = proc.stime > prev.stime ? proc.stime - prev.stime : 0;
                rp.cpu_user_pct = 100.0 * static_cast<double>(du) /
                                  static_cast<double>(delta_total) * num_cpus_;
                rp.cpu_sys_pct = 100.0 * static_cast<double>(ds) /
                                 static_cast<double>(delta_total) * num_cpus_;
                rp.cpu_total_pct = rp.cpu_user_pct + rp.cpu_sys_pct;
                rp.has_prev = true;
            }
            ranked.push_back(rp);
        }

        // 排序取 Top N
        std::sort(ranked.begin(), ranked.end(),
                  [](const RankedProcess& a, const RankedProcess& b) {
                      return a.cpu_total_pct > b.cpu_total_pct;
                  });
        if (ranked.size() > top_n_)
            ranked.resize(top_n_);

        // 输出各进程指标
        for (auto& rp : ranked) {
            if (!rp.has_prev) continue;
            auto& proc = rp.snap;

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("source"),
                                  batch->InternString("process_cpu")});
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("process")});
            rec.labels.push_back({batch->InternString("pid"),
                                  batch->InternString(std::to_string(proc.pid))});
            rec.labels.push_back({batch->InternString("comm"),
                                  batch->InternString(proc.comm)});

            // 指标字段
            rec.SetField(batch->InternString("cpu_user_pct"), rp.cpu_user_pct);
            rec.SetField(batch->InternString("cpu_sys_pct"), rp.cpu_sys_pct);
            rec.SetField(batch->InternString("cpu_total_pct"), rp.cpu_total_pct);
            rec.SetField(batch->InternString("state"),
                         batch->InternString(std::string(1, proc.state)));
            rec.SetField(batch->InternString("num_threads"),
                         static_cast<uint64_t>(proc.num_threads));
            rec.SetField(batch->InternString("rss_kb"),
                         static_cast<uint64_t>(proc.rss_pages * 4));  // 页数 × 4KB
            rec.SetField(batch->InternString("vsize_kb"), proc.vsize / 1024);
            rec.SetField(batch->InternString("voluntary_ctxt_switches"),
                         proc.voluntary_ctxt_switches);
            rec.SetField(batch->InternString("nonvoluntary_ctxt_switches"),
                         proc.nonvoluntary_ctxt_switches);

            // 线程详情展开（满足条件的进程）
            bool expand_threads =
                thread_detail_pids_.count(proc.pid) > 0 ||
                rp.cpu_total_pct >= thread_detail_threshold_pct_;
            if (expand_threads) {
                CollectThreads(batch, proc.pid, proc.comm, delta_total);
            }
        }

        // 更新历史快照
        prev_snapshots_.clear();
        for (auto& proc : processes) {
            prev_snapshots_[proc.pid] = proc;
        }
        prev_total_jiffies_ = cur_total_jiffies;

        return batch;
    }

private:
    struct ProcessSnapshot {
        uint32_t pid = 0;
        std::string comm;                     // 进程名
        char state = '?';                     // 进程状态 R/S/D/Z/T...
        uint64_t utime = 0;                   // 用户态 CPU 时间 (jiffies)
        uint64_t stime = 0;                   // 内核态 CPU 时间 (jiffies)
        uint64_t starttime = 0;               // 进程启动时间
        uint32_t num_threads = 0;
        uint64_t vsize = 0;                   // 虚拟内存大小
        int64_t rss_pages = 0;                // 常驻内存页数
        uint64_t voluntary_ctxt_switches = 0;
        uint64_t nonvoluntary_ctxt_switches = 0;
    };

    struct ThreadSnapshot {
        uint32_t tid = 0;
        std::string comm;
        uint64_t utime = 0;
        uint64_t stime = 0;
        char state = '?';
    };

    // 读取系统总 CPU jiffies（/proc/stat 第一行 "cpu  $fields" 累加）
    static void ReadTotalCpuJiffies(uint64_t& total) {
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return;
        std::string line;
        if (!std::getline(file, line)) return;
        if (line.compare(0, 4, "cpu ") != 0) return;
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;  // 跳过 "cpu"
        total = 0;
        uint64_t v;
        while (iss >> v) total += v;  // 累加所有字段
    }

    // 统计在线 CPU 核数
    static int CountOnlineCpus() {
        int count = 0;
        std::ifstream file("/proc/stat");
        if (!file.is_open()) return 1;
        std::string line;
        while (std::getline(file, line)) {
            if (line.compare(0, 3, "cpu") == 0 && line.size() > 3 &&
                line[3] >= '0' && line[3] <= '9') {
                ++count;
            }
        }
        return count > 0 ? count : 1;
    }

    // 解析 /proc/[pid]/stat 获取进程快照
    static bool ParseProcStat(const std::string& path, ProcessSnapshot& out) {
        std::ifstream file(path);
        if (!file.is_open()) return false;
        // 读取整个文件内容（进程名可能含空格，需要特殊处理）
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // 进程名在 () 中，找最后一对括号
        auto open = content.find('(');
        auto close = content.rfind(')');
        if (open == std::string::npos || close == std::string::npos) return false;

        out.comm = content.substr(open + 1, close - open - 1);

        // 解析 ) 之后的字段
        std::istringstream iss(content.substr(close + 2));
        std::string state_str;
        int64_t ppid, pgrp, session, tty_nr, tpgid;
        uint64_t flags, minflt, cminflt, majflt, cmajflt;
        uint64_t cutime, cstime;
        int64_t priority, nice_val;
        int64_t itrealvalue;

        // 按 /proc/[pid]/stat 的字段顺序解析
        iss >> state_str >> ppid >> pgrp >> session >> tty_nr >> tpgid
            >> flags >> minflt >> cminflt >> majflt >> cmajflt
            >> out.utime >> out.stime >> cutime >> cstime
            >> priority >> nice_val >> out.num_threads >> itrealvalue
            >> out.starttime >> out.vsize >> out.rss_pages;

        if (!state_str.empty()) out.state = state_str[0];
        return true;
    }

    // 从 /proc/[pid]/status 读取上下文切换计数
    static void ReadCtxtSwitches(uint32_t pid, ProcessSnapshot& out) {
        std::ifstream file("/proc/" + std::to_string(pid) + "/status");
        if (!file.is_open()) return;
        std::string line;
        while (std::getline(file, line)) {
            if (line.compare(0, 25, "voluntary_ctxt_switches:\t") == 0) {
                std::istringstream iss(line.substr(25));
                iss >> out.voluntary_ctxt_switches;
            } else if (line.compare(0, 28, "nonvoluntary_ctxt_switches:\t") == 0) {
                std::istringstream iss(line.substr(28));
                iss >> out.nonvoluntary_ctxt_switches;
            }
        }
    }

    // 扫描 /proc 中的数字目录，解析每个进程
    void ScanProcesses(std::vector<ProcessSnapshot>& out) {
        DIR* dir = opendir("/proc");
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;  // 只关心目录

            // 目录名必须是纯数字（PID）
            char* endp;
            long pid = strtol(entry->d_name, &endp, 10);
            if (*endp != '\0' || pid <= 0) continue;

            ProcessSnapshot snap;
            snap.pid = static_cast<uint32_t>(pid);
            std::string stat_path = "/proc/" + std::string(entry->d_name) + "/stat";
            if (!ParseProcStat(stat_path, snap)) continue;

            ReadCtxtSwitches(snap.pid, snap);
            out.push_back(std::move(snap));
        }
        closedir(dir);
    }

    // 收集高 CPU 进程的线程级详细指标
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

            std::string stat_path = task_dir + "/" + entry->d_name + "/stat";
            ProcessSnapshot ts;
            ts.pid = static_cast<uint32_t>(tid);
            if (!ParseProcStat(stat_path, ts)) continue;

            // 计算线程 CPU%
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

            // 保存线程快照用于下次对比
            prev_thread_snapshots_[key] = {
                static_cast<uint32_t>(tid), ts.comm, ts.utime, ts.stime, ts.state
            };

            if (!had_prev) continue;  // 首次看到的线程跳过

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

    // 组合 PID 和 TID 为唯一键（高 32 位 PID，低 32 位 TID）
    static uint64_t MakeThreadKey(uint32_t pid, uint32_t tid) {
        return (static_cast<uint64_t>(pid) << 32) | tid;
    }

    // 配置参数
    uint32_t interval_ms_ = 2000;
    uint32_t top_n_ = 50;
    double thread_detail_threshold_pct_ = 3.0;
    std::unordered_set<uint32_t> thread_detail_pids_;
    int num_cpus_ = 1;

    // 正则过滤
    bool has_include_regex_ = false;
    bool has_exclude_regex_ = false;
    std::regex include_regex_;
    std::regex exclude_regex_;

    // 历史快照（与当前周期对比计算增量）
    uint64_t prev_total_jiffies_ = 0;
    std::unordered_map<uint32_t, ProcessSnapshot> prev_snapshots_;
    std::unordered_map<uint64_t, ThreadSnapshot> prev_thread_snapshots_;
};

IL_REGISTER_SOURCE("process_cpu", ProcessCpuSource);

}  // namespace illuminator
