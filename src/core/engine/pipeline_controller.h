// ============================================================================
// Illuminator 流水线引擎 — Pipeline 和 PipelineController
// ============================================================================
//
// 本文件定义了 Illuminator 的数据处理管道核心。
//
// 一、Pipeline 类 — 单条数据处理管道
// ======================================
// 每条 Pipeline 遵循固定的数据流向：
//   Source → Processor₁ → Processor₂ → ... → (Aggregator) → Sink₁, Sink₂, ...
//
// 各阶段职责：
// --------
// Source (数据源):
//   负责产生原始观测数据。支持两种工作模式：
//   - Pull 模式（拉取）: 流水线定期调用 Collect() 主动获取数据
//     适用于轮询 /proc、sysfs 等场景
//   - Push 模式（推送）: Source 通过回调函数异步推送数据到流水线
//     适用于 eBPF 事件驱动的场景（如 RingBuffer 回调）
//
// Processor (处理器):
//   对数据进行实时转换。多个 Processor 按配置顺序串联执行：
//   - 过滤: 丢弃不符合条件的记录
//   - 符号化: 将堆栈地址解析为函数名
//   - 堆栈合并: 合并相同的调用栈以降低数据量
//
// Aggregator (聚合器, 可选):
//   在时间窗口内缓冲数据，定期输出聚合结果：
//   - 适用于计算 avg/min/max/P50/P99 等统计量
//   - 也适用于合并多帧采样数据
//
// Sink (数据出口):
//   将处理后的数据写入目标位置：
//   - 本地 SQLite 存储
//   - 控制台输出
//   - Prometheus 指标暴露
//   - OTLP 导出
//   - pprof 格式导出
//   - WebSocket 推送
//
// 运行机制：
// --------
// 1. Source 在 Pull 模式下，collect_thread_ 定期调用 Collect() 获取数据
// 2. Source 在 Push 模式下，数据通过回调函数 OnBatchReceived() 流入
// 3. 数据依次经过所有 Processor
// 4. 如果有 Aggregator，数据进入聚合缓冲区；否则直接送给 Sinks
// 5. Aggregator 的 flush_thread_ 按 flush_interval 定期输出聚合结果
// 6. 最后一个 Sink 可以获取数据的所有权（shared_ptr），其他 Sink 共享引用
//
// 二、PipelineController 类 — 多管道管理器
// ============================================
// 根据 GlobalConfig 创建和管理多个 Pipeline 实例。
// - BuildFromConfig(): 解析全局配置，逐条构建所有管道
// - StartAll()/StopAll(): 统一启动/停止所有管道
// - GetPipeline(): 按名称查找管道实例
// ============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/data_batch.h"
#include "core/threading/thread_util.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

// ============================================================================
// Pipeline — 单条数据处理管道
// ============================================================================
class Pipeline {
public:
    explicit Pipeline(const std::string& name) : name_(name) {}
    ~Pipeline() { Stop(); }

    // 禁止拷贝：每个 Pipeline 实例拥有独立的线程和状态
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    const std::string& name() const { return name_; }

    // ---- 配置阶段方法 ----

    // 设置数据源（每条管道必须有且仅有一个 Source）
    void SetSource(std::unique_ptr<SourcePlugin> source) {
        source_ = std::move(source);
    }
    SourcePlugin* GetSource() { return source_.get(); }

    // 添加一个处理器（多个 Processor 按添加顺序执行）
    void AddProcessor(std::unique_ptr<ProcessorPlugin> proc) {
        processors_.push_back(std::move(proc));
    }

    // 设置聚合器（可选，每条管道最多一个）
    void SetAggregator(std::unique_ptr<AggregatorPlugin> agg) {
        aggregator_ = std::move(agg);
    }

    // 添加一个数据出口（至少需要一个 Sink）
    void AddSink(std::unique_ptr<SinkPlugin> sink) {
        sinks_.push_back(std::move(sink));
    }

    // ---- 运行阶段方法 ----

    // 启动管道：初始化并启动所有插件，启动采集/推送线程
    Status Start() {
        // 前置校验：必须有 Source 和至少一个 Sink
        if (!source_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no source: " + name_);
        }
        if (sinks_.empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no sinks: " + name_);
        }

        // 按顺序启动各级插件
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

        // E4: all components started successfully → set running flag
        running_.store(true, std::memory_order_release);

        // 根据 Source 的工作模式选择数据采集方式
        if (source_->IsPushMode()) {
            // Push 模式：Source 通过回调异步推送数据
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

        IL_INFO("Pipeline '{}' started", name_);
        return Status::Ok();
    }

    // 停止管道：发送停止信号，等待线程退出，关闭所有插件
    Status Stop() {
        // 使用 exchange 原子操作确保只执行一次停止
        if (!running_.exchange(false)) return Status::Ok();

        // 等待所有工作线程退出
        if (collect_thread_.joinable()) collect_thread_.join();
        if (flush_thread_.joinable()) flush_thread_.join();

        // 关闭各级插件（按启动的逆序）
        source_->Stop();
        for (auto& p : processors_) p->Stop();
        if (aggregator_) aggregator_->Stop();
        for (auto& s : sinks_) {
            s->Flush();   // 先刷出缓冲数据
            s->Stop();
        }

        IL_INFO("Pipeline '{}' stopped", name_);
        return Status::Ok();
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // ---- 运行统计 ----
    // 这些计数器使用原子操作，可安全地跨线程读取

    uint64_t BatchesProcessed() const { return batches_processed_.load(); }
    uint64_t RecordsProcessed() const { return records_processed_.load(); }
    uint64_t ErrorCount() const { return error_count_.load(); }

    // ---- 手动运行处理器链 ----
    // 供 HTTP API 等场景直接调用，不经过聚合器和 Sink
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
    // ---- Pull 模式采集循环 ----
    // 在独立线程中运行，按固定间隔调用 Source->Collect()
    void CollectLoop() {
        while (running_.load(std::memory_order_acquire)) {
            auto result = source_->Collect();
            if (result.ok()) {
                // 将采集到的数据送入处理器链
                OnBatchReceived(std::move(result.value()));
            }
            // 按 Source 指定的间隔休眠
            std::this_thread::sleep_for(
                std::chrono::milliseconds(source_->IntervalMs()));
        }
    }

    // ---- Aggregator 刷新循环 ----
    // 在独立线程中运行，按聚合器指定的间隔刷出数据
    void FlushLoop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(aggregator_->FlushIntervalMs()));
            auto result = aggregator_->Flush();
            if (result.ok()) {
                for (auto& batch : result.value()) {
                    DeliverToSinks(std::move(batch));  // 每个聚合结果批次送给 Sinks
                }
            }
        }
        // 停止前最后一次刷出：确保不丢失数据
        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                DeliverToSinks(std::move(batch));
            }
        }
    }

    // ---- 数据批次到达处理 ----
    // 这是管道数据流的核心入口，处理流程：
    //   1. 更新处理记录数统计
    //   2. 依次经过所有 Processor
    //   3. 如果有 Aggregator 则聚合，否则直接送达 Sinks
    void OnBatchReceived(DataBatchPtr batch) {
        if (!batch || batch->Empty()) return;

        std::lock_guard<std::mutex> lock(process_mutex_);
        records_processed_.fetch_add(batch->Size(), std::memory_order_relaxed);

        // ---- Processor 链处理 ----
        // 依次调用每个 Processor 的 Process() 方法
        // 每个 Processor 可以修改数据、产生新数据、过滤数据或返回错误
        for (auto& proc : processors_) {
            auto result = proc->Process(std::move(batch));
            if (!result.ok()) {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                return;  // 处理失败，丢弃这批数据
            }
            batch = std::move(result.value());
            if (!batch || batch->Empty()) return;  // 数据被完全过滤
        }

        // ---- 数据分发 ----
        if (aggregator_) {
            aggregator_->Add(std::move(batch));  // 送入聚合缓冲区
        } else {
            DeliverToSinks(std::move(batch));    // 直接送达所有 Sink
        }

        batches_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- 送达数据到所有 Sink ----
    // 每个 Sink 获得 DataBatch 的 shared_ptr 引用
    void DeliverToSinks(DataBatchPtr batch) {
        for (size_t i = 0; i < sinks_.size(); ++i) {
            auto status = sinks_[i]->Write(batch);
            if (!status.ok()) {
                IL_WARN("Sink write error in pipeline '{}': {}",
                        name_, status.message());
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // ============ 成员变量 ============

    std::string name_;                                    // 管道名称（唯一标识）
    std::atomic<bool> running_{false};                    // 运行状态标志

    std::unique_ptr<SourcePlugin> source_;                 // 数据源
    std::vector<std::unique_ptr<ProcessorPlugin>> processors_;  // 处理器链
    std::unique_ptr<AggregatorPlugin> aggregator_;         // 聚合器（可选）
    std::vector<std::unique_ptr<SinkPlugin>> sinks_;       // 数据出口列表

    std::thread collect_thread_;   // Pull 模式采集线程
    std::thread flush_thread_;     // Aggregator 刷新线程
    std::mutex process_mutex_;     // OnBatchReceived 并发保护

    // 统计计数器（原子类型，线程安全）
    std::atomic<uint64_t> batches_processed_{0};   // 已处理的批次数
    std::atomic<uint64_t> records_processed_{0};   // 已处理的记录数
    std::atomic<uint64_t> error_count_{0};         // 错误累计次数
};

// ============================================================================
// PipelineController — 多管道管理器
// ============================================================================
class PipelineController {
public:
    // ---- 从配置构建所有管道 ----
    // 遍历 GlobalConfig.pipelines，逐条创建 Pipeline 实例
    // 失败时返回错误（如找不到指定插件、初始化失败等）
    Status BuildFromConfig(const GlobalConfig& config);

    // ---- 统一生命周期管理 ----
    // 启动所有管道（如任一启动失败则停止已启动的管道）
    Status StartAll();
    // 停止所有管道
    Status StopAll();

    // ---- 按名称查找管道 ----
    // 返回 nullptr 表示未找到
    Pipeline* GetPipeline(const std::string& name);
    // 获取所有管道的只读列表
    const std::vector<std::unique_ptr<Pipeline>>& Pipelines() const {
        return pipelines_;
    }

private:
    std::vector<std::unique_ptr<Pipeline>> pipelines_;  // 所有管道实例
};

}  // namespace illuminator
