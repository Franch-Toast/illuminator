// ============================================================================
// CpuProfilerSource — eBPF CPU 性能剖析器（火焰图数据源）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 cpu_profiler.legacy.h 作为功能参考。
//
// 两种工作模式：
//   - aggregated（Pull，默认）：Collect() → CollectFromMaps() 从 stack_counts 读聚合数据
//   - stream（Push）：ring buffer → ConsumeAndBatch() 批量消费
//
// perf_event 管理通过 bpf_util 工具函数完成，无独立线程。
// ============================================================================

#pragma once

#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cpu_profiler_sk.skel.h"
#include "core/common/logging.h"
#include "core/common/string_util.h"
#include "ebpf_common/include/event_types.h"
#include "ebpf_common/loader/bpf_util.h"
#include "ebpf_common/loader/stack_trace_util.h"
#include "plugin/api/ebpf_source_base.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class CpuProfilerSource : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(cpu_profiler_sk);

public:
    const char* Name() const override { return "cpu_profiler"; }
    const char* Version() const override { return "2.0.0"; }
    bool IsPushMode() const override { return stream_mode_; }

    Status Init(const ConfigValue& config) override {
        frequency_hz_ = static_cast<int>(
            config["sample_freq"].AsInt(config["frequency_hz"].AsInt(49)));
        stack_depth_ = static_cast<int>(config["stack_depth"].AsInt(128));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);
        stream_mode_ = (config["mode"].AsString("aggregated") == "stream");

        target_pids_ =
            ParseCommaSeparated<uint32_t>(config["target_pids"].AsString(""));
        ParseComms(config["target_process_names"].AsString(
            config["target_comms"].AsString("")));

        if (stack_depth_ > MAX_STACK_DEPTH) stack_depth_ = MAX_STACK_DEPTH;
        return Status::Ok();
    }

    // ================================================================
    // Hooks — 配置 BPF skeleton
    // ================================================================

    void OnConfigureRodata(void* /*s*/) override {
        if (skel()->rodata)
            skel()->rodata->sample_freq = static_cast<uint64_t>(frequency_hz_);
    }

    void OnConfigureMaps(void* /*s*/) override {
        stacks_fd_ = bpf_map__fd(skel()->maps.stacks);
        counts_fd_ = bpf_map__fd(skel()->maps.stack_counts);
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));

        if (stream_mode_)
            SetRingBufFd(bpf_map__fd(skel()->maps.cpu_events));

        ApplyFilters();
    }

    Status OnPostAttach(void* /*s*/) override {
        bpf_util::PerfConfig pcfg;
        pcfg.type = PERF_TYPE_SOFTWARE;
        pcfg.config = PERF_COUNT_SW_CPU_CLOCK;
        pcfg.sample_freq = static_cast<uint64_t>(frequency_hz_);
        pcfg.freq_mode = true;
        pcfg.exclude_user = !user_stacks_;
        pcfg.exclude_kernel = !kernel_stacks_;

        int prog_fd = bpf_program__fd(skel()->progs.on_cpu_sample);
        perf_fds_ = bpf_util::AttachPerfEvents(prog_fd, pcfg);

        if (perf_fds_.empty()) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: no perf events opened");
        }
        IL_INFO("cpu_profiler: {} perf events attached", perf_fds_.size());
        return Status::Ok();
    }

    void OnPreDestroy() override {
        bpf_util::DetachPerfEvents(perf_fds_);
    }

    // ================================================================
    // Push 模式
    // ================================================================

    ring_buffer_sample_fn GetEventCallback() const override {
        return stream_mode_ ? HandleStreamEvent : nullptr;
    }

    DataBatchPtr MakePushBatch() override {
        return std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    }

    // ================================================================
    // Pull 模式
    // ================================================================

    StatusOr<DataBatchPtr> CollectFromMaps() override {
        if (counts_fd_ < 0 || stacks_fd_ < 0)
            return Status::Error(StatusCode::kUnavailable, "BPF not loaded");

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        ReadAndFlushCounts(batch.get());
        return batch;
    }

    // ================================================================
    // QueryExtra
    // ================================================================

    StatusOr<std::string> QueryExtra(
            const std::string& query, const QueryParams&) override {
        if (query != "snapshot")
            return Status::Error(StatusCode::kUnimplemented, "unknown query");
        std::lock_guard<std::mutex> lk(snap_mu_);
        return latest_snapshot_;
    }

    // ================================================================
    // Reconfigure
    // ================================================================

    Status Reconfigure(const ConfigValue& params) override {
        target_pids_ =
            ParseCommaSeparated<uint32_t>(params["target_pids"].AsString(""));
        ParseComms(params["target_process_names"].AsString(
            params["target_comms"].AsString("")));

        bpf_util::RewritePidFilter(
            bpf_map__fd(skel()->maps.target_pids), target_pids_);
        bpf_util::RewriteCommFilter(
            bpf_map__fd(skel()->maps.target_comms), target_comms_);
        ApplyConfigFlags();

        bpf_util::DetachPerfEvents(perf_fds_);
        auto st = OnPostAttach(nullptr);

        IL_INFO("cpu_profiler: reconfigured (pids={}, comms={}, perf={})",
                target_pids_.size(), target_comms_.size(), perf_fds_.size());
        return st;
    }

    // ================================================================
    // Pause/Resume/Backpressure
    // ================================================================

    void OnPause() override {
        bpf_util::DisablePerfEvents(perf_fds_);
    }

    void OnResume() override {
        bpf_util::EnablePerfEvents(perf_fds_);
    }

    void OnBackpressure(bool active) override {
        uint64_t freq = active
            ? static_cast<uint64_t>(std::max(1, frequency_hz_ / 4))
            : static_cast<uint64_t>(frequency_hz_);
        bpf_util::AdjustPerfFrequency(perf_fds_, freq);
    }

private:
    void ParseComms(const std::string& s) {
        target_comms_.clear();
        if (s.empty()) return;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (!tok.empty()) target_comms_.push_back(tok);
        }
    }

    void ApplyFilters() {
        bpf_util::ConfigurePidNamespace(
            bpf_map__fd(skel()->maps.cpu_pidns_cfg));
        bpf_util::WritePidFilter(
            bpf_map__fd(skel()->maps.target_pids), target_pids_);
        bpf_util::WriteCommFilter(
            bpf_map__fd(skel()->maps.target_comms), target_comms_);
        ApplyConfigFlags();
    }

    void ApplyConfigFlags() {
        int fd = bpf_map__fd(skel()->maps.cpu_profiler_cfg);
        if (fd < 0) return;
        uint32_t flags = (stream_mode_ ? 1u : 0u) |
                         (!target_pids_.empty() ? 2u : 0u) |
                         (!target_comms_.empty() ? 4u : 0u);
        bpf_util::WriteMapU32(fd, 0, flags);
    }

    void ReadAndFlushCounts(DataBatch* batch) {
        il_stack_key cur{}, next{};
        std::vector<il_stack_key> keys;
        int err = bpf_map_get_next_key(counts_fd_, nullptr, &cur);
        while (err == 0) {
            keys.push_back(cur);
            err = bpf_map_get_next_key(counts_fd_, &cur, &next);
            cur = next;
        }

        for (const auto& key : keys) {
            uint64_t count = 0;
            if (bpf_map_lookup_elem(counts_fd_, &key, &count) != 0) continue;

            auto& s = batch->AddStackSample();
            s.pid = key.pid;
            s.tid = key.tid;
            s.comm = batch->InternString(std::string_view(
                key.comm, strnlen(key.comm, TASK_COMM_LEN)));
            s.count = count;
            s.sample_type = SampleType::kOnCpu;
            s.kernel_stack_id = key.kernel_stack_id;
            s.user_stack_id = key.user_stack_id;
            s.kernel_stack = LookupBpfStackTrace(
                stacks_fd_, key.kernel_stack_id,
                static_cast<size_t>(stack_depth_));
            s.user_stack = LookupBpfStackTrace(
                stacks_fd_, key.user_stack_id,
                static_cast<size_t>(stack_depth_));

            bpf_map_delete_elem(counts_fd_, &key);
        }

        if (!batch->Empty()) {
            BuildJsonSnapshot(*batch);
        }
    }

    void BuildJsonSnapshot(const DataBatch& batch) {
        nlohmann::json j;
        nlohmann::json samples = nlohmann::json::array();
        for (const auto& s : batch.stack_samples()) {
            nlohmann::json item;
            item["pid"] = s.pid;
            item["tid"] = s.tid;
            item["comm"] = std::string(s.comm);
            item["count"] = s.count;
            nlohmann::json stack_arr = nlohmann::json::array();
            for (const auto& f : s.kernel_stack) {
                if (!f.function_name.empty())
                    stack_arr.push_back(std::string(f.function_name));
                else {
                    std::ostringstream oss;
                    oss << "0x" << std::hex << f.address;
                    stack_arr.push_back(oss.str());
                }
            }
            for (const auto& f : s.user_stack) {
                if (!f.function_name.empty())
                    stack_arr.push_back(std::string(f.function_name));
                else {
                    std::ostringstream oss;
                    oss << "0x" << std::hex << f.address;
                    stack_arr.push_back(oss.str());
                }
            }
            item["stack"] = std::move(stack_arr);
            samples.push_back(std::move(item));
        }
        j["stack_samples"] = std::move(samples);
        j["pipeline"] = "cpu_profile";
        std::lock_guard<std::mutex> lk(snap_mu_);
        latest_snapshot_ = j.dump();
    }

    static int HandleStreamEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<CpuProfilerSource*>(ctx);
        if (size < sizeof(il_cpu_sample_event) || !self->pending_batch_)
            return 0;
        auto* ev = static_cast<il_cpu_sample_event*>(data);
        auto* batch = self->pending_batch_.get();

        auto& s = batch->AddStackSample();
        s.pid = ev->pid;
        s.tid = ev->tid;
        s.cpu = ev->cpu;
        s.comm = batch->InternString(std::string_view(
            ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
        s.count = 1;
        s.sample_type = SampleType::kOnCpu;
        s.kernel_stack_id = ev->kernel_stack_id;
        s.user_stack_id = ev->user_stack_id;
        s.kernel_stack = LookupBpfStackTrace(
            self->stacks_fd_, ev->kernel_stack_id,
            static_cast<size_t>(self->stack_depth_));
        s.user_stack = LookupBpfStackTrace(
            self->stacks_fd_, ev->user_stack_id,
            static_cast<size_t>(self->stack_depth_));
        return 0;
    }

    int frequency_hz_ = 49;
    int stack_depth_ = MAX_STACK_DEPTH;
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    bool stream_mode_ = false;

    std::vector<uint32_t> target_pids_;
    std::vector<std::string> target_comms_;

    int stacks_fd_ = -1;
    int counts_fd_ = -1;
    std::vector<int> perf_fds_;

    mutable std::mutex snap_mu_;
    std::string latest_snapshot_ = R"({"stack_samples":[]})";
};

IL_REGISTER_SOURCE("cpu_profiler", CpuProfilerSource);

}  // namespace illuminator
