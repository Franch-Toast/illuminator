// ============================================================================
// EbpfIoMonitor — 基于 eBPF 的块设备 I/O 延迟监控器（Push 模式）
// ============================================================================
//
// 使用 eBPF tracepoint 监控块设备 I/O 请求的生命周期，
// 在 block_rq_issue（请求发出）和 block_rq_complete（请求完成）两个点
// 采集时间戳，计算 I/O 延迟。
//
// 输出 Record 指标：
// ===================
// 每条 I/O 事件记录包含：
//   - 进程信息（pid, comm）
//   - I/O 延迟（latency_us 微秒）
//   - 操作信息（sector 扇区号, nr_sector 扇区数, rw 读/写）
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

class EbpfIoMonitor : public SourcePlugin {
public:
    const char* Name() const override { return "ebpf_io_monitor"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsPushMode() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    Status Init(const ConfigValue& config) override {
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("ebpf_io_monitor: no BPF object, idle mode");
            stub_mode_ = true;
            return Status::Ok();
        }

        auto status = bpf_mgr_.LoadObject("bio_latency", bpf_obj_path_);
        if (!status.ok()) return status;

        // 挂载 block_rq_issue 和 block_rq_complete 两个 tracepoint 探针
        status = bpf_mgr_.AttachPrograms("bio_latency",
            {"trace_block_rq_issue", "trace_block_rq_complete"});
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd("bio_latency", "bio_events");
        if (map_fd < 0) return Status::Error(StatusCode::kInternal, "bio_events not found");

        ring_buf_ = bpf_mgr_.CreateRingBuffer(map_fd, HandleEvent, this);
        if (!ring_buf_) return Status::Error(StatusCode::kInternal, "ringbuf create failed");

        running_ = true;
        poll_thread_ = std::thread([this] {
            SetThreadName("io-poll");
            while (running_) ring_buffer__poll(ring_buf_, 100);
        });
        IL_INFO("eBPF I/O monitor started");
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
    // Ring Buffer 事件回调：将 BPF I/O 事件转为 DataBatch Record
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
                     static_cast<uint64_t>(event->latency_ns / 1000));  // ns → us
        rec.SetField(batch->InternString("sector"), event->sector);
        rec.SetField(batch->InternString("nr_sector"), static_cast<uint64_t>(event->nr_sector));
        rec.SetField(batch->InternString("rw"),
                     batch->InternString(event->rwflag ? "write" : "read"));

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

IL_REGISTER_SOURCE("ebpf_io_monitor", EbpfIoMonitor);

}  // namespace illuminator
