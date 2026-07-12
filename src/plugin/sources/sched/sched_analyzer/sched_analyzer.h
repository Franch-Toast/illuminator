// ============================================================================
// SchedAnalyzerSource — eBPF 调度分析器（调度延迟与迁移追踪）
// ============================================================================
//
// 通过 eBPF 挂载到内核调度器关键事件点（进程切换、唤醒、迁移），
// 实时采集进程调度延迟、运行队列等待时间、CPU 核心间迁移频次等指标。
// 用于诊断 CPU 调度瓶颈、NUMA 亲和性问题、调度延迟过高等场景。
//
// 采集指标：
// ==========
// 在聚合模式下，从 sched_agg map 中每次 Collect 取出各进程的累计统计：
//   - switch_count：上下文切换次数（被调度上/下 CPU 的次数）
//   - total_runqueue_latency_ns：运行队列中累计等待时间（纳秒）
//   - max_runqueue_latency_ns：运行队列最大单次等待时间（纳秒）
//   - migrate_count：CPU 核心迁移次数
//
// 在详细模式（detailed_mode）下，通过 ring buffer 实时推送每条调度事件：
//   - 事件类型：switch（上下文切换）、wakeup（进程唤醒）、migrate（核心迁移）
//   - prev_pid / next_pid：切换前后进程 PID
//   - prev_comm / next_comm：切换前后进程名
//   - cpu：事件发生的 CPU 核心
//   - latency_ns：延迟时间（纳秒）
//
// 工作原理：
// ==========
// 1. 通过 eBPF 挂载到以下内核 tracepoint：
//    - sched/sched_switch：进程切换事件（捕获 prev → next 的任务切换，计算运行队列延迟）
//    - sched/sched_wakeup / sched/sched_wakeup_new：进程唤醒事件（记录被唤醒进程的等待时间）
//    - sched/sched_migrate_task（可选）：进程迁移事件（记录进程从哪个核迁移到哪个核）
//
// 2. 两种工作模式：
//    - 聚合模式（默认，detailed_mode=false）：
//      每个 eBPF 事件触发时，更新 sched_agg map 中的 per-PID 聚合统计。
//      Collect() 方法遍历 map 中所有条目，取出统计值后删除（reset）。
//
//    - 详细模式（detailed_mode=true）：
//      通过 sched_analyzer_events ring buffer 推送每条原始调度事件。
//      按事件类型分类（switch/wakeup/migrate），适合做时序追踪或分布分析。
//
// 配置参数：
// ==========
// - detailed_mode：是否启用详细事件推送（默认 false）
// - aggregate_interval_ms：聚合统计输出间隔（默认 5000ms）
// - track_migrations：是否追踪 CPU 核心迁移事件（默认 true）
// - target_pids：逗号分隔的 PID 白名单（空 = 追踪所有进程）
// - bpf_object：eBPF 目标文件路径（必需）
// ============================================================================

#pragma once

#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "ebpf/include/bpf_compat.h"
#include <nlohmann/json.hpp>

#include "sched_analyzer_sk.skel.h"
#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "core/threading/thread_util.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_stats_reader.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

struct SchedHistoryPoint {
    uint64_t timestamp_ms = 0;
    uint64_t total_switches = 0;
    double avg_latency_us = 0;
    uint64_t max_latency_ns = 0;
    uint64_t total_migrations = 0;
    uint32_t process_count = 0;
};

struct SchedEventRecord {
    uint64_t timestamp_ms = 0;
    uint32_t event_type = 0;   // 0=switch, 1=wakeup, 2=migrate
    uint32_t prev_pid = 0;
    uint32_t next_pid = 0;
    uint32_t cpu = 0;
    uint64_t latency_ns = 0;
    char prev_comm[TASK_COMM_LEN] = {};
    char next_comm[TASK_COMM_LEN] = {};
};

struct WakeupRecord {
    uint64_t timestamp_ms = 0;
    uint32_t waker_pid = 0;
    uint32_t wakee_pid = 0;
    char waker_comm[TASK_COMM_LEN] = {};
    char wakee_comm[TASK_COMM_LEN] = {};
};

class SchedAnalyzerSource : public SourcePlugin {
public:
    const char* Name() const override { return "sched_analyzer"; }
    const char* Version() const override { return "0.2.0"; }

    bool IsPushMode() const override { return detailed_mode_; }
    bool HasBpfProbe() const override { return !stub_mode_; }
    bool IsStub() const override { return stub_mode_; }

    MetaStats GetBpfStats() const override {
        return ReadBpfMetaStats(meta_stats_fd_);
    }

    uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

    // ========================================================================
    // Init — 初始化插件配置
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        detailed_mode_ = config["detailed_mode"].AsBool(false);
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(5000));
        track_migrations_ = config["track_migrations"].AsBool(true);
        target_pids_ = ParseCommaSeparated<uint32_t>(
            config["target_pids"].AsString(""));
        // 构建 PID 快速查找集合
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        return Status::Ok();
    }

    // ========================================================================
    // Start — 加载 eBPF 程序并挂载调度 tracepoint
    // ========================================================================
    // 1. 加载 eBPF 目标文件
    // 2. 设置 sched_analyzer_cfg 配置 map（模式标志、迁移追踪标志）
    // 3. 挂载 eBPF 程序到 sched/sched_wakeup、sched/sched_switch、
    //    （可选）sched/sched_migrate_task 等内核 tracepoint
    // 4. 详细模式下创建 ring buffer 和后台轮询线程
    Status Start() override {
        // Skeleton 模式：从嵌入的字节码加载
        sched_skel_ = sched_analyzer_sk_bpf__open();
        if (!sched_skel_) {
            IL_WARN("sched_analyzer: skeleton open failed; idle mode");
            stub_mode_ = true;
            running_.store(false);
            return Status::Ok();
        }

        int err = sched_analyzer_sk_bpf__load(sched_skel_);
        if (err) {
            sched_analyzer_sk_bpf__destroy(sched_skel_);
            sched_skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                "sched_analyzer: skeleton load failed (err=" +
                std::to_string(err) + ")");
        }

        // 写入配置标志位到 eBPF 侧（位 0: detailed, 位 1: migrations）
        uint32_t cfg_flags =
            (detailed_mode_ ? 1u : 0u) | (track_migrations_ ? 2u : 0u);
        int cfg_fd = bpf_map__fd(sched_skel_->maps.sched_analyzer_cfg);
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &cfg_flags, BPF_ANY);
        }

        // Skeleton 自动 attach 所有程序
        int err2 = sched_analyzer_sk_bpf__attach(sched_skel_);
        if (err2) {
            sched_analyzer_sk_bpf__destroy(sched_skel_);
            sched_skel_ = nullptr;
            return Status::Error(StatusCode::kInternal,
                "sched_analyzer: attach failed (err=" + std::to_string(err2) + ")");
        }

        agg_fd_ = bpf_map__fd(sched_skel_->maps.sched_agg);
        meta_stats_fd_ = bpf_map__fd(sched_skel_->maps.meta_stats);

        if (detailed_mode_) {
            int rb_fd = bpf_map__fd(sched_skel_->maps.sched_analyzer_events);
            ring_buf_ = ring_buffer__new(rb_fd, HandleDetailedEvent, this, nullptr);
            if (!ring_buf_) {
                return Status::Error(StatusCode::kInternal,
                                     "sched_analyzer ring buffer failed");
            }
            running_.store(true);
            poll_thread_ = std::thread([this] {
                SetThreadName("sched-poll");
                while (running_.load())
                    ring_buffer__poll(ring_buf_, 100);
            });
        } else {
            running_.store(true);
        }

        IL_INFO("sched_analyzer started (detailed={} migrations={})",
                detailed_mode_, track_migrations_);
        return Status::Ok();
    }

    // ========================================================================
    // PauseCollection / ResumeCollection — Push 模式暂停/恢复
    // ========================================================================
    // detailed_mode_ 时为 Push 模式，通过清除 cfg bit0 关闭 BPF 事件发射
    Status PauseCollection() override {
        if (stub_mode_ || !detailed_mode_) return Status::Ok();
        int cfg_fd = bpf_map__fd(sched_skel_->maps.sched_analyzer_cfg);
        if (cfg_fd >= 0) {
            uint32_t k = 0, val = 0;
            bpf_map_update_elem(cfg_fd, &k, &val, BPF_ANY);
        }
        paused_ = true;
        if (poll_thread_.joinable()) poll_thread_.join();
        IL_INFO("sched_analyzer: collection paused");
        return Status::Ok();
    }

    Status ResumeCollection() override {
        if (stub_mode_ || !detailed_mode_) return Status::Ok();
        paused_ = false;
        poll_thread_ = std::thread([this] {
            SetThreadName("sched-poll");
            while (running_.load() && !paused_)
                ring_buffer__poll(ring_buf_, 100);
        });
        int cfg_fd = bpf_map__fd(sched_skel_->maps.sched_analyzer_cfg);
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            uint32_t cfg_flags =
                (detailed_mode_ ? 1u : 0u) | (track_migrations_ ? 2u : 0u);
            bpf_map_update_elem(cfg_fd, &k, &cfg_flags, BPF_ANY);
        }
        IL_INFO("sched_analyzer: collection resumed");
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止调度分析，清理资源
    // ========================================================================
    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        // skeleton destroy handles detach
        agg_fd_ = -1;

        if (sched_skel_) {
            sched_analyzer_sk_bpf__destroy(sched_skel_);
            sched_skel_ = nullptr;
        }

        return Status::Ok();
    }

    // ========================================================================
    // Collect — 聚合模式下的轮询采集入口
    // ========================================================================
    // 遍历 sched_agg map 中的所有 PID 条目，读取其调度统计后删除。
    // 对于不在目标 PID 白名单中的条目也执行删除（清理不需要的数据）。
    // 详细模式下 Collect 返回空 batch（数据已通过 ring buffer 推送）。
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        if (agg_fd_ < 0)
            return batch;

        uint32_t cur{}, next{};
        std::vector<uint32_t> pids;
        int err = bpf_map_get_next_key(agg_fd_, nullptr, &cur);
        while (err == 0) {
            pids.push_back(cur);
            err = bpf_map_get_next_key(agg_fd_, &cur, &next);
            cur = next;
        }

        uint64_t total_sw = 0, total_lat = 0, max_lat = 0, total_mig = 0;
        uint32_t proc_count = 0;

        for (uint32_t pid : pids) {
            il_sched_stats stats{};
            if (bpf_map_lookup_elem(agg_fd_, &pid, &stats) != 0)
                continue;

            if (!AllowPid(pid)) {
                bpf_map_delete_elem(agg_fd_, &pid);
                continue;
            }

            bpf_map_delete_elem(agg_fd_, &pid);

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("sched")});
            rec.labels.push_back({batch->InternString("mode"),
                                  batch->InternString("aggregated")});

            std::string comm(stats.comm,
                              strnlen(stats.comm, TASK_COMM_LEN));
            if (!comm.empty()) {
                rec.labels.push_back({batch->InternString("comm"),
                                      batch->InternString(comm)});
            }

            rec.SetField(batch->InternString("pid"), static_cast<uint64_t>(pid));
            rec.SetField(batch->InternString("switch_count"),
                         stats.switch_count);
            rec.SetField(batch->InternString("total_runqueue_latency_ns"),
                         stats.total_runqueue_latency_ns);
            rec.SetField(batch->InternString("max_runqueue_latency_ns"),
                         stats.max_runqueue_latency_ns);
            rec.SetField(batch->InternString("migrate_count"),
                         stats.migrate_count);

            total_sw += stats.switch_count;
            total_lat += stats.total_runqueue_latency_ns;
            if (stats.max_runqueue_latency_ns > max_lat)
                max_lat = stats.max_runqueue_latency_ns;
            total_mig += stats.migrate_count;
            proc_count++;
        }

        {
            std::lock_guard<std::mutex> lk(history_mu_);
            SchedHistoryPoint pt;
            pt.timestamp_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            pt.total_switches = total_sw;
            pt.avg_latency_us = total_sw > 0
                ? static_cast<double>(total_lat) / total_sw / 1000.0
                : 0;
            pt.max_latency_ns = max_lat;
            pt.total_migrations = total_mig;
            pt.process_count = proc_count;
            history_.push_back(pt);
            while (history_.size() > kMaxHistory)
                history_.pop_front();
        }

        return batch;
    }

    std::vector<SchedHistoryPoint> GetHistory(size_t max_n = 0) const {
        std::lock_guard<std::mutex> lk(history_mu_);
        if (max_n == 0 || max_n >= history_.size())
            return {history_.begin(), history_.end()};
        return {history_.end() - max_n, history_.end()};
    }

    std::vector<SchedEventRecord> GetRecentEvents(uint32_t filter_pid = 0,
                                                    size_t max_n = 200) const {
        std::lock_guard<std::mutex> lk(events_mu_);
        std::vector<SchedEventRecord> out;
        for (auto it = recent_events_.rbegin();
             it != recent_events_.rend() && out.size() < max_n; ++it) {
            if (filter_pid == 0 || it->prev_pid == filter_pid ||
                it->next_pid == filter_pid)
                out.push_back(*it);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::vector<WakeupRecord> GetRecentWakeups(size_t max_n = 200) const {
        std::lock_guard<std::mutex> lk(wakeups_mu_);
        size_t start = wakeups_.size() > max_n ? wakeups_.size() - max_n : 0;
        return {wakeups_.begin() + start, wakeups_.end()};
    }

    StatusOr<std::string> QueryExtra(
        const std::string& query, const QueryParams& params) override {
        using json = nlohmann::json;

        if (query == "history") {
            auto pts = GetHistory();
            json arr = json::array();
            for (auto& p : pts) {
                arr.push_back({
                    {"timestamp_ms", p.timestamp_ms},
                    {"total_switches", p.total_switches},
                    {"avg_latency_us", p.avg_latency_us},
                    {"max_latency_ns", p.max_latency_ns},
                    {"total_migrations", p.total_migrations},
                    {"process_count", p.process_count},
                });
            }
            return json{{"history", std::move(arr)}}.dump();
        }

        if (query == "events") {
            uint32_t pid = 0;
            size_t limit = 200;
            auto pit = params.find("pid");
            if (pit != params.end()) pid = std::atoi(pit->second.c_str());
            auto lit = params.find("limit");
            if (lit != params.end()) limit = std::atoi(lit->second.c_str());

            auto events = GetRecentEvents(pid, limit);
            const char* types[] = {"switch", "wakeup", "migrate"};
            json arr = json::array();
            for (auto& e : events) {
                unsigned ti = e.event_type < 3 ? e.event_type : 0;
                arr.push_back({
                    {"timestamp_ms", e.timestamp_ms},
                    {"event_type", types[ti]},
                    {"prev_pid", e.prev_pid},
                    {"next_pid", e.next_pid},
                    {"cpu", e.cpu},
                    {"latency_ns", e.latency_ns},
                    {"prev_comm", std::string(e.prev_comm,
                        strnlen(e.prev_comm, TASK_COMM_LEN))},
                    {"next_comm", std::string(e.next_comm,
                        strnlen(e.next_comm, TASK_COMM_LEN))},
                });
            }
            return json{{"events", std::move(arr)}}.dump();
        }

        if (query == "wakeups") {
            size_t limit = 500;
            auto lit = params.find("limit");
            if (lit != params.end()) limit = std::atoi(lit->second.c_str());

            auto wakeups = GetRecentWakeups(limit);
            json arr = json::array();
            for (auto& w : wakeups) {
                arr.push_back({
                    {"timestamp_ms", w.timestamp_ms},
                    {"waker_pid", w.waker_pid},
                    {"wakee_pid", w.wakee_pid},
                    {"waker_comm", std::string(w.waker_comm,
                        strnlen(w.waker_comm, TASK_COMM_LEN))},
                    {"wakee_comm", std::string(w.wakee_comm,
                        strnlen(w.wakee_comm, TASK_COMM_LEN))},
                });
            }
            return json{{"wakeups", std::move(arr)}}.dump();
        }

        return Status::Error(StatusCode::kNotFound, "unknown query: " + query);
    }

private:
    // ========================================================================
    // AllowPid — 检查 PID 是否在白名单中
    // ========================================================================
    // 如果白名单为空，允许所有 PID；否则只允许在白名单中的 PID。
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty())
            return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    // ========================================================================
    // HandleDetailedEvent — 详细模式的事件回调
    // ========================================================================
    // 当 ring buffer 收到一条 sched_event 时被调用。
    // 根据事件类型（switch/wakeup/migrate）分类输出，包含参与双方的
    // PID/进程名、CPU 核心、延迟时间等完整信息。
    static int HandleDetailedEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<SchedAnalyzerSource*>(ctx);
        if (size < sizeof(il_sched_event))
            return 0;
        auto* event = static_cast<il_sched_event*>(data);

        if (!self->AllowPid(event->prev_pid) &&
            !self->AllowPid(event->next_pid))
            return 0;

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        {
            std::lock_guard<std::mutex> lk(self->events_mu_);
            SchedEventRecord er;
            er.timestamp_ms = now_ms;
            er.event_type = event->event_type;
            er.prev_pid = event->prev_pid;
            er.next_pid = event->next_pid;
            er.cpu = event->cpu;
            er.latency_ns = event->latency_ns;
            std::memcpy(er.prev_comm, event->prev_comm,
                        strnlen(event->prev_comm, TASK_COMM_LEN));
            std::memcpy(er.next_comm, event->next_comm,
                        strnlen(event->next_comm, TASK_COMM_LEN));
            self->recent_events_.push_back(er);
            while (self->recent_events_.size() > kMaxEvents)
                self->recent_events_.pop_front();
        }

        if (event->event_type == 1) {
            std::lock_guard<std::mutex> lk(self->wakeups_mu_);
            WakeupRecord wr;
            wr.timestamp_ms = now_ms;
            wr.waker_pid = event->prev_pid;
            wr.wakee_pid = event->next_pid;
            std::memcpy(wr.waker_comm, event->prev_comm,
                        strnlen(event->prev_comm, TASK_COMM_LEN));
            std::memcpy(wr.wakee_comm, event->next_comm,
                        strnlen(event->next_comm, TASK_COMM_LEN));
            self->wakeups_.push_back(wr);
            while (self->wakeups_.size() > kMaxWakeups)
                self->wakeups_.pop_front();
        }

        if (self->callback_) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
            auto& rec = batch->AddRecord();

            const char* evt_types[] = {"switch", "wakeup", "migrate"};
            unsigned idx = event->event_type < 3 ? event->event_type : 0;

            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("sched")});
            rec.labels.push_back({batch->InternString("event"),
                                  batch->InternString(evt_types[idx])});

            rec.SetField(batch->InternString("prev_pid"),
                         static_cast<uint64_t>(event->prev_pid));
            rec.SetField(batch->InternString("next_pid"),
                         static_cast<uint64_t>(event->next_pid));
            rec.SetField(batch->InternString("cpu"),
                         static_cast<uint64_t>(event->cpu));
            rec.SetField(batch->InternString("latency_ns"),
                         static_cast<uint64_t>(event->latency_ns));
            rec.SetField(batch->InternString("prev_comm"),
                         batch->InternString(std::string_view(
                             event->prev_comm,
                             strnlen(event->prev_comm, TASK_COMM_LEN))));
            rec.SetField(batch->InternString("next_comm"),
                         batch->InternString(std::string_view(
                             event->next_comm,
                             strnlen(event->next_comm, TASK_COMM_LEN))));

            self->callback_(std::move(batch));
        }
        return 0;
    }

    bool stub_mode_ = false;
    bool detailed_mode_ = false;
    uint32_t aggregate_interval_ms_ = 5000;   // 聚合统计输出间隔（毫秒）
    bool track_migrations_ = true;            // 是否追踪 CPU 迁移事件
    std::vector<uint32_t> target_pids_;       // 目标 PID 列表
    std::unordered_set<uint32_t> target_pid_allow_;  // PID 快速查找集合

    // ---- 运行时状态 ----
    std::atomic<bool> running_{false};
    bool paused_ = false;
    struct sched_analyzer_sk_bpf* sched_skel_ = nullptr;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    int agg_fd_ = -1;
    int meta_stats_fd_ = -1;

    // ---- 历史和缓存 ----
    static constexpr size_t kMaxHistory = 360;
    static constexpr size_t kMaxEvents = 5000;
    static constexpr size_t kMaxWakeups = 2000;

    mutable std::mutex history_mu_;
    std::deque<SchedHistoryPoint> history_;

    mutable std::mutex events_mu_;
    std::deque<SchedEventRecord> recent_events_;

    mutable std::mutex wakeups_mu_;
    std::deque<WakeupRecord> wakeups_;
};

IL_REGISTER_SOURCE("sched_analyzer", SchedAnalyzerSource);

}  // namespace illuminator
