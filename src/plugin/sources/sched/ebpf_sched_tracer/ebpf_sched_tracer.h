// ============================================================================
// EbpfSchedTracer — 基于 eBPF 的调度器事件追踪器（Skeleton Push 模式）
// ============================================================================
//
// 使用 bpftool gen skeleton 生成的类型安全骨架加载 BPF 程序。
//
// eBPF tracepoint: sched_wakeup + sched_switch
// 输出: prev_pid, next_pid, cpu, latency_us, prev_comm, next_comm, event
// ============================================================================

#pragma once

#include <cstring>

#include "sched_tracer_sk.skel.h"
#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_skeleton_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

IL_DEFINE_SKEL_OPS_WITH_META(SchedTracerSkelOps, sched_tracer_sk,
                             sched_events, collection_gate, "schedtrc-poll",
                             meta_stats);

class EbpfSchedTracer : public EbpfSkeletonSource<SchedTracerSkelOps> {
public:
    const char* Name() const override { return "ebpf_sched_tracer"; }
    const char* Version() const override { return "0.2.0"; }

protected:
    ring_buffer_sample_fn EventCallback() const override {
        return HandleEvent;
    }

private:
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
};

IL_REGISTER_SOURCE("ebpf_sched_tracer", EbpfSchedTracer);

}  // namespace illuminator
