#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <unistd.h>

#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

inline std::vector<int> ParseOnlineCpuIds() {
    std::vector<int> cpus;
    std::ifstream f("/sys/devices/system/cpu/online");
    if (!f)
        return cpus;
    std::string line;
    std::getline(f, line);
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, ',')) {
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(part));
        } else {
            int lo = std::stoi(part.substr(0, dash));
            int hi = std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi; ++c)
                cpus.push_back(c);
        }
    }
    return cpus;
}

inline void ParseCommaSeparatedInts(const std::string& s,
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

inline void ParseCommaSeparatedStrings(const std::string& s,
                                       std::vector<std::string>* out) {
    out->clear();
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        if (!token.empty())
            out->push_back(token);
    }
}

inline long PerfEventOpenSys(struct perf_event_attr* attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
#if defined(SYS_perf_event_open)
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
#elif defined(__NR_perf_event_open)
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
#else
    (void)attr;
    (void)pid;
    (void)cpu;
    (void)group_fd;
    (void)flags;
    return -1;
#endif
}

class CpuProfilerSource : public SourcePlugin {
public:
    const char* Name() const override { return "cpu_profiler"; }
    const char* Version() const override { return "0.2.0"; }

    bool IsPushMode() const override { return stream_mode_; }

    StatusOr<DataBatchPtr> Collect() override {
        std::lock_guard<std::mutex> lock(last_batch_mu_);
        if (last_batch_ && !last_batch_->Empty())
            return last_batch_;
        if (counts_fd_ < 0 || stacks_fd_ < 0)
            return Status::Error(StatusCode::kUnavailable, "BPF not loaded");
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        SnapshotAggregatedCounts(batch.get());
        return batch;
    }

    Status Init(const ConfigValue& config) override {
        frequency_hz_ = static_cast<int>(config["frequency_hz"].AsInt(49));
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(1000));
        stack_depth_ = static_cast<int>(config["stack_depth"].AsInt(128));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);

        auto mode = config["mode"].AsString("aggregated");
        stream_mode_ = (mode == "stream");

        bpf_obj_path_ = config["bpf_object"].AsString("");

        ParseCommaSeparatedInts(config["target_pids"].AsString(""),
                                &target_pids_);
        ParseCommaSeparatedStrings(config["target_comms"].AsString(""),
                                   &target_comms_);

        if (stack_depth_ > MAX_STACK_DEPTH)
            stack_depth_ = MAX_STACK_DEPTH;

        (void)stack_depth_;  // BPF objects compile MAX_STACK_DEPTH; kept for config API
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "cpu_profiler: no bpf_object path specified; profiler idle "
                "(install probes and set bpf_object)");
            running_.store(false);
            return Status::Ok();
        }

        auto st = bpf_mgr_.LoadObject("cpu_profiler", bpf_obj_path_);
        if (!st.ok())
            return st;

        int stacks_fd = bpf_mgr_.GetMapFd("cpu_profiler", "stacks");
        int counts_fd = bpf_mgr_.GetMapFd("cpu_profiler", "stack_counts");
        if (stacks_fd < 0 || counts_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: stacks/stack_counts maps missing");
        }
        stacks_fd_ = stacks_fd;
        counts_fd_ = counts_fd;

        ApplyFilterMaps();

        int prog_fd = bpf_mgr_.GetProgFd("cpu_profiler", "on_cpu_sample");
        if (prog_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: on_cpu_sample program missing");
        }

        auto cpus = ParseOnlineCpuIds();
        if (cpus.empty()) {
            IL_WARN("cpu_profiler: could not read online CPUs; defaulting to cpu 0");
            cpus.push_back(0);
        }

        for (int cpu : cpus) {
            struct perf_event_attr attr = {};
            attr.size = sizeof(attr);
            attr.type = PERF_TYPE_SOFTWARE;
            attr.config = PERF_COUNT_SW_CPU_CLOCK;
            attr.freq = 1;
            attr.sample_freq = static_cast<uint64_t>(frequency_hz_);
            attr.sample_type = PERF_SAMPLE_CALLCHAIN;
            attr.disabled = 1;
            attr.exclude_user = user_stacks_ ? 0 : 1;
            attr.exclude_kernel = kernel_stacks_ ? 0 : 1;

            int fd = static_cast<int>(
                PerfEventOpenSys(&attr, /*pid=*/-1, cpu, /*group=*/-1,
                                 PERF_FLAG_FD_CLOEXEC));
            if (fd < 0) {
                IL_WARN("cpu_profiler: perf_event_open failed for cpu %d errno=%d",
                        cpu, errno);
                continue;
            }

            if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
                IL_WARN("cpu_profiler: PERF_EVENT_IOC_SET_BPF failed cpu %d errno=%d",
                        cpu, errno);
                close(fd);
                continue;
            }

            if (ioctl(fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
                IL_WARN("cpu_profiler: PERF_EVENT_IOC_ENABLE failed cpu %d errno=%d",
                        cpu, errno);
                close(fd);
                continue;
            }

            perf_fds_.push_back(fd);
        }

        if (perf_fds_.empty()) {
            return Status::Error(StatusCode::kInternal,
                                 "cpu_profiler: failed to open any perf events");
        }

        running_.store(true);

        if (stream_mode_) {
            int rb_fd = bpf_mgr_.GetMapFd("cpu_profiler", "cpu_events");
            if (rb_fd < 0) {
                return Status::Error(StatusCode::kInternal,
                                     "cpu_profiler: cpu_events ringbuf missing");
            }
            ring_buf_ = bpf_mgr_.CreateRingBuffer(rb_fd, HandleStreamEvent, this);
            if (!ring_buf_) {
                return Status::Error(StatusCode::kInternal,
                                     "cpu_profiler: ring buffer init failed");
            }
            poll_thread_ = std::thread([this] { StreamPollLoop(); });
        } else {
            agg_thread_ = std::thread([this] { AggregatedPullLoop(); });
        }

        IL_INFO("cpu_profiler started (%s mode, %zu perf fds)",
                stream_mode_ ? "stream" : "aggregated", perf_fds_.size());
        return Status::Ok();
    }

    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (agg_thread_.joinable())
            agg_thread_.join();

        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }

        for (int fd : perf_fds_) {
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            close(fd);
        }
        perf_fds_.clear();

        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

private:
    void ApplyFilterMaps() {
        int cfg_fd = bpf_mgr_.GetMapFd("cpu_profiler", "cpu_profiler_cfg");
        uint32_t flags =
            (stream_mode_ ? 1u : 0u) |
            (!target_pids_.empty() ? 2u : 0u) |
            (!target_comms_.empty() ? 4u : 0u);
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &flags, BPF_ANY);
        }

        int pid_fd = bpf_mgr_.GetMapFd("cpu_profiler", "target_pids");
        if (pid_fd >= 0 && !target_pids_.empty()) {
            uint8_t one = 1;
            for (uint32_t pid : target_pids_)
                bpf_map_update_elem(pid_fd, &pid, &one, BPF_ANY);
        }

        int comm_fd = bpf_mgr_.GetMapFd("cpu_profiler", "target_comms");
        if (comm_fd >= 0 && !target_comms_.empty()) {
            uint8_t one = 1;
            for (const auto& name : target_comms_) {
                char key[TASK_COMM_LEN] = {};
                std::memcpy(key, name.c_str(),
                            std::min(name.size(), sizeof(key) - 1));
                bpf_map_update_elem(comm_fd, key, &one, BPF_ANY);
            }
        }
    }

    void StreamPollLoop() {
        while (running_.load()) {
            int err = ring_buffer__poll(ring_buf_, 100);
            if (err < 0 && err != -EINTR)
                IL_WARN("cpu_profiler: ringbuf poll err %d", err);
        }
    }

    void AggregatedPullLoop() {
        using namespace std::chrono_literals;
        while (running_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(aggregate_interval_ms_));
            if (!running_.load())
                break;
            FlushAggregatedCounts();
        }
    }

    void LookupStackFrames(int32_t stack_id, std::vector<StackFrame>* out) {
        out->clear();
        if (stack_id < 0 || stacks_fd_ < 0)
            return;

        uint64_t raw[MAX_STACK_DEPTH];
        std::memset(raw, 0, sizeof(raw));
        uint32_t sid = static_cast<uint32_t>(stack_id);
        if (bpf_map_lookup_elem(stacks_fd_, &sid, raw) != 0)
            return;

        for (int i = 0; i < MAX_STACK_DEPTH && i < stack_depth_; ++i) {
            if (raw[i] == 0)
                break;
            StackFrame fr;
            fr.address = raw[i];
            out->push_back(fr);
        }
    }

    void SnapshotAggregatedCounts(DataBatch* batch) {
        if (counts_fd_ < 0)
            return;

        il_stack_key cur{};
        il_stack_key next{};
        std::vector<il_stack_key> keys;

        int err = bpf_map_get_next_key(counts_fd_, nullptr, &cur);
        while (err == 0) {
            keys.push_back(cur);
            err = bpf_map_get_next_key(counts_fd_, &cur, &next);
            cur = next;
        }

        for (const auto& key : keys) {
            uint64_t count = 0;
            if (bpf_map_lookup_elem(counts_fd_, &key, &count) != 0)
                continue;

            auto& sample = batch->AddStackSample();
            sample.pid = key.pid;
            sample.tid = key.tid;
            sample.comm = batch->InternString(std::string_view(
                key.comm, strnlen(key.comm, TASK_COMM_LEN)));
            sample.count = count;
            sample.sample_type = SampleType::kOnCpu;
            sample.kernel_stack_id = key.kernel_stack_id;
            sample.user_stack_id = key.user_stack_id;

            LookupStackFrames(key.kernel_stack_id, &sample.kernel_stack);
            LookupStackFrames(key.user_stack_id, &sample.user_stack);
        }
    }

    void FlushAggregatedCounts() {
        if (counts_fd_ < 0 || !callback_)
            return;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        SnapshotAggregatedCounts(batch.get());

        il_stack_key cur{};
        il_stack_key next{};
        int err = bpf_map_get_next_key(counts_fd_, nullptr, &cur);
        while (err == 0) {
            il_stack_key to_delete = cur;
            err = bpf_map_get_next_key(counts_fd_, &cur, &next);
            cur = next;
            bpf_map_delete_elem(counts_fd_, &to_delete);
        }

        if (!batch->stack_samples().empty()) {
            {
                std::lock_guard<std::mutex> lock(last_batch_mu_);
                last_batch_ = batch;
            }
            callback_(std::move(batch));
        }
    }

    static int HandleStreamEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<CpuProfilerSource*>(ctx);
        if (size < sizeof(il_cpu_sample_event))
            return 0;
        auto* ev = static_cast<il_cpu_sample_event*>(data);

        if (!self->callback_)
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();
        sample.pid = ev->pid;
        sample.tid = ev->tid;
        sample.cpu = ev->cpu;
        sample.comm = batch->InternString(std::string_view(
            ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
        sample.count = 1;
        sample.sample_type = SampleType::kOnCpu;
        sample.kernel_stack_id = ev->kernel_stack_id;
        sample.user_stack_id = ev->user_stack_id;

        self->LookupStackFrames(ev->kernel_stack_id, &sample.kernel_stack);
        self->LookupStackFrames(ev->user_stack_id, &sample.user_stack);

        self->callback_(std::move(batch));
        return 0;
    }

    int frequency_hz_ = 49;
    uint32_t aggregate_interval_ms_ = 1000;
    int stack_depth_ = MAX_STACK_DEPTH;
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    bool stream_mode_ = false;

    std::string bpf_obj_path_;
    std::vector<uint32_t> target_pids_;
    std::vector<std::string> target_comms_;

    std::atomic<bool> running_{false};
    BpfProgramManager bpf_mgr_;
    int stacks_fd_ = -1;
    int counts_fd_ = -1;
    std::vector<int> perf_fds_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    std::thread agg_thread_;

    mutable std::mutex last_batch_mu_;
    DataBatchPtr last_batch_;
};

IL_REGISTER_SOURCE("cpu_profiler", CpuProfilerSource);

}  // namespace illuminator
