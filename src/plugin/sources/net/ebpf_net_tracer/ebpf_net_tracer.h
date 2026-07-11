// ============================================================================
// EbpfNetTracer — 基于 eBPF 的 TCP 连接追踪器（Skeleton Push 模式）
// ============================================================================
//
// 使用 bpftool gen skeleton 生成的类型安全骨架加载 BPF 程序。
//
// eBPF tracepoint: inet_sock_set_state
// 输出: pid, comm, saddr, daddr, sport, dport, event
// ============================================================================

#pragma once

#include <arpa/inet.h>
#include <cstring>

#include "net_tracer_sk.skel.h"
#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_skeleton_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

IL_DEFINE_SKEL_OPS(NetTracerSkelOps, net_tracer_sk,
                   net_events, collection_gate, "net-poll");

class EbpfNetTracer : public EbpfSkeletonSource<NetTracerSkelOps> {
public:
    const char* Name() const override { return "ebpf_net_tracer"; }
    const char* Version() const override { return "0.2.0"; }

protected:
    ring_buffer_sample_fn EventCallback() const override {
        return HandleEvent;
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfNetTracer*>(ctx);
        if (size < sizeof(il_net_event)) return 0;

        auto* event = static_cast<il_net_event*>(data);
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();

        char saddr_str[INET_ADDRSTRLEN], daddr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &event->saddr, saddr_str, sizeof(saddr_str));
        inet_ntop(AF_INET, &event->daddr, daddr_str, sizeof(daddr_str));

        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("net_connection")});
        rec.SetField(batch->InternString("pid"), static_cast<uint64_t>(event->pid));
        rec.SetField(batch->InternString("comm"),
                     batch->InternString(std::string_view(event->comm,
                         strnlen(event->comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("saddr"), batch->InternString(saddr_str));
        rec.SetField(batch->InternString("daddr"), batch->InternString(daddr_str));
        rec.SetField(batch->InternString("sport"), static_cast<uint64_t>(event->sport));
        rec.SetField(batch->InternString("dport"), static_cast<uint64_t>(event->dport));

        const char* evt_types[] = {"connect", "accept", "close"};
        int idx = event->event_type < 3 ? event->event_type : 0;
        rec.SetField(batch->InternString("event"), batch->InternString(evt_types[idx]));

        if (self->callback_) self->callback_(std::move(batch));
        return 0;
    }
};

IL_REGISTER_SOURCE("ebpf_net_tracer", EbpfNetTracer);

}  // namespace illuminator
