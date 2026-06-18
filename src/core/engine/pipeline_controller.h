// ============================================================================
// Illuminator Pipeline v3 — 事件驱动异步管道引擎
// ============================================================================
//
// 本文件是 Illuminator v3 架构的核心，定义了 Pipeline（单管道）和
// PipelineController（多管道管理器）两个关键类。
//
// 架构概览：
// ==========
//   TimerWheel (1 thread)  — 全局定时调度，触发 Collect 和 Flush 事件
//   CollectPool (M threads) — 并行执行 Source::Collect()
//   ProcessThread (1 per pipeline) — 纯事件处理器 (variant dispatch)
//   SinkPool (K threads)   — 并行执行 Sink::Write()
//
// 数据流：
// ========
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
//
// 生命周期：
// ==========
//   构造 → SetSource/AddProcessor/SetAggregator/AddSink → SetSinkPool
//   → Start() → [运行] → Stop() → 析构
//
// 设计原则：
// ==========
//   1. 调度与执行分离 — TimerWheel 只调度，ProcessThread 只执行
//   2. 单一职责线程 — 每个线程只做一件事，职责清晰
//   3. 零锁处理路径 — Processor 和 Aggregator 由独占线程串行访问
//   4. I/O 隔离 — 所有 I/O 在池化线程中执行，与数据处理解耦
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
//
// Pipeline 是数据处理的最小单元，包含一条完整的处理链路：
//   Source → Processor[] → Aggregator(可选) → Sink[]
//
// 关键设计：
//   - 每个 Pipeline 拥有独立的 AsyncChannel 和 ProcessThread
//   - 管道之间完全隔离，一个管道阻塞不影响其他管道
//   - 支持 Pull（定时轮询）和 Push（eBPF 回调）两种数据源模式
//   - SinkPool 由 PipelineController 管理，Pipeline 只持有裸指针
class Pipeline {
public:
    // ---- 构造 ----
    // name:            管道名称（用于日志和指标，如 "cpu_utilization"）
    // channel_capacity: 内部通道容量（默认 4096）
    // drop_policy:      队列满时的丢包策略
    // bp_high/bp_low:   反压水位线比例
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
        sinks_.push_back(std::move(sink));
    }

    // 设置共享的 Sink 写入线程池（由 PipelineController 调用）
    void SetSinkPool(ThreadPool* pool) { sink_pool_ = pool; }

    // ====================================================================
    // Start — 启动管线
    // ====================================================================
    // 生命周期顺序：Source → Processor → Aggregator → Sink
    // 然后启动 ProcessThread，最后设置 Push Source 的回调。
    //
    // Push Source 的回调在 Start 之后设置（而不是 Init 阶段），
    // 确保 ProcessThread 已经准备好消费数据，避免回调推入的数据被丢弃。
    Status Start() {
        if (!source_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no source: " + name_);
        }
        if (sinks_.empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no sinks: " + name_);
        }

        // 按顺序启动所有插件
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

        // 设置 running_ 为 true（使用 release 语义确保所有初始化对其他线程可见）
        running_.store(true, std::memory_order_release);

        // 启动 ProcessThread（线程名截断到 15 字符，Linux pthread 限制）
        process_thread_ = std::thread([this] {
            SetThreadName(name_.substr(0, 15));
            ProcessLoop();
        });

        // Push 模式：设置回调，数据源通过此回调将数据推入 channel
        if (source_->IsPushMode()) {
            source_->SetCallback([this](DataBatchPtr batch) {
                Enqueue(std::move(batch));
            });
        }

        IL_INFO("Pipeline '{}' started (v3 event-driven, channel capacity={})",
                name_, ingest_channel_.capacity());
        return Status::Ok();
    }

    // ====================================================================
    // Stop — 优雅停止管线
    // ====================================================================
    // 停止顺序：
    //   1. Source::Stop() — 停止数据采集（Pull 不再触发，Push 取消回调）
    //   2. 等待 ProcessThread 退出（排空 channel 后 join）
    //   3. Processor/Aggregator/Sink Stop() — 依次停止后续阶段
    //   4. Sink::Flush() — 刷出 Sink 内部缓冲
    //
    // 使用 exchange 保证 running_ 只被设置一次（防止重复 Stop）
    Status Stop() {
        if (!running_.exchange(false)) return Status::Ok();

        source_->Stop();

        // 等待 ProcessThread 退出（内部会调用 Drain 排空 channel）
        if (process_thread_.joinable()) process_thread_.join();

        for (auto& p : processors_) p->Stop();
        if (aggregator_) aggregator_->Stop();
        for (auto& s : sinks_) {
            s->Flush();   // 先刷出 Sink 内部缓冲
            s->Stop();    // 再停止
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

    // 反压信号传递链：Enqueue → 检测 backpressure 状态变化 → source_->OnBackpressure(bp)
    // → Source 降低/恢复采集频率
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

    // ---- InjectFlush — 注入刷盘信号（由 TimerWheel 回调调用） ----
    void InjectFlush() {
        if (!ingest_channel_.InjectFlush()) {
            IL_WARN("Pipeline '{}': failed to inject FlushSentinel", name_);
        }
    }

    bool HasAggregator() const { return aggregator_ != nullptr; }
    uint32_t FlushIntervalMs() const {
        return aggregator_ ? aggregator_->FlushIntervalMs() : 0;
    }

    // ---- 统计查询（全部 atomic 读取，线程安全） ----
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

    // ---- RunProcessors — 执行 Processor 链（也被外部测试使用） ----
    // Processor 链是串行同步的：上一个 Processor 的输出是下一个的输入。
    // 如果某个 Processor 返回空批次，链终止（后续 Processor 不会执行）。
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

            // 每 100 次成功处理循环：同步指标 + 检查资源限制
            if (++loop_count % 100 == 0) {
                SyncChannelMetrics();
                auto usage = ResourceLimiter::Instance().Check();
                if (usage.memory_exceeded) {
                    IL_WARN("Pipeline '{}': memory limit exceeded (RSS={} bytes)",
                            name_, usage.rss_bytes);
                }
            }
        }

        Drain();  // 退出前排空 channel 中所有剩余数据
    }

    // ====================================================================
    // HandleData — 处理数据批次
    // ====================================================================
    // 处理流程：
    //   1. 统计处理记录数
    //   2. 串行执行 Processor 链（任意 Processor 失败则终止）
    //   3. 有 Aggregator → 累积数据；无 Aggregator → 直接提交到 Sink
    void HandleData(DataBatchPtr batch) {
        records_processed_.fetch_add(batch->Size(), std::memory_order_relaxed);

        for (auto& proc : processors_) {
            auto result = proc->Process(std::move(batch));
            if (!result.ok()) {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                return;  // Processor 失败，丢弃该批次
            }
            batch = std::move(result.value());
            if (!batch || batch->Empty()) return;  // 空批次，终止处理
        }

        // 有 Aggregator 则累积（等待 FlushSentinel 触发刷出），
        // 无 Aggregator 则直接提交到 Sink（实时输出）
        if (aggregator_) {
            aggregator_->Add(std::move(batch));
        } else {
            SubmitToSinks(std::move(batch));
        }

        batches_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    // ====================================================================
    // HandleFlush — 处理刷盘信号
    // ====================================================================
    // 收到 FlushSentinel 后，调用 Aggregator::Flush() 将累积的数据
    // 通过 swap 操作快速取出，然后提交到 SinkPool 写入。
    // Flush 是 swap 操作（微秒级），不会阻塞 ProcessThread。
    void HandleFlush() {
        if (!aggregator_) return;

        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                SubmitToSinks(std::move(batch));
            }
        }
    }

    // ====================================================================
    // Drain — 优雅停机：排空 channel 中所有剩余数据
    // ====================================================================
    // 在 ProcessLoop 退出前调用，确保 channel 中没有任何遗留数据。
    // 使用 TryDequeue（非阻塞）快速排空，最后再 flush 一次 Aggregator。
    //
    // 为什么最后还要 flush 一次？
    //   因为 Drain 过程中 HandleData 可能又在 Aggregator 中累积了数据，
    //   最后一次 flush 确保所有数据都被刷出。
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

    // ====================================================================
    // SubmitToSinks — 提交数据到 SinkPool
    // ====================================================================
    // 有两种模式：
    //   1. 无 SinkPool（sink_pool_ == nullptr）：直接在当前线程同步写入
    //      （仅单 Sink 场景可接受，因为会阻塞 ProcessThread）
    //   2. 有 SinkPool：提交到共享线程池异步写入
    //      （推荐，I/O 隔离，不会阻塞 ProcessThread）
    //
    // SinkPool 过载保护：
    //   当 SinkPool 中待处理任务超过 256 时，直接丢弃数据。
    //   这防止 SinkPool 无限制积压导致内存耗尽。
    void SubmitToSinks(DataBatchPtr batch) {
        if (!sink_pool_) {
            // 无池回退：直接在当前线程同步写入
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

        // 提交到 SinkPool 异步写入
        // 注意：lambda 按值捕获 batch（shared_ptr），确保数据在异步写入期间存活
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

    // ====================================================================
    // SyncChannelMetrics — 同步 Channel 指标到 InternalMetrics
    // ====================================================================
    // 每 100 次循环调用一次，将 AsyncChannel 的统计信息写入全局指标系统。
    // 指标前缀：pipeline_<name>_channel_*
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

    // ---- 成员变量 ----

    // 管道标识
    std::string name_;                          // 管道名称（如 "cpu_utilization"）
    std::atomic<bool> running_{false};          // 运行状态标志

    // 插件链（拥有所有权）
    std::unique_ptr<SourcePlugin> source_;      // 数据源（必须）
    std::vector<std::unique_ptr<ProcessorPlugin>> processors_;  // 处理器链（可选）
    std::unique_ptr<AggregatorPlugin> aggregator_;  // 聚合器（可选）
    std::vector<std::unique_ptr<SinkPlugin>> sinks_;  // 数据出口（至少一个）

public:
    // 获取 Sink 列表（供外部遍历，如 FeatureManager 注入 RecordingSink 时查找）
    const std::vector<std::unique_ptr<SinkPlugin>>& GetSinks() const {
        return sinks_;
    }

private:
    // 通信与线程
    AsyncChannel ingest_channel_;          // 异步有界通道（生产者-消费者桥梁）
    ThreadPool* sink_pool_ = nullptr;      // 共享 Sink 写入池（不拥有所有权）

    std::thread process_thread_;           // ProcessThread 线程

    // 反压状态
    std::atomic<bool> last_backpressure_state_{false};  // 上次反压状态（用于检测变化）

    // 统计计数器（atomic，多线程安全）
    std::atomic<uint64_t> batches_processed_{0};   // 已处理的数据批次数
    std::atomic<uint64_t> records_processed_{0};   // 已处理的记录数（批次大小累加）
    std::atomic<uint64_t> error_count_{0};         // 错误计数
};

// ============================================================================
// PipelineController — 多管道管理器 + 共享基础设施
// ============================================================================
//
// PipelineController 是引擎的顶层入口，管理所有 Pipeline 和共享基础设施：
//   - TimerWheel: 全局定时调度器（1 个线程）
//   - CollectPool: 采集线程池（M 个线程，所有管道共享）
//   - SinkPool: 写入线程池（K 个线程，所有管道共享）
//   - Pipelines[]: 所有管道实例
//
// 职责：
//   1. 从配置构建所有 Pipeline（BuildFromConfig）
//   2. 管理共享基础设施的初始化（线程池、TimerWheel）
//   3. 注册定时事件到 TimerWheel（Pull Source 采集、Aggregator 刷盘、指标同步）
//   4. 协调启动和停止顺序（StartAll / StopAll）
//   5. 提供 Pipeline 的查询接口
class PipelineController {
public:
    // BuildFromConfig — 从 GlobalConfig 构建所有 Pipeline
    // 实现见 pipeline_controller.cc
    Status BuildFromConfig(const GlobalConfig& config);

    // StartAll — 启动所有 Pipeline 和共享基础设施
    // 启动顺序：
    //   1. 启动所有 Pipeline（各自启动 Source/Processor/Aggregator/Sink）
    //   2. 含有 BPF 探针的管道间添加 100ms 延迟（避免内核过载）
    //   3. 注册 Pull Source 采集事件到 TimerWheel
    //   4. 注册 Aggregator 刷盘事件到 TimerWheel
    //   5. 注册定期指标同步到 TimerWheel
    //   6. 如果有存储后端，注册定期数据清理到 TimerWheel
    //   7. 启动 TimerWheel 调度线程
    Status StartAll();

    // StopAll — 停止所有 Pipeline 和共享基础设施
    // 停止顺序（与启动顺序相反）：
    //   1. TimerWheel::Stop() — 停止所有定时器
    //   2. CollectPool 销毁 — 等待正在执行的 Collect 完成
    //   3. 逐个 Pipeline::Stop() — 排空 channel + 最后 flush
    //   4. SinkPool 销毁（析构函数中）— 等待所有 Write 完成
    Status StopAll();

    Pipeline* GetPipeline(const std::string& name);
    const std::vector<std::unique_ptr<Pipeline>>& Pipelines() const {
        return pipelines_;
    }

    // ---- 动态添加 Pipeline（供 FeatureManager 使用） ----
    void AddPipeline(std::unique_ptr<Pipeline> pipeline) {
        pipelines_.push_back(std::move(pipeline));
    }

    // ---- 共享基础设施初始化 ----
    // SinkPool：写入线程池，num_threads=0 时自动设为 CPU 核数/2（至少 2）
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

    // CollectPool：采集线程池，num_threads=0 时默认 2 线程
    void InitCollectPool(size_t num_threads = 0) {
        if (num_threads == 0) num_threads = 2;
        collect_pool_ = std::make_unique<ThreadPool>(num_threads, "collecter");
        IL_INFO("CollectPool initialized ({} threads)", num_threads);
    }

    // ---- 基础设施访问器（供 FeatureManager 使用） ----
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
