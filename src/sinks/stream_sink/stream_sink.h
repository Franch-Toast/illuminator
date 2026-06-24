// =============================================================================
// StreamSink — 统一数据缓冲 Sink
// =============================================================================
//
// 【架构定位】
// StreamSink 是 Illuminator 数据分发架构的核心枢纽。它位于 Pipeline 的 Sink 层，
// 为每个 Feature 提供独立的环形数据缓冲区，同时服务于三个数据消费方：
//
//   ┌───────────────────────────────────────────────────────────────┐
//   │                     Pipeline 处理链路                          │
//   │  Source → Processor[] → Aggregator → Sink[]                  │
//   │                                        │                      │
//   │                    ┌───────────────────┼───────────────┐      │
//   │                    ▼                   ▼               ▼      │
//   │              UserSink           StreamSink       RecordingSink│
//   │              (用户配置)          (自动注入)        (自动注入)  │
//   │                    │                   │               │      │
//   └────────────────────┼───────────────────┼───────────────┼──────┘
//                        │                   │               │
//              ┌─────────▼───────────────────▼───────────────▼──────┐
//              │              StreamSinkStore (全局单例)             │
//              │  ┌──────────────────┐  ┌──────────────────┐       │
//              │  │ StreamBuffer     │  │ StreamBuffer     │  ...  │
//              │  │ (cpu_utilization)│  │ (cpu_profiler)   │       │
//              │  │ deque<DataBatch> │  │ deque<DataBatch> │       │
//              │  │ (max 60 bathes) │  │ (max 60 bathes) │       │
//              │  └────────┬─────────┘  └────────┬─────────┘       │
//              └───────────┼─────────────────────┼──────────────────┘
//                          │                     │
//          ┌───────────────┼─────────┐  ┌────────┼───────────────┐
//          │               ▼         │  │        ▼               │
//          │  HTTP API 拉取          │  │  WebSocket 实时广播     │
//          │  /api/v1/features/      │  │  WebSocketManager       │
//          │  :name/collect          │  │  BroadcastLoop()        │
//          │  :name/stream?cursor=N  │  │  → buf.Latest()         │
//          │  → buf.Recent(1)        │  │  → 序列化 → 推送所有    │
//          │  → buf.PollSince(N)     │  │    订阅客户端           │
//          └─────────────────────────┘  └─────────────────────────┘
//
// 【设计要点】
//   1. 每个 Feature 一个独立 StreamBuffer（60 batch 环形缓冲区）
//      - 默认保留最近 60 个 DataBatch（约 60 秒的数据，假设 1 秒采集间隔）
//      - 超出上限时自动淘汰最旧的数据（FIFO 策略）
//
//   2. 全局递增序号（seq_）
//      - 每个 Push 操作递增 seq_，用于实现增量拉取（cursor 机制）
//      - 客户端通过 cursor 参数告诉服务端"我已经消费到第 N 条"，
//        服务端只返回 N 之后的新数据，避免重复传输
//
//   3. 三种数据访问模式：
//      - Latest(): 获取最新一条 → WebSocket 广播使用（高频推送）
//      - PollSince(cursor): 增量拉取 → HTTP Stream API 使用（避免重复消费）
//      - Recent(n): 获取最近 N 条 → HTTP Collect API 和 Export API 使用
//
//   4. 零拷贝推送（WebSocket 去重）
//      - WebSocketManager 在 BroadcastLoop 中通过 Latest() 获取最新数据
//      - 与上次广播的数据比较（shared_ptr 比较），相同则跳过
//      - 这意味着：如果数据没有变化，不会产生任何序列化和网络开销
//
//   5. 全局单例 Store
//      - StreamSinkStore 是全局单例，按 Feature 名索引
//      - Sink 插件（StreamSink）和 HTTP API / WebSocket 层都通过此单例访问数据
//      - 避免了为每个 Feature 单独创建 WebSocket 连接的复杂性
// =============================================================================

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// =============================================================================
// StreamBuffer — 单个 Feature 的环形数据缓冲区
// =============================================================================
//
// 每个 Feature 对应一个 StreamBuffer 实例，存储在 StreamSinkStore 中。
// 内部使用 std::deque + std::mutex 实现线程安全的环形缓冲区。
//
// 线程安全：
//   - Push 在 SinkPool 线程中调用（Pipeline 的 Sink::Write 在 SinkPool 中执行）
//   - Latest/PollSince/Recent 在 HTTP 线程或 WebSocket 广播线程中调用
//   - 所有方法使用 std::mutex 保护，保证线程安全
//
// 内存管理：
//   - DataBatch 使用 shared_ptr 管理，StreamBuffer 只持有引用
//   - 当 DataBatch 被淘汰出 deque 时，如果没有其他引用，自动释放内存
//   - 不会造成内存泄漏
class StreamBuffer {
public:
    // 构造
    // max_batches: 环形缓冲区最大容量（默认 60）
    //             超过此值时，最早的数据会被自动淘汰（pop_front）
    explicit StreamBuffer(size_t max_batches = 60)
        : max_batches_(max_batches) {}

    // ---- Push — 推入新的数据批次（由 StreamSink::Write 调用） ----
    // 将 DataBatch 添加到缓冲区末尾，递增全局序号。
    // 如果缓冲区已满（超过 max_batches_），淘汰最旧的数据。
    //
    // 调用方：SinkPool 线程（Pipeline 的 Sink::Write 在 SinkPool 中异步执行）
    void Push(DataBatchPtr batch) {
        std::lock_guard<std::mutex> lk(mu_);
        batches_.push_back(std::move(batch));
        ++seq_;  // 全局序号递增（用于 cursor 增量拉取机制）
        // 环形缓冲区淘汰：超过上限时弹出最旧的数据
        while (batches_.size() > max_batches_)
            batches_.pop_front();
    }

    // ---- Latest — 获取最新一条数据（WebSocket 广播使用） ----
    // 返回缓冲区中最新的 DataBatch（不弹出）。
    // 如果缓冲区为空，返回 nullptr。
    //
    // 调用方：WebSocket 广播线程（WebSocketManager::BroadcastLoop）
    // 去重机制：WebSocketManager 会比较本次 Latest() 返回的 shared_ptr
    //          与上次广播的 shared_ptr 是否相同，相同则跳过广播。
    DataBatchPtr Latest() const {
        std::lock_guard<std::mutex> lk(mu_);
        return batches_.empty() ? nullptr : batches_.back();
    }

    // ---- PollSince — 增量拉取（支持 cursor 避免重复消费） ----
    // 返回 cursor 之后的所有新数据批次，并更新 cursor 为当前最新序号。
    //
    // 参数：
    //   cursor: [in/out] 上次消费的序号，调用后更新为当前最新序号
    //
    // 返回值：
    //   从 cursor 之后到当前最新的所有 DataBatch 列表
    //
    // 边界情况处理：
    //   1. cursor >= 当前序号 → 无新数据，返回空列表
    //   2. 数据被淘汰 → cursor 回退到可用的最早数据位置
    //      （例如客户端断连 5 分钟，缓冲区只保留 60 条，中间的数据已丢失）
    //   3. 正常增量 → 返回 cursor 之后的所有新数据
    //
    // 调用方：HTTP API 线程（/api/v1/features/:name/stream?cursor=N）
    std::vector<DataBatchPtr> PollSince(uint64_t& cursor) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<DataBatchPtr> out;
        uint64_t available = seq_;  // 当前最新序号

        // 情况 1：cursor 已是最新，无新数据
        if (cursor >= available) {
            cursor = available;
            return out;
        }

        // 计算需要跳过的数据量
        // 情况 2：cursor 对应的数据已被淘汰（断连太久）
        //         此时从最早可用数据开始返回
        // 情况 3：正常增量，跳过已消费的数据
        size_t skip = 0;
        if (available - cursor > batches_.size()) {
            // 数据已被淘汰：cursor 回退到最早可用位置
            skip = 0;
            cursor = available - batches_.size();
        } else {
            // 正常增量：跳过已消费的数据
            skip = batches_.size() - (available - cursor);
        }

        // 从 skip 位置开始，收集所有剩余数据
        for (size_t i = skip; i < batches_.size(); ++i)
            out.push_back(batches_[i]);
        cursor = available;  // 更新 cursor 为最新序号
        return out;
    }

    // ---- Recent — 获取最近 N 条数据 ----
    // 返回缓冲区中最近 n 条数据（不弹出）。
    // 如果缓冲区中数据不足 n 条，返回所有可用数据。
    //
    // 调用方：
    //   - HTTP API（/api/v1/features/:name/collect → Recent(1)）
    //   - Export API（POST /api/v1/export → Recent(lookback_batches)）
    //
    // 参数：
    //   n: 需要获取的最近数据条数
    std::vector<DataBatchPtr> Recent(size_t n) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<DataBatchPtr> out;
        size_t start = batches_.size() > n ? batches_.size() - n : 0;
        for (size_t i = start; i < batches_.size(); ++i)
            out.push_back(batches_[i]);
        return out;
    }

    // ---- 查询方法 ----
    uint64_t Sequence() const { return seq_.load(); }  // 当前全局序号（用于 cursor 初始化）
    size_t Size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return batches_.size();
    }

private:
    mutable std::mutex mu_;          // 互斥锁（mutable 允许在 const 方法中加锁）
    std::deque<DataBatchPtr> batches_;  // 数据批次队列（DataBatch 使用 shared_ptr 管理）
    std::atomic<uint64_t> seq_{0};      // 全局递增序号（每次 Push 递增，用于 cursor 机制）
    size_t max_batches_;                // 最大缓冲区容量（默认 60）
};

// =============================================================================
// StreamSinkStore — 全局流数据存储（按 Feature 名索引）
// =============================================================================
//
// 全局单例，管理所有 Feature 的 StreamBuffer。
//
// 设计意图：
//   为什么需要全局单例 Store，而不是每个 Feature 自己管理 StreamBuffer？
//
//   1. 跨层访问：StreamSink 插件在 Pipeline 内部（Sink 层），
//      HTTP API 和 WebSocket 在 Server 层。如果没有全局 Store，
//      需要为每个 Feature 单独创建通信通道，复杂度高。
//
//   2. 生命周期解耦：StreamBuffer 的生命周期与 Feature/Pipeline 解耦。
//      Feature 停止时，调用 RemoveBuffer 清除缓冲区（避免重启后返回旧数据）。
//      但 Store 本身不受影响。
//
//   3. 统一接口：所有数据消费方（HTTP API、WebSocket、Export）都通过
//      同一个 GetBuffer(name) 接口访问数据，避免了重复实现。
//
// 线程安全：
//   所有方法使用 std::mutex 保护（GetBuffer 和 RemoveBuffer 是写操作）。
class StreamSinkStore {
public:
    // 全局单例访问（Meyers' Singleton，线程安全）
    static StreamSinkStore& Instance() {
        static StreamSinkStore inst;
        return inst;
    }

    // ---- GetBuffer — 获取指定 Feature 的 StreamBuffer ----
    // 如果 Feature 的缓冲区不存在，自动创建（惰性初始化）。
    // 返回引用，调用者可以安全地持有和使用（Store 生命周期是全局的）。
    //
    // 参数：
    //   feature_name: Feature 名称（如 "cpu_utilization"）
    //
    // 返回：
    //   对应 Feature 的 StreamBuffer 引用
    StreamBuffer& GetBuffer(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = buffers_.find(feature_name);
        if (it == buffers_.end()) {
            // 惰性创建：首次访问时自动创建 StreamBuffer
            auto [inserted, _] = buffers_.emplace(
                feature_name, std::make_unique<StreamBuffer>());
            return *inserted->second;
        }
        return *it->second;
    }

    // ---- RemoveBuffer — 移除指定 Feature 的 StreamBuffer ----
    // Feature 停止或重配置时调用，清除旧数据。
    // 避免重启后返回过期采样数据。
    //
    // 调用方：
    //   - FeatureManager::StopAndDestroyPipeline()（Feature 停止时）
    //   - FeatureManager::ReconfigureFilter()（在线重配置时）
    //
    // 参数：
    //   feature_name: Feature 名称
    void RemoveBuffer(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        buffers_.erase(feature_name);
    }

private:
    mutable std::mutex mu_;
    // key: Feature 名称，value: StreamBuffer 实例（unique_ptr 管理所有权）
    std::unordered_map<std::string, std::unique_ptr<StreamBuffer>> buffers_;
};

// =============================================================================
// StreamSink — Sink 插件实现
// =============================================================================
//
// StreamSink 是 Pipeline 中的一个 Sink 插件，负责将处理后的 DataBatch
// 推送到 StreamSinkStore 中对应的 Feature 缓冲区。
//
// 这是 FeatureManager 自动注入的 Sink（不需要用户在 YAML 配置中显式声明）。
// 每个 Feature 的 Pipeline 创建时，FeatureManager 会自动添加一个 StreamSink。
//
// 数据流：
//   Pipeline::SubmitToSinks() → StreamSink::Write() → StreamSinkStore::GetBuffer().Push()
//
// 调用方：SinkPool 线程（Pipeline 在 SinkPool 中异步执行 Sink::Write）
class StreamSink : public SinkPlugin {
public:
    const char* Name() const override { return "stream_sink"; }
    const char* Version() const override { return "0.2.0"; }

    // ---- Init — 从配置初始化 ----
    // 配置参数：
    //   feature_name: Feature 名称（用于索引 StreamSinkStore）
    //   max_buffer:   缓冲区最大容量（默认 60）
    Status Init(const ConfigValue& config) override {
        feature_name_ = config["feature_name"].AsString("default");
        max_buffer_ = static_cast<size_t>(config["max_buffer"].AsInt(60));
        return Status::Ok();
    }

    // ---- Write — 将 DataBatch 推送到缓冲区 ----
    // 这是 SinkPlugin 接口的核心方法，由 Pipeline 在 SinkPool 中调用。
    //
    // 参数：
    //   batch: 处理后的 DataBatch（shared_ptr 管理，零拷贝传递）
    //
    // 返回：
    //   Ok: 推送成功
    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        StreamSinkStore::Instance().GetBuffer(feature_name_).Push(
            std::move(batch));
        return Status::Ok();
    }

    // ---- SetFeatureName — 设置 Feature 名称（由 FeatureManager 调用） ----
    // FeatureManager 在创建 Pipeline 时调用此方法，将 Feature 名称绑定到 StreamSink。
    // 这比通过 Init 配置更直接，因为 FeatureManager 创建 StreamSink 时已经知道 Feature 名称。
    //
    // 参数：
    //   name: Feature 名称（如 "cpu_utilization"）
    void SetFeatureName(const std::string& name) { feature_name_ = name; }

private:
    std::string feature_name_ = "default";  // Feature 名称（用于索引 StreamSinkStore）
    size_t max_buffer_ = 60;                // 缓冲区最大容量
};

// 注册 StreamSink 插件到全局 PluginRegistry
// 这使 PluginRegistry 能够通过 "stream_sink" 名称创建 StreamSink 实例。
IL_REGISTER_SINK("stream_sink", StreamSink);

}  // namespace illuminator