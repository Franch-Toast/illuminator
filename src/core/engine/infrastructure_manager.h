// ============================================================================
// InfrastructureManager — 基础设施管理器
// ============================================================================
//
// 最底层组件，只管理共享基础设施（定时器、线程池），不知道 Feature 或 Pipeline。
//
// 职责：
//   1. 管理 TimerWheel（全局定时调度器）
//   2. 管理 CollectPool（采集线程池）
//   3. 管理 SinkPool（写入线程池，队列满时丢弃）
//   4. 基础设施的启动和停止
//
// 不负责：
//   ✗ 构建 Pipeline
//   ✗ 注册定时器
//   ✗ 管理 Feature 集合
//
// 线程安全：
//   Start/Stop 应只从主线程调用一次。Get* 方法在 Start 之后可安全并发调用。
// ============================================================================

#pragma once

#include <memory>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/timer_wheel.h"
#include "core/threading/thread_pool.h"

namespace illuminator {

struct InfrastructureConfig {
    size_t collect_pool_threads = 2;
    size_t sink_pool_threads = 0;  // 0 = auto (hardware_concurrency / 2, min 2)
};

class InfrastructureManager {
public:
    using Config = InfrastructureConfig;

    static InfrastructureManager& Instance() {
        static InfrastructureManager inst;
        return inst;
    }

    Status Start(const Config& config = Config{}) {
        if (started_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "InfrastructureManager already started");
        }

        size_t sink_threads = config.sink_pool_threads;
        if (sink_threads == 0) {
            sink_threads = std::max(2u, std::thread::hardware_concurrency() / 2);
        }

        collect_pool_ = std::make_unique<ThreadPool>(
            config.collect_pool_threads, "collecter");
        sink_pool_ = std::make_unique<ThreadPool>(sink_threads, "sink-write");

        timer_.Start();

        IL_INFO("InfrastructureManager started: CollectPool={} threads, SinkPool={} threads",
                config.collect_pool_threads, sink_threads);
        started_ = true;
        return Status::Ok();
    }

    Status Stop() {
        if (!started_) return Status::Ok();

        timer_.Stop();
        collect_pool_.reset();
        sink_pool_.reset();

        IL_INFO("InfrastructureManager stopped");
        started_ = false;
        return Status::Ok();
    }

    bool IsStarted() const { return started_; }

    TimerWheel& GetTimerWheel() { return timer_; }
    ThreadPool* GetCollectPool() { return collect_pool_.get(); }
    ThreadPool* GetSinkPool() { return sink_pool_.get(); }

private:
    InfrastructureManager() = default;
    ~InfrastructureManager() { Stop(); }

    InfrastructureManager(const InfrastructureManager&) = delete;
    InfrastructureManager& operator=(const InfrastructureManager&) = delete;

    bool started_ = false;
    TimerWheel timer_;
    std::unique_ptr<ThreadPool> collect_pool_;
    std::unique_ptr<ThreadPool> sink_pool_;
};

}  // namespace illuminator
