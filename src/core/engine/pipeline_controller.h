// ============================================================================
// Illuminator 流水线引擎 — Pipeline 和 PipelineController
// ============================================================================
//
// 异步三段式管道架构：
//
//   Source ──→ AsyncChannel (LockFreeQueue) ──→ ProcessThread ──→ ThreadPool (Sinks)
//
// 一、Pipeline 类 — 单条数据处理管道
// ======================================
// 每条 Pipeline 遵循固定的数据流向：
//   Source → [AsyncChannel] → Processor₁ → ... → (Aggregator) → Sink₁, Sink₂, ...
//
// 异步解耦设计：
// --------
// 1. Source 产生数据后通过 AsyncChannel 无锁入队（纳秒级 CAS）
// 2. 独立的 process_thread_ 从 Channel 消费，运行 Processor 链
// 3. 多 Sink 通过共享 ThreadPool 并行写入
// 4. 水位线反压防止内存无限增长，Source 可感知背压状态
//
// 二、PipelineController 类 — 多管道管理器
// ============================================
// 管理全局共享的 Sink ThreadPool 和所有 Pipeline 实例。
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
#include "core/threading/thread_pool.h"
#include "core/threading/thread_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

// ============================================================================
// Pipeline — 单条异步数据处理管道
// ============================================================================
class Pipeline {
public:
    explicit Pipeline(const std::string& name) : name_(name) {}
    ~Pipeline() { Stop(); }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    const std::string& name() const { return name_; }

    // ---- 配置阶段 ----

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

    // ---- 运行阶段 ----

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

        // 启动 ProcessLoop 消费线程（所有模式都需要）
        process_thread_ = std::thread([this] {
            SetThreadName("il-" + name_.substr(0, 10) + "-p");
            ProcessLoop();
        });

        if (source_->IsPushMode()) {
            source_->SetCallback([this](DataBatchPtr batch) {
                OnBatchReceived(std::move(batch));
            });
        } else {
            collect_thread_ = std::thread([this] {
                SetThreadName("il-" + name_.substr(0, 10) + "-c");
                CollectLoop();
            });
        }

        if (aggregator_) {
            flush_thread_ = std::thread([this] {
                SetThreadName("il-" + name_.substr(0, 10) + "-f");
                FlushLoop();
            });
        }

        IL_INFO("Pipeline '{}' started (async, channel capacity={})",
                name_, ingest_channel_.capacity());
        return Status::Ok();
    }

    Status Stop() {
        if (!running_.exchange(false)) return Status::Ok();

        // 1. 停止 Source，防止新数据产生
        source_->Stop();

        // 2. 等待采集线程退出（Pull 模式）
        if (collect_thread_.joinable()) collect_thread_.join();

        // 3. 等待处理线程退出（会 drain 残余数据）
        if (process_thread_.joinable()) process_thread_.join();

        // 4. 等待聚合器刷新线程退出
        if (flush_thread_.joinable()) flush_thread_.join();

        // 5. 关闭各级插件
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

    // ---- 运行统计 ----

    uint64_t BatchesProcessed() const { return batches_processed_.load(); }
    uint64_t RecordsProcessed() const { return records_processed_.load(); }
    uint64_t ErrorCount() const { return error_count_.load(); }

    // ---- Channel 统计（供 API 暴露） ----

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

    // ---- 手动运行处理器链（供 HTTP API 直接调用） ----
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
    // ---- 数据入队（Source 回调或 CollectLoop 调用） ----
    // 无锁 CAS 入队，纳秒级延迟。
    // 同时检测反压状态并通知 Source。
    void OnBatchReceived(DataBatchPtr batch) {
        if (!batch || batch->Empty()) return;

        if (!ingest_channel_.TryEnqueue(std::move(batch))) {
            IL_WARN("Pipeline '{}': channel full, data dropped", name_);
        }

        // 反压通知：状态变化时通知 Source
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

    // ---- Pull 模式采集循环 ----
    void CollectLoop() {
        while (running_.load(std::memory_order_acquire)) {
            auto result = source_->Collect();
            if (result.ok()) {
                OnBatchReceived(std::move(result.value()));
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(source_->IntervalMs()));
        }
    }

    // ---- 异步处理循环（独立线程） ----
    // 从 AsyncChannel 消费数据，运行 Processor 链，分发到 Sinks。
    // 每 100 次循环检查一次 ResourceLimiter。
    void ProcessLoop() {
        uint32_t loop_count = 0;
        while (running_.load(std::memory_order_acquire)) {
            auto batch = ingest_channel_.Dequeue(std::chrono::milliseconds(100));
            if (batch) {
                ProcessAndDeliver(std::move(*batch));
            }

            if (++loop_count % 100 == 0) {
                auto usage = ResourceLimiter::Instance().Check();
                if (usage.memory_exceeded) {
                    IL_WARN("Pipeline '{}': memory limit exceeded (RSS={} bytes)",
                            name_, usage.rss_bytes);
                }
            }
        }
        // 优雅停机：drain channel 中的残余数据
        while (auto batch = ingest_channel_.TryDequeue()) {
            ProcessAndDeliver(std::move(*batch));
        }
    }

    // ---- 处理 + 分发（单线程，无需锁） ----
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

    // ---- Aggregator 刷新循环 ----
    void FlushLoop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(aggregator_->FlushIntervalMs()));
            auto result = aggregator_->Flush();
            if (result.ok()) {
                for (auto& batch : result.value()) {
                    DeliverToSinks(std::move(batch));
                }
            }
        }
        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                DeliverToSinks(std::move(batch));
            }
        }
    }

    // ---- 分发数据到所有 Sink ----
    // 单 Sink 走快路径；多 Sink 通过 ThreadPool 并行写入。
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
            // 无线程池时回退串行
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

        // 多 Sink 并行写入
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

    // ============ 成员变量 ============

    std::string name_;
    std::atomic<bool> running_{false};

    std::unique_ptr<SourcePlugin> source_;
    std::vector<std::unique_ptr<ProcessorPlugin>> processors_;
    std::unique_ptr<AggregatorPlugin> aggregator_;
    std::vector<std::unique_ptr<SinkPlugin>> sinks_;

    AsyncChannel<4096> ingest_channel_;   // Source → ProcessThread 异步通道
    ThreadPool* sink_pool_ = nullptr;     // 共享 Sink 线程池（PipelineController 拥有）

    std::thread collect_thread_;    // Pull 模式采集线程
    std::thread process_thread_;    // 异步处理线程
    std::thread flush_thread_;      // Aggregator 刷新线程

    bool last_backpressure_state_ = false;

    std::atomic<uint64_t> batches_processed_{0};
    std::atomic<uint64_t> records_processed_{0};
    std::atomic<uint64_t> error_count_{0};
};

// ============================================================================
// PipelineController — 多管道管理器
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

    // 创建共享 Sink 线程池并关联到所有管道
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

private:
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    std::unique_ptr<ThreadPool> sink_pool_;
};

}  // namespace illuminator
