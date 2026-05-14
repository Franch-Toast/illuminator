// ============================================================================
// Illuminator 流水线引擎 — Pipeline / PullScheduler / PipelineController
// ============================================================================
//
// 分层混合线程模型：
//
//   Layer 0 - Ingestion:
//     Push Sources: eBPF 回调直接无锁入队
//     Pull Sources: 全局 PullScheduler (1 thread) 统一调度所有 Pull 间隔
//
//   Layer 1 - Processing:
//     每管道 1 个 process_thread (从 channel 消费 -> Processor 链 -> 分发)
//     兼任 Aggregator flush 定时 (消除独立 flush_thread)
//
//   Layer 2 - Delivery:
//     共享 SinkPool (ThreadPool) 并行写多个 Sink
//     单 Sink 快路径: 直接在 process_thread 中写
//
// 线程计数 (N 条管道):
//   1 scheduler + N process_threads + M sink_workers
//   典型 10 管道 = 1 + 10 + 4 = 15 线程 (对比原 30+ 线程)
// ============================================================================

#pragma once

#include <algorithm>
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
#include "core/threading/thread_pool.h"
#include "core/threading/thread_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

class PipelineController;

// ============================================================================
// Pipeline - single async data processing pipeline
// ============================================================================
class Pipeline {
public:
    explicit Pipeline(const std::string& name) : name_(name) {}
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
            SetThreadName("il-" + name_.substr(0, 10) + "-p");
            ProcessLoop();
        });

        if (source_->IsPushMode()) {
            source_->SetCallback([this](DataBatchPtr batch) {
                Enqueue(std::move(batch));
            });
        }

        IL_INFO("Pipeline '{}' started (async, channel capacity={})",
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
        IL_INFO("Pipeline '{}' stopped (enqueued={}, dequeued={}, dropped={})",
                name_,
                ch.enqueued.load(std::memory_order_relaxed),
                ch.dequeued.load(std::memory_order_relaxed),
                ch.dropped.load(std::memory_order_relaxed));
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
    // Unified processing loop: channel consume + Processor chain + Sink dispatch
    // + Aggregator flush timer (eliminates dedicated flush_thread_)
    void ProcessLoop() {
        using Clock = std::chrono::steady_clock;
        uint32_t loop_count = 0;

        auto next_flush = aggregator_
            ? Clock::now() + std::chrono::milliseconds(aggregator_->FlushIntervalMs())
            : Clock::time_point::max();

        while (running_.load(std::memory_order_acquire)) {
            auto now = Clock::now();
            auto wait = std::chrono::milliseconds(100);

            if (aggregator_) {
                auto until_flush = std::chrono::duration_cast<
                    std::chrono::milliseconds>(next_flush - now);
                if (until_flush.count() <= 0) {
                    FlushAggregator();
                    next_flush = Clock::now() +
                        std::chrono::milliseconds(aggregator_->FlushIntervalMs());
                    until_flush = std::chrono::duration_cast<
                        std::chrono::milliseconds>(next_flush - Clock::now());
                }
                if (until_flush < wait && until_flush.count() > 0) {
                    wait = std::chrono::milliseconds(until_flush.count());
                }
            }

            auto batch = ingest_channel_.Dequeue(wait);
            if (batch) {
                ProcessAndDeliver(std::move(*batch));
            }

            if (++loop_count % 100 == 0) {
                SyncChannelMetrics();
                auto usage = ResourceLimiter::Instance().Check();
                if (usage.memory_exceeded) {
                    IL_WARN("Pipeline '{}': memory limit exceeded (RSS={} bytes)",
                            name_, usage.rss_bytes);
                }
            }
        }

        // Graceful shutdown: drain remaining batches
        while (auto batch = ingest_channel_.TryDequeue()) {
            ProcessAndDeliver(std::move(*batch));
        }
        if (aggregator_) {
            FlushAggregator();
        }
    }

    void ProcessAndDeliver(DataBatchPtr batch) {
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
            DeliverToSinks(std::move(batch));
        }

        batches_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    void FlushAggregator() {
        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                DeliverToSinks(std::move(batch));
            }
        }
    }

    void DeliverToSinks(DataBatchPtr batch) {
        if (sinks_.size() == 1) {
            auto status = sinks_[0]->Write(batch);
            if (!status.ok()) {
                IL_WARN("Sink write error in pipeline '{}': {}",
                        name_, status.message());
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        if (!sink_pool_ || sinks_.size() <= 1) {
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

        std::vector<std::future<Status>> futures;
        futures.reserve(sinks_.size());

        for (auto& sink : sinks_) {
            futures.push_back(sink_pool_->Submit(
                [&sink, batch]() -> Status {
                    return sink->Write(batch);
                }
            ));
        }

        for (auto& f : futures) {
            auto status = f.get();
            if (!status.ok()) {
                IL_WARN("Sink write error in pipeline '{}': {}",
                        name_, status.message());
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
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

    AsyncChannel<4096> ingest_channel_;
    ThreadPool* sink_pool_ = nullptr;

    std::thread process_thread_;

    bool last_backpressure_state_ = false;

    std::atomic<uint64_t> batches_processed_{0};
    std::atomic<uint64_t> records_processed_{0};
    std::atomic<uint64_t> error_count_{0};
};

// ============================================================================
// PullScheduler - unified scheduler for all Pull-mode Sources
// ============================================================================
// Replaces N collect_threads with 1 scheduler thread that calls
// Source::Collect() at each source's configured interval.
class PullScheduler {
public:
    struct Entry {
        Pipeline* pipeline;
        SourcePlugin* source;
        uint32_t interval_ms;
        std::chrono::steady_clock::time_point next_fire;
    };

    void Register(Pipeline* pipeline, SourcePlugin* source) {
        entries_.push_back({
            pipeline, source, source->IntervalMs(),
            std::chrono::steady_clock::now()
        });
    }

    void Start() {
        if (entries_.empty()) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] {
            SetThreadName("il-scheduler");
            Run();
        });
        IL_INFO("PullScheduler started ({} sources)", entries_.size());
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        IL_INFO("PullScheduler stopped");
    }

    size_t EntryCount() const { return entries_.size(); }

private:
    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            auto next_wake = now + std::chrono::milliseconds(500);

            for (auto& entry : entries_) {
                if (!entry.pipeline->IsRunning()) continue;

                if (now >= entry.next_fire) {
                    auto result = entry.source->Collect();
                    if (result.ok()) {
                        entry.pipeline->Enqueue(std::move(*result));
                    }
                    entry.next_fire = now +
                        std::chrono::milliseconds(entry.interval_ms);
                }
                if (entry.next_fire < next_wake) {
                    next_wake = entry.next_fire;
                }
            }

            auto sleep_dur = next_wake - std::chrono::steady_clock::now();
            if (sleep_dur.count() > 0) {
                std::this_thread::sleep_for(sleep_dur);
            }
        }
    }

    std::vector<Entry> entries_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ============================================================================
// PipelineController - multi-pipeline manager
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

    void InitSinkPool(size_t num_threads = 0) {
        if (num_threads == 0) {
            num_threads = std::max(2u, std::thread::hardware_concurrency() / 2);
        }
        sink_pool_ = std::make_unique<ThreadPool>(num_threads, "il-sink");
        for (auto& p : pipelines_) {
            p->SetSinkPool(sink_pool_.get());
        }
        IL_INFO("Sink thread pool initialized with {} threads", num_threads);
    }

    PullScheduler& GetScheduler() { return scheduler_; }

private:
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    std::unique_ptr<ThreadPool> sink_pool_;
    PullScheduler scheduler_;
};

}  // namespace illuminator
