// ============================================================================
// Illuminator Pipeline v3 — 事件驱动异步管道引擎
// ============================================================================
//
// 架构:
//   TimerWheel (1 thread)  — 全局定时调度，触发 Collect 和 Flush 事件
//   CollectPool (M threads) — 并行执行 Source::Collect()
//   ProcessThread (1 per pipeline) — 纯事件处理器 (variant dispatch)
//   SinkPool (K threads)   — 并行执行 Sink::Write()
//
// 数据流:
//   Push Source → channel.TryEnqueue(DataBatch)  ─┐
//   TimerWheel → CollectPool → src.Collect()     ─┤→ AsyncChannel(ChannelItem)
//   TimerWheel → channel.InjectFlush()           ─┘        │
//                                                          ▼
//                                                   ProcessThread
//                                                   match event:
//                                                     DataBatch  → Process → Sink
//                                                     Sentinel   → Flush   → Sink
//
// 线程计数 (N pipelines):
//   1 timer + M collect + N process + K sink
//   典型 10 管道 = 1 + 2 + 10 + 4 = 17 线程
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/self_observability.h"
#include "core/common/status.h"
#include "core/engine/async_channel.h"
#include "core/engine/data_batch.h"
#include "core/engine/timer_wheel.h"
#include "core/threading/thread_pool.h"
#include "core/threading/thread_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

class StorageBackend;

class PipelineController;

// ============================================================================
// Pipeline — 单条异步数据处理管道
// ============================================================================
class Pipeline {
public:
    explicit Pipeline(const std::string& name,
                      size_t channel_capacity = 4096,
                      DropPolicy drop_policy = DropPolicy::kDropNewest,
                      double bp_high = 0.8, double bp_low = 0.2)
        : name_(name),
          ingest_channel_(channel_capacity, drop_policy, bp_high, bp_low) {}
    ~Pipeline() { Stop(); }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    const std::string& name() const { return name_; }

    void SetSource(std::unique_ptr<SourcePlugin> source) {
        source_ = std::move(source);
    }
    SourcePlugin* GetSource() { return source_.get(); }

    void AddProcessor(std::unique_ptr<ProcessorPlugin> proc) {
        processors_.push_back(std::move(proc));
    }

    void SetAggregator(std::unique_ptr<AggregatorPlugin> agg) {
        aggregator_ = std::move(agg);
    }

    void AddSink(std::unique_ptr<SinkPlugin> sink) {
        sinks_.push_back(std::move(sink));
    }

    void SetSinkPool(ThreadPool* pool) { sink_pool_ = pool; }

    Status Start() {
        if (!source_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no source: " + name_);
        }
        if (sinks_.empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no sinks: " + name_);
        }

        auto status = source_->Start();
        if (!status.ok()) return status;

        for (auto& p : processors_) {
            status = p->Start();
            if (!status.ok()) return status;
        }
        if (aggregator_) {
            status = aggregator_->Start();
            if (!status.ok()) return status;
        }
        for (auto& s : sinks_) {
            status = s->Start();
            if (!status.ok()) return status;
        }

        running_.store(true, std::memory_order_release);

        process_thread_ = std::thread([this] {
            SetThreadName(name_.substr(0, 15));
            ProcessLoop();
        });

        if (source_->IsPushMode()) {
            source_->SetCallback([this](DataBatchPtr batch) {
                Enqueue(std::move(batch));
            });
        }

        IL_INFO("Pipeline '{}' started (v3 event-driven, channel capacity={})",
                name_, ingest_channel_.capacity());
        return Status::Ok();
    }

    Status Stop() {
        if (!running_.exchange(false)) return Status::Ok();

        source_->Stop();

        if (process_thread_.joinable()) process_thread_.join();

        for (auto& p : processors_) p->Stop();
        if (aggregator_) aggregator_->Stop();
        for (auto& s : sinks_) {
            s->Flush();
            s->Stop();
        }

        auto& ch = ingest_channel_.stats();
        IL_INFO("Pipeline '{}' stopped (enqueued={}, dequeued={}, dropped={}, "
                "flush_injected={})",
                name_,
                ch.enqueued.load(std::memory_order_relaxed),
                ch.dequeued.load(std::memory_order_relaxed),
                ch.dropped.load(std::memory_order_relaxed),
                ch.flush_injected.load(std::memory_order_relaxed));
        return Status::Ok();
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    void Enqueue(DataBatchPtr batch) {
        if (!batch || batch->Empty()) return;

        if (!ingest_channel_.TryEnqueue(std::move(batch))) {
            IL_WARN("Pipeline '{}': channel full, data dropped", name_);
        }

        bool bp = ingest_channel_.IsBackpressured();
        if (bp != last_backpressure_state_) {
            last_backpressure_state_ = bp;
            source_->OnBackpressure(bp);
            if (bp) {
                IL_WARN("Pipeline '{}': backpressure ON", name_);
            } else {
                IL_INFO("Pipeline '{}': backpressure OFF", name_);
            }
        }
    }

    void InjectFlush() {
        if (!ingest_channel_.InjectFlush()) {
            IL_WARN("Pipeline '{}': failed to inject FlushSentinel", name_);
        }
    }

    bool HasAggregator() const { return aggregator_ != nullptr; }
    uint32_t FlushIntervalMs() const {
        return aggregator_ ? aggregator_->FlushIntervalMs() : 0;
    }

    uint64_t BatchesProcessed() const { return batches_processed_.load(); }
    uint64_t RecordsProcessed() const { return records_processed_.load(); }
    uint64_t ErrorCount() const { return error_count_.load(); }

    uint64_t ChannelEnqueued() const {
        return ingest_channel_.stats().enqueued.load(std::memory_order_relaxed);
    }
    uint64_t ChannelDequeued() const {
        return ingest_channel_.stats().dequeued.load(std::memory_order_relaxed);
    }
    uint64_t ChannelDropped() const {
        return ingest_channel_.stats().dropped.load(std::memory_order_relaxed);
    }
    uint64_t ChannelFlushInjected() const {
        return ingest_channel_.stats().flush_injected.load(std::memory_order_relaxed);
    }
    uint64_t ChannelBackpressureEvents() const {
        return ingest_channel_.stats().backpressure_events.load(std::memory_order_relaxed);
    }
    size_t ChannelSize() const { return ingest_channel_.SizeApprox(); }
    size_t ChannelCapacity() const { return ingest_channel_.capacity(); }
    bool ChannelBackpressured() const { return ingest_channel_.IsBackpressured(); }

    StatusOr<DataBatchPtr> RunProcessors(DataBatchPtr batch) {
        for (auto& proc : processors_) {
            auto result = proc->Process(std::move(batch));
            if (!result.ok())
                return result.status();
            batch = std::move(*result);
        }
        return batch;
    }

private:
    // ====================================================================
    // ProcessLoop — 纯事件处理器
    // 只做: Dequeue → variant dispatch (HandleData | HandleFlush)
    // 不做: I/O, 定时器管理, 调度决策
    // ====================================================================
    void ProcessLoop() {
        uint32_t loop_count = 0;

        while (running_.load(std::memory_order_acquire)) {
            auto item = ingest_channel_.Dequeue(std::chrono::milliseconds(100));
            if (!item) {
                if (++loop_count % 100 == 0) SyncChannelMetrics();
                continue;
            }

            std::visit(Overloaded{
                [this](DataBatchPtr& batch) {
                    HandleData(std::move(batch));
                },
                [this](FlushSentinel&) {
                    HandleFlush();
                },
            }, *item);

            if (++loop_count % 100 == 0) {
                SyncChannelMetrics();
                auto usage = ResourceLimiter::Instance().Check();
                if (usage.memory_exceeded) {
                    IL_WARN("Pipeline '{}': memory limit exceeded (RSS={} bytes)",
                            name_, usage.rss_bytes);
                }
            }
        }

        Drain();
    }

    void HandleData(DataBatchPtr batch) {
        records_processed_.fetch_add(batch->Size(), std::memory_order_relaxed);

        for (auto& proc : processors_) {
            auto result = proc->Process(std::move(batch));
            if (!result.ok()) {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            batch = std::move(result.value());
            if (!batch || batch->Empty()) return;
        }

        if (aggregator_) {
            aggregator_->Add(std::move(batch));
        } else {
            SubmitToSinks(std::move(batch));
        }

        batches_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    void HandleFlush() {
        if (!aggregator_) return;

        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                SubmitToSinks(std::move(batch));
            }
        }
    }

    void Drain() {
        while (auto item = ingest_channel_.TryDequeue()) {
            std::visit(Overloaded{
                [this](DataBatchPtr& batch) {
                    HandleData(std::move(batch));
                },
                [this](FlushSentinel&) {
                    HandleFlush();
                },
            }, *item);
        }
        if (aggregator_) HandleFlush();
    }

    void SubmitToSinks(DataBatchPtr batch) {
        if (!sink_pool_) {
            for (auto& sink : sinks_) {
                auto status = sink->Write(batch);
                if (!status.ok()) {
                    IL_WARN("Sink write error in pipeline '{}': {}",
                            name_, status.message());
                    error_count_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        // Back-pressure: drop batch when SinkPool is overloaded
        static constexpr size_t kMaxPendingTasks = 256;
        if (sink_pool_->PendingTasks() > kMaxPendingTasks) {
            IL_WARN("Pipeline '{}': SinkPool overloaded ({} pending), dropping batch",
                    name_, sink_pool_->PendingTasks());
            error_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        for (auto& sink : sinks_) {
            sink_pool_->Submit(
                [sink_ptr = sink.get(), batch, this]() -> void {
                    auto status = sink_ptr->Write(batch);
                    if (!status.ok()) {
                        IL_WARN("Sink write error in pipeline '{}': {}",
                                name_, status.message());
                        error_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            );
        }
    }

    void SyncChannelMetrics() {
        auto& m = InternalMetrics::Instance();
        const auto& s = ingest_channel_.stats();
        std::string prefix = "pipeline_" + name_ + "_channel_";
        m.SetGauge(prefix + "size", static_cast<double>(ingest_channel_.SizeApprox()));
        m.SetGauge(prefix + "enqueued", static_cast<double>(
            s.enqueued.load(std::memory_order_relaxed)));
        m.SetGauge(prefix + "dequeued", static_cast<double>(
            s.dequeued.load(std::memory_order_relaxed)));
        m.SetGauge(prefix + "dropped", static_cast<double>(
            s.dropped.load(std::memory_order_relaxed)));
        m.SetGauge(prefix + "flush_injected", static_cast<double>(
            s.flush_injected.load(std::memory_order_relaxed)));
        m.SetGauge(prefix + "backpressure_events", static_cast<double>(
            s.backpressure_events.load(std::memory_order_relaxed)));
        m.SetGauge(prefix + "utilization",
            ingest_channel_.capacity() > 0
            ? static_cast<double>(ingest_channel_.SizeApprox()) / ingest_channel_.capacity()
            : 0.0);
    }

    std::string name_;
    std::atomic<bool> running_{false};

    std::unique_ptr<SourcePlugin> source_;
    std::vector<std::unique_ptr<ProcessorPlugin>> processors_;
    std::unique_ptr<AggregatorPlugin> aggregator_;
    std::vector<std::unique_ptr<SinkPlugin>> sinks_;

public:
    const std::vector<std::unique_ptr<SinkPlugin>>& GetSinks() const {
        return sinks_;
    }

private:
    AsyncChannel ingest_channel_;
    ThreadPool* sink_pool_ = nullptr;

    std::thread process_thread_;

    std::atomic<bool> last_backpressure_state_{false};

    std::atomic<uint64_t> batches_processed_{0};
    std::atomic<uint64_t> records_processed_{0};
    std::atomic<uint64_t> error_count_{0};
};

// ============================================================================
// PipelineController — 多管道管理器 + TimerWheel + CollectPool + SinkPool
// ============================================================================
class PipelineController {
public:
    Status BuildFromConfig(const GlobalConfig& config);

    Status StartAll();
    Status StopAll();

    Pipeline* GetPipeline(const std::string& name);
    const std::vector<std::unique_ptr<Pipeline>>& Pipelines() const {
        return pipelines_;
    }

    void AddPipeline(std::unique_ptr<Pipeline> pipeline) {
        pipelines_.push_back(std::move(pipeline));
    }

    void InitSinkPool(size_t num_threads = 0) {
        if (num_threads == 0) {
            num_threads = std::max(2u, std::thread::hardware_concurrency() / 2);
        }
        sink_pool_ = std::make_unique<ThreadPool>(num_threads, "sink-write");
        for (auto& p : pipelines_) {
            p->SetSinkPool(sink_pool_.get());
        }
        IL_INFO("SinkPool initialized ({} threads)", num_threads);
    }

    void InitCollectPool(size_t num_threads = 0) {
        if (num_threads == 0) num_threads = 2;
        collect_pool_ = std::make_unique<ThreadPool>(num_threads, "collecter");
        IL_INFO("CollectPool initialized ({} threads)", num_threads);
    }

    TimerWheel& GetTimerWheel() { return timer_; }

    ThreadPool* GetSinkPool() { return sink_pool_.get(); }
    ThreadPool* GetCollectPool() { return collect_pool_.get(); }

    void SetStorageBackend(StorageBackend* backend) {
        storage_backend_ = backend;
    }

    StorageBackend* GetStorageBackend() const {
        return storage_backend_;
    }

private:
    StorageBackend* storage_backend_ = nullptr;
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    std::unique_ptr<ThreadPool> sink_pool_;
    std::unique_ptr<ThreadPool> collect_pool_;
    TimerWheel timer_;
};

}  // namespace illuminator
