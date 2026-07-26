// ============================================================================
// EbpfIoMonitor — eBPF 块设备 I/O 延迟监控器（Push 模式）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 ebpf_io_monitor.legacy.h 作为功能参考。
//
// eBPF tracepoint: block_rq_issue + block_rq_complete
// 输出: pid, comm, latency_us, sector, nr_sector, rw
// ============================================================================

#pragma once

#include <cstring>

#include "bio_latency_sk.skel.h"
#include "ebpf_common/include/event_types.h"
#include "plugin/api/ebpf_source_base.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class EbpfIoMonitor : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(bio_latency_sk);

public:
    const char* Name() const override { return "ebpf_io_monitor"; }
    const char* Version() const override { return "2.0.0"; }

protected:
    void OnConfigureMaps(void* /*s*/) override {
        SetRingBufFd(bpf_map__fd(skel()->maps.bio_events));
        SetGateFd(bpf_map__fd(skel()->maps.collection_gate));
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return HandleEvent;
    }

private:
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfIoMonitor*>(ctx);
        if (size < sizeof(il_bio_event) || !self->pending_batch_) return 0;
        auto* ev = static_cast<il_bio_event*>(data);
        auto* batch = self->pending_batch_.get();

        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("bio")});
        rec.SetField(batch->InternString("pid"),
                     static_cast<uint64_t>(ev->pid));
        rec.SetField(batch->InternString("comm"),
                     batch->InternString(std::string_view(
                         ev->comm, strnlen(ev->comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("latency_us"),
                     static_cast<uint64_t>(ev->latency_ns / 1000));
        rec.SetField(batch->InternString("sector"), ev->sector);
        rec.SetField(batch->InternString("nr_sector"),
                     static_cast<uint64_t>(ev->nr_sector));
        rec.SetField(batch->InternString("rw"),
                     batch->InternString(ev->rwflag ? "write" : "read"));
        return 0;
    }
};

IL_REGISTER_SOURCE("ebpf_io_monitor", EbpfIoMonitor);

}  // namespace illuminator
