#pragma once

#include <cstring>
#include <string>
#include <thread>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <unistd.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// eBPF-based CPU profiler using perf_event sampling.
// Attaches a BPF program to PERF_COUNT_SW_CPU_CLOCK events.
class EbpfCpuSampler : public SourcePlugin {
public:
    const char* Name() const override { return "ebpf_cpu_sampler"; }
    const char* Version() const override { return "0.1.0"; }

    bool IsPushMode() const override { return true; }

    Status Init(const ConfigValue& config) override {
        frequency_hz_ = static_cast<int>(config["frequency_hz"].AsInt(49));
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("ebpf_cpu_sampler: no BPF object path specified, "
                     "falling back to /proc-based sampling");
            fallback_mode_ = true;
            return StartFallback();
        }

        auto status = bpf_mgr_.LoadObject("cpu_sampler", bpf_obj_path_);
        if (!status.ok()) return status;

        status = bpf_mgr_.AttachProgram("cpu_sampler", "on_cpu_sample");
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd("cpu_sampler", "cpu_events");
        if (map_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                "cpu_events ring buffer map not found");
        }

        ring_buf_ = bpf_mgr_.CreateRingBuffer(map_fd, HandleEvent, this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal,
                "Failed to create ring buffer");
        }

        running_ = true;
        poll_thread_ = std::thread([this] { PollLoop(); });
        IL_INFO("eBPF CPU sampler started at %d Hz", frequency_hz_);
        return Status::Ok();
    }

    Status Stop() override {
        running_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

private:
    Status StartFallback() {
        running_ = true;
        return Status::Ok();
    }

    void PollLoop() {
        while (running_) {
            int err = ring_buffer__poll(ring_buf_, 100 /* ms */);
            if (err < 0 && err != -EINTR) {
                IL_WARN("Ring buffer poll error: %d", err);
            }
        }
    }

    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfCpuSampler*>(ctx);
        if (size < sizeof(il_cpu_sample_event)) return 0;

        auto* event = static_cast<il_cpu_sample_event*>(data);

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();

        sample.pid = event->pid;
        sample.tid = event->tid;
        sample.comm = batch->InternString(
            std::string_view(event->comm, strnlen(event->comm, TASK_COMM_LEN)));

        // Stack frames would be resolved via /proc/pid/maps and symbolizer
        StackFrame frame;
        frame.address = 0;
        frame.function_name = batch->InternString("[BPF stack]");
        sample.user_stack.push_back(frame);

        if (self->callback_) {
            self->callback_(std::move(batch));
        }
        return 0;
    }

    int frequency_hz_ = 49;
    std::string bpf_obj_path_;
    bool fallback_mode_ = false;
    bool running_ = false;

    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

IL_REGISTER_SOURCE("ebpf_cpu_sampler", EbpfCpuSampler);

}  // namespace illuminator
