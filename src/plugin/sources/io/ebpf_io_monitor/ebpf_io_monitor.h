// ============================================================================
// EbpfIoMonitor — eBPF 块设备 I/O 延迟监控器（Skeleton Push 模式）
// ============================================================================
//
// 使用 bpftool gen skeleton 生成的类型安全骨架加载 BPF 程序。
// BPF 字节码嵌入二进制，无需外部 .bpf.o 文件。
//
// eBPF tracepoint: block_rq_issue + block_rq_complete
// 输出: pid, comm, latency_us, sector, nr_sector, rw
// ============================================================================

#pragma once

#include <cstring>

#include "bio_latency_sk.skel.h"
#include "ebpf/include/event_types.h"
#include "plugin/sources/ebpf_skeleton_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

IL_DEFINE_SKEL_OPS_WITH_META(BioLatencySkelOps, bio_latency_sk,
                             bio_events, collection_gate, "io-poll",
                             meta_stats);

class EbpfIoMonitor : public EbpfSkeletonSource<BioLatencySkelOps> {
public:
    const char* Name() const override { return "ebpf_io_monitor"; }
    const char* Version() const override { return "0.2.0"; }

protected:
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
