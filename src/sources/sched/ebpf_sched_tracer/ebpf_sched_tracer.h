// ============================================================================
// EbpfSchedTracer — 基于 eBPF 的调度器事件追踪器（Push 模式）
// ============================================================================
//
// 使用 eBPF tracepoint 追踪内核调度器事件：
//   - sched_wakeup: 进程被唤醒，记录唤醒时间戳用于计算运行队列延迟
//   - sched_switch: 上下文切换，计算实际的运行队列等待时间
//
// 输出 Record 指标：
// ===================
// 每条调度事件记录包含：
//   - prev_pid/next_pid: 被切换出的进程 / 被切换进的进程
//   - cpu: 发生在哪个 CPU 核
//   - latency_us: 运行队列延迟（微秒，仅在 sched_switch 时有意义）
//   - prev_comm/next_comm: 进程名
//   - event 类型: "switch" 或 "wakeup"
//
// 备选模式（无 bpf_object 时）：静默 IDLE 模式，不产生数据
// ============================================================================

#pragma once

#include <cstring>
#include <string>
#include <thread>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class EbpfSchedTracer : public SourcePlugin {
public:
    const char* Name() const override { return "ebpf_sched_tracer"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsPushMode() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    Status Init(const ConfigValue& config) override {
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("ebpf_sched_tracer: no BPF object, idle mode");
            stub_mode_ = true;
            return Status::Ok();
        }

        auto status = bpf_mgr_.LoadObject("sched_tracer", bpf_obj_path_);
        if (!status.ok()) return status;

        // 挂载 sched_wakeup 和 sched_switch 两个 tracepoint BPF 程序
        status = bpf_mgr_.AttachPrograms("sched_tracer",
            {"trace_sched_wakeup", "trace_sched_switch"});
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd("sched_tracer", "sched_events");
        if (map_fd < 0) return Status::Error(StatusCode::kInternal, "sched_events not found");

        ring_buf_ = bpf_mgr_.CreateRingBuffer(map_fd, HandleEvent, this);
        if (!ring_buf_) return Status::Error(StatusCode::kInternal, "ringbuf failed");

        running_ = true;
        poll_thread_ = std::thread([this] {
            SetThreadName("il-schedtr-pol");
            while (running_) ring_buffer__poll(ring_buf_, 100);
        });
        IL_INFO("eBPF sched tracer started");
        return Status::Ok();
    }

    Status Stop() override {
        running_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) { ring_buffer__free(ring_buf_); ring_buf_ = nullptr; }
        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

private:
    // Ring Buffer 事件回调：将调度事件转为 DataBatch Record
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfSchedTracer*>(ctx);
        if (size < sizeof(il_sched_event)) return 0;
        auto* event = static_cast<il_sched_event*>(data);

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        auto& rec = batch->AddRecord();

        const char* evt_types[] = {"switch", "wakeup"};
        int idx = event->event_type < 2 ? event->event_type : 0;

        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("sched")});
        rec.labels.push_back({batch->InternString("event"),
                              batch->InternString(evt_types[idx])});

        rec.SetField(batch->InternString("prev_pid"), static_cast<uint64_t>(event->prev_pid));
        rec.SetField(batch->InternString("next_pid"), static_cast<uint64_t>(event->next_pid));
        rec.SetField(batch->InternString("cpu"), static_cast<uint64_t>(event->cpu));
        rec.SetField(batch->InternString("latency_us"),
                     static_cast<uint64_t>(event->latency_ns / 1000));
        rec.SetField(batch->InternString("prev_comm"),
                     batch->InternString(std::string_view(event->prev_comm,
                         strnlen(event->prev_comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("next_comm"),
                     batch->InternString(std::string_view(event->next_comm,
                         strnlen(event->next_comm, TASK_COMM_LEN))));

        if (self->callback_) self->callback_(std::move(batch));
        return 0;
    }

    std::string bpf_obj_path_;
    bool running_ = false;
    bool stub_mode_ = false;
    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

IL_REGISTER_SOURCE("ebpf_sched_tracer", EbpfSchedTracer);

}  // namespace illuminator
