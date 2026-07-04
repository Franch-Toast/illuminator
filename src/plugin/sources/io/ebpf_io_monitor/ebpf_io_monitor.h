// ============================================================================
// EbpfIoMonitor — eBPF 块设备 I/O 延迟监控器（Push 模式）
// ============================================================================
//
// 继承 EbpfRingBufferSource 基类，仅实现 I/O 事件特有的配置和事件解析。
//
// eBPF tracepoint: block_rq_issue + block_rq_complete
// 输出: pid, comm, latency_us, sector, nr_sector, rw
// ============================================================================

#pragma once

#include <cstring>

#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_ring_buffer_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class EbpfIoMonitor : public EbpfRingBufferSource {
public:
    const char* Name() const override { return "ebpf_io_monitor"; }
    const char* Version() const override { return "0.1.0"; }

protected:
    EbpfSourceBpfConfig BpfConfig() const override {
        return {"bio_latency",
                {"trace_block_rq_issue", "trace_block_rq_complete"},
                "bio_events",
                "io-poll"};
    }

    ring_buffer_sample_fn EventCallback() const override {
        return HandleEvent;
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfIoMonitor*>(ctx);
        if (size < sizeof(il_bio_event)) return 0;
        auto* event = static_cast<il_bio_event*>(data);

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();

        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("bio")});
        rec.SetField(batch->InternString("pid"), static_cast<uint64_t>(event->pid));
        rec.SetField(batch->InternString("comm"),
                     batch->InternString(std::string_view(event->comm,
                         strnlen(event->comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("latency_us"),
                     static_cast<uint64_t>(event->latency_ns / 1000));
        rec.SetField(batch->InternString("sector"), event->sector);
        rec.SetField(batch->InternString("nr_sector"), static_cast<uint64_t>(event->nr_sector));
        rec.SetField(batch->InternString("rw"),
                     batch->InternString(event->rwflag ? "write" : "read"));

        if (self->callback_) self->callback_(std::move(batch));
        return 0;
    }
};

IL_REGISTER_SOURCE("ebpf_io_monitor", EbpfIoMonitor);

}  // namespace illuminator
