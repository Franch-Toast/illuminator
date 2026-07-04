// ============================================================================
// EbpfRingBufferSource — eBPF Ring Buffer Push Source 基类
// ============================================================================
//
// 抽取自 EbpfSchedTracer / EbpfIoMonitor / EbpfNetTracer 的公共样板代码。
//
// 子类只需提供：
//   1. Name() / Version() 标识
//   2. BpfConfig() 描述 BPF 对象名、程序列表、map 名、线程名
//   3. HandleEvent() 静态回调实现事件特有的数据转换
//
// 基类统一处理：
//   - Init: 读取 bpf_object 配置
//   - Start: 加载 BPF → 挂载程序 → 创建 RingBuffer → 启动轮询线程
//   - Stop: 停止轮询 → 释放 RingBuffer → 分离 BPF 程序
//   - stub_mode: 无 BPF 对象时的降级模式
// ============================================================================

#pragma once

#include <string>
#include <thread>
#include <vector>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {

struct EbpfSourceBpfConfig {
    std::string object_name;                // BPF 逻辑对象名
    std::vector<std::string> program_names; // 要挂载的 BPF 程序列表
    std::string map_name;                   // RingBuffer map 名称
    std::string thread_name;                // 轮询线程名
};

class EbpfRingBufferSource : public SourcePlugin {
public:
    bool IsPushMode() const override { return true; }
    bool IsStub() const override { return stub_mode_; }

    Status Init(const ConfigValue& config) override {
        bpf_obj_path_ = config["bpf_object"].AsString("");
        return OnInit(config);
    }

    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN("{}: no BPF object, idle mode", Name());
            stub_mode_ = true;
            return OnStubStart();
        }

        auto bpf_cfg = BpfConfig();

        auto status = bpf_mgr_.LoadObject(bpf_cfg.object_name, bpf_obj_path_);
        if (!status.ok()) return status;

        status = bpf_mgr_.AttachPrograms(
            bpf_cfg.object_name, bpf_cfg.program_names);
        if (!status.ok()) return status;

        int map_fd = bpf_mgr_.GetMapFd(
            bpf_cfg.object_name, bpf_cfg.map_name);
        if (map_fd < 0) {
            return Status::Error(StatusCode::kInternal,
                bpf_cfg.map_name + " map not found");
        }

        ring_buf_ = bpf_mgr_.CreateRingBuffer(
            map_fd, EventCallback(), this);
        if (!ring_buf_) {
            return Status::Error(StatusCode::kInternal,
                "Failed to create ring buffer for " + bpf_cfg.object_name);
        }

        running_ = true;
        poll_thread_ = std::thread([this, thread_name = bpf_cfg.thread_name] {
            SetThreadName(thread_name.c_str());
            while (running_) ring_buffer__poll(ring_buf_, 100);
        });

        IL_INFO("{} started (BPF: {})", Name(), bpf_cfg.object_name);
        return Status::Ok();
    }

    Status Stop() override {
        running_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) { ring_buffer__free(ring_buf_); ring_buf_ = nullptr; }
        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

protected:
    // 子类必须实现：提供 BPF 配置
    virtual EbpfSourceBpfConfig BpfConfig() const = 0;

    // 子类必须实现：返回 ring_buffer 回调函数指针
    virtual ring_buffer_sample_fn EventCallback() const = 0;

    // 子类可选覆写：额外的 Init 逻辑
    virtual Status OnInit(const ConfigValue& /*config*/) {
        return Status::Ok();
    }

    // 子类可选覆写：stub 模式的特殊启动逻辑
    virtual Status OnStubStart() {
        return Status::Ok();
    }

    std::string bpf_obj_path_;
    bool running_ = false;
    bool stub_mode_ = false;
    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
};

}  // namespace illuminator
