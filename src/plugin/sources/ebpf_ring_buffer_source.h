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

#include <atomic>
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

        gate_map_fd_ = bpf_mgr_.GetMapFd(
            bpf_cfg.object_name, GateMapName());

        running_ = true;
        paused_ = false;
        StartPollThread(bpf_cfg.thread_name);
        SetBpfGate(true);

        IL_INFO("{} started (BPF: {})", Name(), bpf_cfg.object_name);
        return Status::Ok();
    }

    Status Stop() override {
        SetBpfGate(false);
        running_ = false;
        paused_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
        if (ring_buf_) { ring_buffer__free(ring_buf_); ring_buf_ = nullptr; }
        bpf_mgr_.DetachAll();
        return Status::Ok();
    }

    // 暂停采集：先关 BPF gate（内核停止发射事件），再停 poll 线程
    Status PauseCollection() override {
        if (stub_mode_) return Status::Ok();
        SetBpfGate(false);
        paused_ = true;
        if (poll_thread_.joinable()) poll_thread_.join();
        IL_INFO("{}: collection paused (BPF gate closed)", Name());
        return Status::Ok();
    }

    // 恢复采集：先启 poll 线程（消费端就绪），再开 BPF gate
    Status ResumeCollection() override {
        if (stub_mode_) return Status::Ok();
        paused_ = false;
        StartPollThread(BpfConfig().thread_name);
        SetBpfGate(true);
        IL_INFO("{}: collection resumed (BPF gate opened)", Name());
        return Status::Ok();
    }

protected:
    virtual EbpfSourceBpfConfig BpfConfig() const = 0;
    virtual ring_buffer_sample_fn EventCallback() const = 0;

    virtual Status OnInit(const ConfigValue& /*config*/) {
        return Status::Ok();
    }

    virtual Status OnStubStart() {
        return Status::Ok();
    }

    // 子类可覆写：指定 gate map 的名称（默认 "collection_gate"）
    virtual const char* GateMapName() const { return "collection_gate"; }

    std::string bpf_obj_path_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool stub_mode_ = false;
    BpfProgramManager bpf_mgr_;
    struct ring_buffer* ring_buf_ = nullptr;
    std::thread poll_thread_;
    int gate_map_fd_ = -1;

private:
    void SetBpfGate(bool enabled) {
        if (gate_map_fd_ < 0) return;
        uint32_t key = 0;
        uint32_t val = enabled ? 1 : 0;
        bpf_map_update_elem(gate_map_fd_, &key, &val, BPF_ANY);
    }

    void StartPollThread(const std::string& thread_name) {
        poll_thread_ = std::thread([this, tn = thread_name] {
            SetThreadName(tn.c_str());
            while (running_ && !paused_) ring_buffer__poll(ring_buf_, 100);
        });
    }
};

}  // namespace illuminator
