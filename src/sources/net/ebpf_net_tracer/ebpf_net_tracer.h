// ============================================================================
// EbpfNetTracer — 基于 eBPF 的 TCP 连接追踪器（Push 模式）
// ============================================================================
//
// 使用 eBPF tracepoint 跟踪 TCP 连接的生命周期事件（连接建立/关闭）。
// 挂钩点在 inet_sock_set_state，自动过滤 IPv4 协议族。
//
// 输出 Record 指标：
// ===================
// 每条连接事件记录包含：
//   - 进程信息（pid, comm）
//   - 源/目标地址和端口（saddr, daddr, sport, dport）
//   - 事件类型（connect/accept/close）
//
// 备选模式（无 bpf_object 时）：
//   - 自动启用 /proc/net/tcp fallback（当前为空实现）
// ============================================================================

#pragma once

#include <arpa/inet.h>
#include <cstring>

#include "ebpf/include/event_types.h"
#include "sources/ebpf_ring_buffer_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class EbpfNetTracer : public EbpfRingBufferSource {
public:
    const char* Name() const override { return "ebpf_net_tracer"; }
    const char* Version() const override { return "0.1.0"; }

protected:
    EbpfSourceBpfConfig BpfConfig() const override {
        return {"net_tracer",
                {"trace_inet_sock_set_state"},
                "net_events",
                "net-poll"};
    }

    ring_buffer_sample_fn EventCallback() const override {
        return HandleEvent;
    }

    Status OnStubStart() override {
        running_ = true;
        return Status::Ok();
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfNetTracer*>(ctx);
        if (size < sizeof(il_net_event)) return 0;

        auto* event = static_cast<il_net_event*>(data);
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();

        // 二进制 IP 转字符串（IPv4 点分十进制）
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

        // 事件类型映射
        const char* evt_types[] = {"connect", "accept", "close"};
        int idx = event->event_type < 3 ? event->event_type : 0;
        rec.SetField(batch->InternString("event"), batch->InternString(evt_types[idx]));

        if (self->callback_) self->callback_(std::move(batch));
        return 0;
    }

    // Members inherited from EbpfRingBufferSource
};

IL_REGISTER_SOURCE("ebpf_net_tracer", EbpfNetTracer);

}  // namespace illuminator
