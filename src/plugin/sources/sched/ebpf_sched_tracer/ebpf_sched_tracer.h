// ============================================================================
// EbpfSchedTracer — 基于 eBPF 的调度器事件追踪器（Push 模式）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 ebpf_sched_tracer.legacy.h 作为功能参考。
//
// eBPF tracepoint: sched_wakeup + sched_switch
// 输出: prev_pid, next_pid, cpu, latency_us, prev_comm, next_comm, event
// ============================================================================

#pragma once

#include <cstring>

#include "sched_tracer_sk.skel.h"
#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_source_base.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class EbpfSchedTracer : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(sched_tracer_sk);

public:
    const char* Name() const override { return "ebpf_sched_tracer"; }
    const char* Version() const override { return "2.0.0"; }
    bool IsPushMode() const override { return true; }

protected:
    void OnConfigureMaps(void* /*s*/) override {
        SetRingBufFd(bpf_map__fd(skel()->maps.sched_events));
        SetGateFd(bpf_map__fd(skel()->maps.collection_gate));
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return HandleEvent;
    }

    DataBatchPtr MakePushBatch() override {
        return std::make_shared<DataBatch>(DataBatch::Type::kTrace);
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfSchedTracer*>(ctx);
        if (size < sizeof(il_sched_event) || !self->pending_batch_) return 0;
        auto* ev = static_cast<il_sched_event*>(data);
        auto* batch = self->pending_batch_.get();

        const char* evt_types[] = {"switch", "wakeup"};
        int idx = ev->event_type < 2 ? ev->event_type : 0;

        auto& rec = batch->AddRecord();
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
        rec.SetField(batch->InternString("latency_us"),
                     static_cast<uint64_t>(ev->latency_ns / 1000));
        rec.SetField(batch->InternString("prev_comm"),
                     batch->InternString(std::string_view(
                         ev->prev_comm, strnlen(ev->prev_comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("next_comm"),
                     batch->InternString(std::string_view(
                         ev->next_comm, strnlen(ev->next_comm, TASK_COMM_LEN))));
        return 0;
    }
};

IL_REGISTER_SOURCE("ebpf_sched_tracer", EbpfSchedTracer);

}  // namespace illuminator
