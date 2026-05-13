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

#include <arpa/inet.h>           // inet_ntop
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

class EbpfNetTracer : public SourcePlugin {
public:
    const char* Name() const override { return "ebpf_net_tracer"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsPushMode() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    Status Init(const ConfigValue& config) override {
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return Status::Ok();
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("ebpf_net_tracer: no BPF object, idle mode");
            stub_mode_ = true;
            return StartProcFallback();
        }

        auto status = bpf_mgr_.LoadObject("net_tracer", bpf_obj_path_);
        if (!status.ok()) return status;

        // 挂载 inet_sock_set_state tracepoint BPF 程序
        status = bpf_mgr_.AttachProgram("net_tracer", "trace_inet_sock_set_state");
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd("net_tracer", "net_events");
        if (map_fd < 0) {
            return Status::Error(StatusCode::kInternal, "net_events map not found");
        }

        ring_buf_ = bpf_mgr_.CreateRingBuffer(map_fd, HandleEvent, this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal, "Failed to create ring buffer");
        }

        running_ = true;
        poll_thread_ = std::thread([this] {
            SetThreadName("il-net-poll");
            PollLoop();
        });
        IL_INFO("eBPF net tracer started");
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
    // /proc/net/tcp 备选启动（当前仅设置状态）
    Status StartProcFallback() {
        running_ = true;
        return Status::Ok();
    }

    void PollLoop() {
        while (running_) {
            ring_buffer__poll(ring_buf_, 100 /* ms */);
        }
    }

    // Ring Buffer 事件回调：将 BPF 事件转为 DataBatch Record
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

    std::string bpf_obj_path_;
    bool running_ = false;
    bool stub_mode_ = false;
    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

IL_REGISTER_SOURCE("ebpf_net_tracer", EbpfNetTracer);

}  // namespace illuminator
