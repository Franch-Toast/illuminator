// ============================================================================
// EbpfCpuSampler — 基于 eBPF 的 CPU 流式采样器（Push 模式）
// ============================================================================
//
// 使用 Linux perf_event 子系统 + eBPF 程序进行 CPU On-CPU 堆栈采样。
// 工作在 Push 模式：BPF 程序通过 Ring Buffer 推送采样事件到用户态。
//
// 工作流程：
// ==========
// 1. 加载 BPF 对象（cpu_sampler 模块），挂载 on_cpu_sample 程序
// 2. 创建 Ring Buffer 接收 CPU 采样事件
// 3. 每个采样事件转换为 StackSample，通过 callback_ 推送到 Pipeline
//
// 备选模式（无 bpf_object 配置时）：
//   - 自动切换为 fallback 模式（不产生数据，仅返回 Ok）
//   - 日志中输出警告提示
//
// 配置参数：
// ==========
// - frequency_hz: 采样频率（默认 49 Hz，避免与调度时钟 50/100/250 Hz 重合）
// - bpf_object: BPF 编译产物路径（.bpf.o 文件），为空时启用 fallback
//
// 注意：此插件是旧版实现，功能被 cpu_profiler 和 cpu_sampler.bpf.c 覆盖，
// 新项目推荐使用 cpu_profiler Source 插件。
// ============================================================================

#pragma once

#include <cstring>
#include <string>
#include <thread>
#include <sys/syscall.h>              // syscall (SYS_perf_event_open)
#include <linux/perf_event.h>         // perf_event_attr, PERF_COUNT_SW_CPU_CLOCK
#include <unistd.h>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class EbpfCpuSampler : public SourcePlugin {
public:
    const char* Name() const override { return "ebpf_cpu_sampler"; }
    const char* Version() const override { return "0.1.0"; }

    // Push 模式：BPF 事件驱动，通过回调推送数据
    bool IsPushMode() const override { return true; }

    // 初始化：读取采样频率和 BPF 对象路径
    Status Init(const ConfigValue& config) override {
        frequency_hz_ = static_cast<int>(config["frequency_hz"].AsInt(49));
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return Status::Ok();
    }

    // 启动：加载 BPF 程序或进入 fallback 模式
    Status Start() override {
        if (bpf_obj_path_.empty()) {
            // 无 BPF 对象，无法进行 eBPF 采样，日志警告并切换备选模式
            IL_WARN("ebpf_cpu_sampler: no BPF object path specified, "
                     "falling back to /proc-based sampling");
            fallback_mode_ = true;
            return StartFallback();
        }

        auto status = bpf_mgr_.LoadObject("cpu_sampler", bpf_obj_path_);
        if (!status.ok()) return status;

        status = bpf_mgr_.AttachProgram("cpu_sampler", "on_cpu_sample");
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd("cpu_sampler", "cpu_events");
        if (map_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                "cpu_events ring buffer map not found");
        }

        // 创建环形缓冲区，绑定事件回调 HandleEvent
        ring_buf_ = bpf_mgr_.CreateRingBuffer(map_fd, HandleEvent, this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal,
                "Failed to create ring buffer");
        }

        // 启动轮询线程持续消费 BPF 事件
        running_ = true;
        poll_thread_ = std::thread([this] {
            SetThreadName("il-cpusamp-pol");
            PollLoop();
        });
        IL_INFO("eBPF CPU sampler started at {} Hz", frequency_hz_);
        return Status::Ok();
    }

    // 停止：关闭线程，释放 Ring Buffer，卸载 BPF 探针
    Status Stop() override {
        running_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

private:
    // Fallback 启动（无 eBPF 时）
    Status StartFallback() {
        running_ = true;
        return Status::Ok();
    }

    // Ring Buffer 轮询循环
    void PollLoop() {
        while (running_) {
            int err = ring_buffer__poll(ring_buf_, 100 /* ms */);
            if (err < 0 && err != -EINTR) {
                IL_WARN("Ring buffer poll error: {}", err);
            }
        }
    }

    // Ring Buffer 事件回调（静态函数，由 libbpf 调用）
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<EbpfCpuSampler*>(ctx);
        if (size < sizeof(il_cpu_sample_event)) return 0;

        auto* event = static_cast<il_cpu_sample_event*>(data);

        // 将 BPF 事件转换为 DataBatch
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& sample = batch->AddStackSample();
        sample.pid = event->pid;
        sample.tid = event->tid;
        sample.comm = batch->InternString(
            std::string_view(event->comm, strnlen(event->comm, TASK_COMM_LEN)));

        // 堆栈帧占位（实际符号化交由后续 Processor 处理）
        StackFrame frame;
        frame.address = 0;
        frame.function_name = batch->InternString("[BPF stack]");
        sample.user_stack.push_back(frame);

        // 通过回调推送到 Pipeline
        if (self->callback_) {
            self->callback_(std::move(batch));
        }
        return 0;
    }

    int frequency_hz_ = 49;
    std::string bpf_obj_path_;
    bool fallback_mode_ = false;
    bool running_ = false;

    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

IL_REGISTER_SOURCE("ebpf_cpu_sampler", EbpfCpuSampler);

}  // namespace illuminator
