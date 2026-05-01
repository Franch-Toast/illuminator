#pragma once

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

inline void ParseCommaSeparatedUint32sLocal(const std::string& s,
                                            std::vector<uint32_t>* out) {
    out->clear();
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        if (token.empty())
            continue;
        try {
            out->push_back(static_cast<uint32_t>(std::stoul(token)));
        } catch (...) {}
    }
}

class SchedAnalyzerSource : public SourcePlugin {
public:
    const char* Name() const override { return "sched_analyzer"; }
    const char* Version() const override { return "0.2.0"; }

    bool IsPushMode() const override { return detailed_mode_; }

    uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

    Status Init(const ConfigValue& config) override {
        detailed_mode_ = config["detailed_mode"].AsBool(false);
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(5000));
        track_migrations_ = config["track_migrations"].AsBool(true);
        bpf_obj_path_ = config["bpf_object"].AsString("");
        ParseCommaSeparatedUint32sLocal(config["target_pids"].AsString(""),
                                      &target_pids_);
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "sched_analyzer: no bpf_object path specified; analyzer idle");
            running_.store(false);
            return Status::Ok();
        }

        auto st = bpf_mgr_.LoadObject("sched_analyzer", bpf_obj_path_);
        if (!st.ok())
            return st;

        uint32_t cfg_flags =
            (detailed_mode_ ? 1u : 0u) | (track_migrations_ ? 2u : 0u);
        int cfg_fd = bpf_mgr_.GetMapFd("sched_analyzer", "sched_analyzer_cfg");
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &cfg_flags, BPF_ANY);
        }

        std::vector<std::string> progs = {"sched_analyzer_wakeup",
                                          "sched_analyzer_switch"};
        if (track_migrations_)
            progs.push_back("sched_analyzer_migrate");

        st = bpf_mgr_.AttachPrograms("sched_analyzer", progs);
        if (!st.ok())
            return st;

        agg_fd_ = bpf_mgr_.GetMapFd("sched_analyzer", "sched_agg");

        if (detailed_mode_) {
            int rb_fd =
                bpf_mgr_.GetMapFd("sched_analyzer", "sched_analyzer_events");
            if (rb_fd < 0) {
                return Status::Error(StatusCode::kInternal,
                                     "sched_analyzer_events ringbuf missing");
            }
            ring_buf_ =
                bpf_mgr_.CreateRingBuffer(rb_fd, HandleDetailedEvent, this);
            if (!ring_buf_) {
                return Status::Error(StatusCode::kInternal,
                                     "sched_analyzer ring buffer failed");
            }
            running_.store(true);
            poll_thread_ = std::thread([this] {
                while (running_.load())
                    ring_buffer__poll(ring_buf_, 100);
            });
        } else {
            running_.store(true);
        }

        IL_INFO("sched_analyzer started (detailed=%d migrations=%d)",
                detailed_mode_, track_migrations_);
        return Status::Ok();
    }

    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        agg_fd_ = -1;
        return Status::Ok();
    }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        if (detailed_mode_ || agg_fd_ < 0)
            return batch;

        uint32_t cur{}, next{};
        std::vector<uint32_t> pids;
        int err = bpf_map_get_next_key(agg_fd_, nullptr, &cur);
        while (err == 0) {
            pids.push_back(cur);
            err = bpf_map_get_next_key(agg_fd_, &cur, &next);
            cur = next;
        }

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
        }

        return batch;
    }

private:
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty())
            return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    static int HandleDetailedEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<SchedAnalyzerSource*>(ctx);
        if (size < sizeof(il_sched_event))
            return 0;
        auto* event = static_cast<il_sched_event*>(data);

        if (!self->AllowPid(event->prev_pid) &&
            !self->AllowPid(event->next_pid))
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        auto& rec = batch->AddRecord();

        const char* evt_types[] = {"switch", "wakeup", "migrate"};
        unsigned idx =
            event->event_type <
                    sizeof(evt_types) / sizeof(evt_types[0])
                ? event->event_type
                : 0;

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

        if (self->callback_)
            self->callback_(std::move(batch));
        return 0;
    }

    bool detailed_mode_ = false;
    uint32_t aggregate_interval_ms_ = 5000;
    bool track_migrations_ = true;
    std::string bpf_obj_path_;
    std::vector<uint32_t> target_pids_;
    std::unordered_set<uint32_t> target_pid_allow_;

    std::atomic<bool> running_{false};
    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    int agg_fd_ = -1;
};

IL_REGISTER_SOURCE("sched_analyzer", SchedAnalyzerSource);

}  // namespace illuminator
