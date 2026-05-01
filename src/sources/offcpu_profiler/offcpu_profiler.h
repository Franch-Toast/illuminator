#pragma once

#include <atomic>
#include <cerrno>
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

inline void OffcpuParseCommaUint32(const std::string& s,
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

class OffcpuProfilerSource : public SourcePlugin {
public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* Version() const override { return "0.1.0"; }

    bool IsPushMode() const override { return true; }

    Status Init(const ConfigValue& config) override {
        min_duration_us_ =
            static_cast<uint32_t>(config["min_duration_us"].AsInt(100));
        user_stacks_ = config["user_stacks"].AsBool(true);
        kernel_stacks_ = config["kernel_stacks"].AsBool(true);
        bpf_obj_path_ = config["bpf_object"].AsString("");
        OffcpuParseCommaUint32(config["target_pids"].AsString(""), &target_pids_);
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        (void)min_duration_us_;
        (void)user_stacks_;
        (void)kernel_stacks_;
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "offcpu_profiler: no bpf_object path specified; profiler idle");
            running_.store(false);
            return Status::Ok();
        }

        auto st = bpf_mgr_.LoadObject("offcpu_profiler", bpf_obj_path_);
        if (!st.ok())
            return st;

        st = bpf_mgr_.AttachProgram("offcpu_profiler", "trace_offcpu");
        if (!st.ok())
            return st;

        stacks_fd_ = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_stacks");

        int rb_fd = bpf_mgr_.GetMapFd("offcpu_profiler", "offcpu_events");
        if (rb_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                                 "offcpu_events ringbuf missing");
        }

        ring_buf_ = bpf_mgr_.CreateRingBuffer(rb_fd, HandleEvent, this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal,
                                 "offcpu ring buffer init failed");
        }

        running_.store(true);
        poll_thread_ = std::thread([this] {
            while (running_.load()) {
                int err = ring_buffer__poll(ring_buf_, 100);
                if (err < 0 && err != -EINTR)
                    IL_WARN("offcpu_profiler: ringbuf poll err %d", err);
            }
        });

        IL_INFO("offcpu_profiler started");
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
        stacks_fd_ = -1;
        return Status::Ok();
    }

private:
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty())
            return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    void LookupStack(int32_t stack_id, std::vector<StackFrame>* out) {
        out->clear();
        if (stack_id < 0 || stacks_fd_ < 0)
            return;

        uint64_t raw[MAX_STACK_DEPTH];
        std::memset(raw, 0, sizeof(raw));
        uint32_t sid = static_cast<uint32_t>(stack_id);
        if (bpf_map_lookup_elem(stacks_fd_, &sid, raw) != 0)
            return;

        for (int i = 0; i < MAX_STACK_DEPTH; ++i) {
            if (raw[i] == 0)
                break;
            StackFrame fr;
            fr.address = raw[i];
            out->push_back(fr);
        }
    }

    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<OffcpuProfilerSource*>(ctx);
        if (size < sizeof(il_offcpu_event))
            return 0;
        auto* ev = static_cast<il_offcpu_event*>(data);

        if (!self->AllowPid(ev->pid))
            return 0;

        if (!self->callback_)
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();
        sample.pid = ev->pid;
        sample.tid = ev->tid;
        sample.cpu = ev->cpu;
        sample.comm = batch->InternString(std::string_view(
            ev->comm, strnlen(ev->comm, TASK_COMM_LEN)));
        sample.sample_type = SampleType::kOffCpu;
        sample.duration_ns = ev->duration_ns;
        sample.count = 1;
        sample.kernel_stack_id = ev->kernel_stack_id;
        sample.user_stack_id = ev->user_stack_id;

        self->LookupStack(ev->kernel_stack_id, &sample.kernel_stack);
        self->LookupStack(ev->user_stack_id, &sample.user_stack);

        self->callback_(std::move(batch));
        return 0;
    }

    uint32_t min_duration_us_ = 100;
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    std::string bpf_obj_path_;
    std::vector<uint32_t> target_pids_;
    std::unordered_set<uint32_t> target_pid_allow_;

    std::atomic<bool> running_{false};
    BpfProgramManager bpf_mgr_;
    int stacks_fd_ = -1;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
