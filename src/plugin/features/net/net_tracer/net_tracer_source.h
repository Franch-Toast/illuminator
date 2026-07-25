// ============================================================================
// EbpfNetTracer — 基于 eBPF 的 TCP 连接追踪器（Push 模式）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 ebpf_net_tracer.legacy.h 作为功能参考。
//
// eBPF tracepoint: inet_sock_set_state
// 输出: pid, comm, saddr, daddr, sport, dport, event
// ============================================================================

#pragma once

#include <arpa/inet.h>
#include <cstring>

#include "net_tracer_sk.skel.h"
#include "ebpf_common/include/event_types.h"
#include "plugin/api/ebpf_source_base.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class EbpfNetTracer : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(net_tracer_sk);

public:
    const char* Name() const override { return "ebpf_net_tracer"; }
    const char* Version() const override { return "2.0.0"; }
    bool IsPushMode() const override { return true; }

protected:
    void OnConfigureMaps(void* /*s*/) override {
        SetRingBufFd(bpf_map__fd(skel()->maps.net_events));
        SetGateFd(bpf_map__fd(skel()->maps.collection_gate));
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return HandleEvent;
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfNetTracer*>(ctx);
        if (size < sizeof(il_net_event) || !self->pending_batch_) return 0;
        auto* ev = static_cast<il_net_event*>(data);
        auto* batch = self->pending_batch_.get();

        char saddr_str[INET_ADDRSTRLEN], daddr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ev->saddr, saddr_str, sizeof(saddr_str));
        inet_ntop(AF_INET, &ev->daddr, daddr_str, sizeof(daddr_str));

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("net_connection")});
        rec.SetField(batch->InternString("pid"),
                     static_cast<uint64_t>(ev->pid));
        rec.SetField(batch->InternString("comm"),
                     batch->InternString(std::string_view(
                         ev->comm, strnlen(ev->comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("saddr"),
                     batch->InternString(saddr_str));
        rec.SetField(batch->InternString("daddr"),
                     batch->InternString(daddr_str));
        rec.SetField(batch->InternString("sport"),
                     static_cast<uint64_t>(ev->sport));
        rec.SetField(batch->InternString("dport"),
                     static_cast<uint64_t>(ev->dport));

        const char* evt_types[] = {"connect", "accept", "close"};
        int idx = ev->event_type < 3 ? ev->event_type : 0;
        rec.SetField(batch->InternString("event"),
                     batch->InternString(evt_types[idx]));
        return 0;
    }
};

IL_REGISTER_SOURCE("ebpf_net_tracer", EbpfNetTracer);

}  // namespace illuminator
