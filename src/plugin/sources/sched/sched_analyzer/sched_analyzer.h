// ============================================================================
// SchedAnalyzerSource — eBPF 调度分析器（调度延迟与迁移追踪）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 sched_analyzer.legacy.h 作为功能参考。
//
// 两种工作模式：
//   - 聚合模式（Pull，默认）：Collect() → CollectFromMaps() 从 sched_agg 读聚合统计
//   - 详细模式（Push，detailed_mode=true）：ring buffer 推送每条调度事件
// ============================================================================

#pragma once

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "ebpf/include/bpf_compat.h"
#include <nlohmann/json.hpp>

#include "sched_analyzer_sk.skel.h"
#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_source_base.h"
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
    uint32_t event_type = 0;
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

class SchedAnalyzerSource : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(sched_analyzer_sk);

public:
    const char* Name() const override { return "sched_analyzer"; }
    const char* Version() const override { return "2.0.0"; }
    bool IsPushMode() const override { return detailed_mode_; }
    uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

    Status Init(const ConfigValue& config) override {
        detailed_mode_ = config["detailed_mode"].AsBool(false);
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(5000));
        track_migrations_ = config["track_migrations"].AsBool(true);
        target_pids_ = ParseCommaSeparated<uint32_t>(
            config["target_pids"].AsString(""));
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        return Status::Ok();
    }

    // ================================================================
    // Hooks
    // ================================================================

    void OnConfigureMaps(void* /*s*/) override {
        agg_fd_ = bpf_map__fd(skel()->maps.sched_agg);
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));

        uint32_t cfg_flags =
            (detailed_mode_ ? 1u : 0u) | (track_migrations_ ? 2u : 0u);
        int cfg_fd = bpf_map__fd(skel()->maps.sched_analyzer_cfg);
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &cfg_flags, BPF_ANY);
        }

        if (detailed_mode_)
            SetRingBufFd(bpf_map__fd(skel()->maps.sched_analyzer_events));
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return detailed_mode_ ? HandleDetailedEvent : nullptr;
    }

    DataBatchPtr MakePushBatch() override {
        return std::make_shared<DataBatch>(DataBatch::Type::kTrace);
    }

    // ================================================================
    // Pull 模式
    // ================================================================

    StatusOr<DataBatchPtr> CollectFromMaps() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        if (agg_fd_ < 0) return batch;

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
            if (bpf_map_lookup_elem(agg_fd_, &pid, &stats) != 0) continue;

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

            std::string comm(stats.comm, strnlen(stats.comm, TASK_COMM_LEN));
            if (!comm.empty()) {
                rec.labels.push_back({batch->InternString("comm"),
                                      batch->InternString(comm)});
            }

            rec.SetField(batch->InternString("pid"),
                         static_cast<uint64_t>(pid));
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
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            pt.total_switches = total_sw;
            pt.avg_latency_us = total_sw > 0
                ? static_cast<double>(total_lat) / total_sw / 1000.0
                : 0;
            pt.max_latency_ns = max_lat;
            pt.total_migrations = total_mig;
            pt.process_count = proc_count;
            history_.push_back(pt);
            while (history_.size() > kMaxHistory) history_.pop_front();
        }
        return batch;
    }

    // ================================================================
    // QueryExtra
    // ================================================================

    std::vector<SchedHistoryPoint> GetHistory(size_t max_n = 0) const {
        std::lock_guard<std::mutex> lk(history_mu_);
        if (max_n == 0 || max_n >= history_.size())
            return {history_.begin(), history_.end()};
        return {history_.end() - max_n, history_.end()};
    }

    std::vector<SchedEventRecord> GetRecentEvents(
            uint32_t filter_pid = 0, size_t max_n = 200) const {
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
        size_t start =
            wakeups_.size() > max_n ? wakeups_.size() - max_n : 0;
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
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty()) return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    static int HandleDetailedEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<SchedAnalyzerSource*>(ctx);
        if (size < sizeof(il_sched_event) || !self->pending_batch_) return 0;
        auto* ev = static_cast<il_sched_event*>(data);

        if (!self->AllowPid(ev->prev_pid) && !self->AllowPid(ev->next_pid))
            return 0;

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        {
            std::lock_guard<std::mutex> lk(self->events_mu_);
            SchedEventRecord er;
            er.timestamp_ms = now_ms;
            er.event_type = ev->event_type;
            er.prev_pid = ev->prev_pid;
            er.next_pid = ev->next_pid;
            er.cpu = ev->cpu;
            er.latency_ns = ev->latency_ns;
            std::memcpy(er.prev_comm, ev->prev_comm,
                        strnlen(ev->prev_comm, TASK_COMM_LEN));
            std::memcpy(er.next_comm, ev->next_comm,
                        strnlen(ev->next_comm, TASK_COMM_LEN));
            self->recent_events_.push_back(er);
            while (self->recent_events_.size() > kMaxEvents)
                self->recent_events_.pop_front();
        }

        if (ev->event_type == 1) {
            std::lock_guard<std::mutex> lk(self->wakeups_mu_);
            WakeupRecord wr;
            wr.timestamp_ms = now_ms;
            wr.waker_pid = ev->prev_pid;
            wr.wakee_pid = ev->next_pid;
            std::memcpy(wr.waker_comm, ev->prev_comm,
                        strnlen(ev->prev_comm, TASK_COMM_LEN));
            std::memcpy(wr.wakee_comm, ev->next_comm,
                        strnlen(ev->next_comm, TASK_COMM_LEN));
            self->wakeups_.push_back(wr);
            while (self->wakeups_.size() > kMaxWakeups)
                self->wakeups_.pop_front();
        }

        auto* batch = self->pending_batch_.get();
        auto& rec = batch->AddRecord();

        const char* evt_types[] = {"switch", "wakeup", "migrate"};
        unsigned idx = ev->event_type < 3 ? ev->event_type : 0;

        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("sched")});
        rec.labels.push_back({batch->InternString("event"),
                              batch->InternString(evt_types[idx])});

        rec.SetField(batch->InternString("prev_pid"),
                     static_cast<uint64_t>(ev->prev_pid));
        rec.SetField(batch->InternString("next_pid"),
                     static_cast<uint64_t>(ev->next_pid));
        rec.SetField(batch->InternString("cpu"),
                     static_cast<uint64_t>(ev->cpu));
        rec.SetField(batch->InternString("latency_ns"),
                     static_cast<uint64_t>(ev->latency_ns));
        rec.SetField(batch->InternString("prev_comm"),
                     batch->InternString(std::string_view(
                         ev->prev_comm,
                         strnlen(ev->prev_comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("next_comm"),
                     batch->InternString(std::string_view(
                         ev->next_comm,
                         strnlen(ev->next_comm, TASK_COMM_LEN))));
        return 0;
    }

    bool detailed_mode_ = false;
    uint32_t aggregate_interval_ms_ = 5000;
    bool track_migrations_ = true;
    std::vector<uint32_t> target_pids_;
    std::unordered_set<uint32_t> target_pid_allow_;

    int agg_fd_ = -1;

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
