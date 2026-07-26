// ============================================================================
// Illuminator Pipeline v3 — 事件驱动异步管道引擎
// ============================================================================
//
// Pipeline 是数据处理的最小单元。每个 FeatureDriver 拥有一个 Pipeline。
//
// 架构概览：
//   TimerWheel (1 thread)  — 全局定时调度，触发 Collect 和 Flush 事件
//   CollectPool (M threads) — 并行执行 Source::Collect()
//   ProcessThread (1 per pipeline) — 纯事件处理器 (variant dispatch)
//   SinkPool (K threads)   — 并行执行 Sink::Write()
//
// 数据流：
//   TimerWheel → CollectPool → src.Collect()     ─┐→ AsyncChannel(ChannelItem)
//   TimerWheel → channel.InjectFlush()            ─┘        │
//                                                           ▼
//                                                    ProcessThread
//                                                    match event:
//                                                      DataBatch  → Process → Sink
//                                                      Sentinel   → Flush   → Sink
//
// 生命周期：
//   构造 → SetSource/AddProcessor/SetAggregator/AddSink → SetSinkPool
//   → Start() → [运行] → Stop() → 析构
//
// 设计原则：
//   1. 调度与执行分离 — TimerWheel 只调度，ProcessThread 只执行
//   2. 单一职责线程 — 每个线程只做一件事，职责清晰
//   3. 零锁处理路径 — Processor 和 Aggregator 由独占线程串行访问
//   4. I/O 隔离 — 所有 I/O 在池化线程中执行，与数据处理解耦
// ============================================================================

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/common/logging.h"
#include "core/engine/self_observability.h"
#include "core/common/status.h"
#include "core/engine/async_channel.h"
#include "core/common/data_batch.h"
#include "core/threading/thread_pool.h"
#include "core/threading/thread_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

inline size_t ResolveChannelCapacity(const std::string& size) {
    if (size == "small") return 1024;
    if (size == "large") return 16384;
    return 4096;
}

inline DropPolicy ResolveDropPolicy(const std::string& policy) {
    return policy == "drop_oldest" ? DropPolicy::kDropOldest
                                   : DropPolicy::kDropNewest;
}

// ============================================================================
// Pipeline — 单条异步数据处理管道
// ============================================================================
//
// Pipeline 是数据处理的最小单元，包含一条完整的处理链路：
//   Source → Processor[] → Aggregator(可选) → Sink[]
//
// 每个 Pipeline 拥有独立的 AsyncChannel 和 ProcessThread。
// 管道之间完全隔离，一个管道阻塞不影响其他管道。
// 所有数据源统一通过 TimerWheel 定时调用 Collect() 采集。
// SinkPool 由 FeatureDriver::Probe() 设置，Pipeline 通过 shared_ptr 持有。
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

    // ---- 管线组装（在 Start() 之前调用） ----
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
        if (!sink) return;
        sinks_.push_back(std::shared_ptr<SinkPlugin>(sink.release()));
    }

    // ====================================================================
    // AddSinkRuntime — 运行时动态添加 Sink
    // ====================================================================
    // 在 Pipeline 已经启动后安全地添加新的 Sink。方法内部会先调用 sink->Start()，
    // 然后在写锁保护下加入 sinks_ 列表。ProcessThread 在遍历 sinks_ 时持有读锁，
    // 因此本操作与数据处理并发安全。
    Status AddSinkRuntime(std::unique_ptr<SinkPlugin> sink) {
        if (!sink) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline AddSinkRuntime received null sink");
        }
        if (!running_.load(std::memory_order_acquire)) {
            return Status::Error(StatusCode::kUnavailable,
                                 "Pipeline is not running: " + name_);
        }
        auto status = sink->Start();
        if (!status.ok()) return status;

        std::unique_lock<std::shared_mutex> lock(sinks_mutex_);
        sinks_.push_back(std::shared_ptr<SinkPlugin>(sink.release()));
        return Status::Ok();
    }

    // ====================================================================
    // RemoveSinkRuntime — 运行时动态移除 Sink
    // ====================================================================
    // 按 Sink::Name() 查找并移除指定 Sink。持有写锁期间将 sink 从列表中取出，
    // 释放锁后再调用 Flush() + Stop()，避免在持有写锁时执行可能阻塞的 I/O。
    Status RemoveSinkRuntime(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(sinks_mutex_);
        auto it = std::find_if(sinks_.begin(), sinks_.end(),
                               [&name](const std::shared_ptr<SinkPlugin>& s) {
                                   return s && s->Name() == name;
                               });
        if (it == sinks_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Sink not found in pipeline: " + name);
        }
        auto sink = std::move(*it);
        sinks_.erase(it);
        lock.unlock();

        sink->Flush();
        sink->Stop();
        return Status::Ok();
    }

    void SetSinkPool(std::shared_ptr<ThreadPool> pool) { sink_pool_ = std::move(pool); }

    // ====================================================================
    // Start — 启动管线
    // ====================================================================
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
        last_flush_time_ = std::chrono::steady_clock::now();

        process_thread_ = std::thread([this] {
            SetThreadName(name_.substr(0, 15));
            ProcessLoop();
        });

        IL_INFO("Pipeline '{}' started (v3 event-driven, channel capacity={})",
                name_, ingest_channel_.capacity());
        return Status::Ok();
    }

    // ====================================================================
    // Stop — 优雅停止管线
    // ====================================================================
    Status Stop() {
        if (!running_.exchange(false)) return Status::Ok();

        source_->Stop();

        if (process_thread_.joinable()) process_thread_.join();

        for (auto& p : processors_) p->Stop();
        if (aggregator_) aggregator_->Stop();
        {
            std::shared_lock<std::shared_mutex> lock(sinks_mutex_);
            for (auto& s : sinks_) {
                s->Flush();
                s->Stop();
            }
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

    // ---- 统计查询 ----
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

    std::vector<std::shared_ptr<SinkPlugin>> GetSinks() const {
        std::shared_lock<std::shared_mutex> lock(sinks_mutex_);
        return sinks_;
    }

    // ========================================================================
    // Reconfigure — 运行时动态重配置，贯穿 Pipeline 全链路
    // ========================================================================
    // 依次调用 Source → Processors → Aggregator → Sinks 的 Reconfigure()。
    // 对于返回 kUnimplemented 的插件，跳过而不中断链路（向后兼容）。
    // 其他非 ok 状态会立即返回错误。
    Status Reconfigure(const ConfigValue& params) {
        bool requires_restart = false;
        if (source_) {
            auto s = source_->Reconfigure(params);
            if (!IsReconfigureContinueCode(s.code())) return s;
            if (s.code() == StatusCode::kRequiresRestart) requires_restart = true;
        }
        for (auto& p : processors_) {
            auto s = p->Reconfigure(params);
            if (!IsReconfigureContinueCode(s.code())) return s;
            if (s.code() == StatusCode::kRequiresRestart) requires_restart = true;
        }
        if (aggregator_) {
            auto s = aggregator_->Reconfigure(params);
            if (!IsReconfigureContinueCode(s.code())) return s;
            if (s.code() == StatusCode::kRequiresRestart) requires_restart = true;
        }
        {
            std::shared_lock<std::shared_mutex> lock(sinks_mutex_);
            for (auto& s : sinks_) {
                auto rc = s->Reconfigure(params);
                if (!IsReconfigureContinueCode(rc.code())) return rc;
                if (rc.code() == StatusCode::kRequiresRestart) requires_restart = true;
            }
        }
        if (requires_restart) {
            return Status(StatusCode::kRequiresRestart,
                          "some parameters require restart to take effect");
        }
        return Status::Ok();
    }

private:
    void ProcessLoop() {
        uint32_t loop_count = 0;

        while (running_.load(std::memory_order_acquire)) {
            MaybeForceFlush();

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
                    last_flush_time_ = std::chrono::steady_clock::now();
                },
            }, *item);

            if (++loop_count % 100 == 0) {
                SyncChannelMetrics();
                SyncSinkPoolMetrics();
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
        last_flush_time_ = std::chrono::steady_clock::now();
    }

    void MaybeForceFlush() {
        if (!aggregator_) return;

        const uint32_t interval_ms = aggregator_->FlushIntervalMs();
        if (interval_ms == 0) return;

        const auto now = std::chrono::steady_clock::now();
        const auto max_delay = std::chrono::milliseconds(interval_ms * 3);
        if (now - last_flush_time_ >= max_delay) {
            IL_WARN("Pipeline '{}': forced flush (no sentinel for {}ms)",
                    name_, interval_ms * 3);
            HandleFlush();
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
        ConstDataBatchPtr const_batch = batch;

        if (!sink_pool_) {
            std::shared_lock<std::shared_mutex> lock(sinks_mutex_);
            for (auto& sink : sinks_) {
                auto status = sink->Write(const_batch);
                if (!status.ok()) {
                    IL_WARN("Sink write error in pipeline '{}': {}",
                            name_, status.message());
                    error_count_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        static constexpr size_t kMaxPendingTasks = 256;
        static constexpr size_t kSinkPoolHighWatermark = 128;
        const size_t pending = sink_pool_->PendingTasks();

        if (pending > kSinkPoolHighWatermark) {
            InternalMetrics::Instance().SetGauge(
                "pipeline_" + name_ + "_sink_pool_backpressure", 1.0);
        } else {
            InternalMetrics::Instance().SetGauge(
                "pipeline_" + name_ + "_sink_pool_backpressure", 0.0);
        }

        if (pending > kMaxPendingTasks) {
            IL_WARN("Pipeline '{}': SinkPool overloaded ({} pending), dropping batch",
                    name_, pending);
            InternalMetrics::Instance().Inc("pipeline_" + name_ + "_sink_drops_total");
            error_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::shared_lock<std::shared_mutex> lock(sinks_mutex_);
        for (auto& sink : sinks_) {
            sink_pool_->Submit(
                [sink, const_batch, this]() -> void {
                    auto status = sink->Write(const_batch);
                    if (!status.ok()) {
                        IL_WARN("Sink write error in pipeline '{}': {}",
                                name_, status.message());
                        error_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            );
        }
    }

    void SyncSinkPoolMetrics() {
        if (!sink_pool_) return;
        auto& m = InternalMetrics::Instance();
        const size_t pending = sink_pool_->PendingTasks();
        m.SetGauge("pipeline_" + name_ + "_sink_pool_pending",
                   static_cast<double>(pending));
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
    std::vector<std::shared_ptr<SinkPlugin>> sinks_;
    mutable std::shared_mutex sinks_mutex_;

    AsyncChannel ingest_channel_;
    std::shared_ptr<ThreadPool> sink_pool_;

    std::thread process_thread_;

    std::chrono::steady_clock::time_point last_flush_time_{};

    std::atomic<bool> last_backpressure_state_{false};

    std::atomic<uint64_t> batches_processed_{0};
    std::atomic<uint64_t> records_processed_{0};
    std::atomic<uint64_t> error_count_{0};
};

}  // namespace illuminator
