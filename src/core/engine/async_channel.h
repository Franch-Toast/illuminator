// ============================================================================
// Illuminator AsyncChannel — 基于 LockFreeQueue 的管道异步通道
// ============================================================================
//
// 封装 LockFreeQueue，为 Pipeline 提供 Source → ProcessThread 的异步解耦。
//
// 设计要点：
// ==========
// 1. 生产端（Source 回调 / CollectLoop）调用 TryEnqueue —— 无锁 CAS，纳秒级
// 2. 消费端（ProcessLoop）调用 Dequeue(timeout) —— spin → yield → sleep 三级退避
// 3. 水位线反压：队列使用率超过 high watermark 时触发 backpressured 标志
// 4. 丢弃策略：队列满时可选 drop_newest（默认）或 drop_oldest
// 5. 全量统计：enqueued / dequeued / dropped / backpressure_events
//
// 模板参数 Capacity 必须是 2 的幂（LockFreeQueue 约束）。
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "core/engine/data_batch.h"
#include "core/memory/lock_free_queue.h"

namespace illuminator {

enum class DropPolicy : uint8_t {
    kDropNewest,   // 队列满时丢弃新到达的数据（默认，最安全）
    kDropOldest,   // 队列满时丢弃队首旧数据，腾出空间给新数据
};

template <size_t Capacity = 4096>
class AsyncChannel {
public:
    struct Stats {
        std::atomic<uint64_t> enqueued{0};
        std::atomic<uint64_t> dropped{0};
        std::atomic<uint64_t> dequeued{0};
        std::atomic<uint64_t> backpressure_events{0};
    };

    explicit AsyncChannel(DropPolicy policy = DropPolicy::kDropNewest,
                          double high_wm = 0.8, double low_wm = 0.2)
        : drop_policy_(policy), high_wm_(high_wm), low_wm_(low_wm) {}

    // 生产端：尝试入队。队列满时按 drop_policy_ 处理。
    // 返回 true 表示数据已成功入队。
    bool TryEnqueue(DataBatchPtr batch) {
        if (queue_.TryPush(std::move(batch))) {
            stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            UpdateBackpressure();
            return true;
        }

        // 队列满
        if (drop_policy_ == DropPolicy::kDropOldest) {
            DataBatchPtr discarded;
            if (queue_.TryPop(discarded)) {
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            if (queue_.TryPush(std::move(batch))) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return true;
            }
        }

        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        UpdateBackpressure();
        return false;
    }

    // 消费端：阻塞式出队（带超时）。
    // 使用 spin → yield → sleep 三级退避策略，平衡延迟与 CPU 占用。
    std::optional<DataBatchPtr> Dequeue(std::chrono::milliseconds timeout) {
        DataBatchPtr result;

        // 阶段 1：spin（~100 次，适合极低延迟场景）
        for (int i = 0; i < 100; ++i) {
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return result;
            }
        }

        // 阶段 2：yield + sleep，按 1ms 步进检查直到超时
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return std::nullopt;
    }

    // 消费端：非阻塞出队
    std::optional<DataBatchPtr> TryDequeue() {
        DataBatchPtr result;
        if (queue_.TryPop(result)) {
            stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
            UpdateBackpressure();
            return result;
        }
        return std::nullopt;
    }

    bool IsBackpressured() const {
        return backpressured_.load(std::memory_order_acquire);
    }

    size_t SizeApprox() const { return queue_.SizeApprox(); }
    static constexpr size_t capacity() { return Capacity; }

    const Stats& stats() const { return stats_; }

private:
    void UpdateBackpressure() {
        bool was = backpressured_.load(std::memory_order_relaxed);
        if (!was && queue_.AboveHighWatermark(high_wm_)) {
            if (backpressured_.compare_exchange_strong(was, true,
                    std::memory_order_release)) {
                stats_.backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (was && queue_.BelowLowWatermark(low_wm_)) {
            backpressured_.store(false, std::memory_order_release);
        }
    }

    LockFreeQueue<DataBatchPtr, Capacity> queue_;
    Stats stats_;
    std::atomic<bool> backpressured_{false};
    DropPolicy drop_policy_;
    double high_wm_;
    double low_wm_;
};

}  // namespace illuminator
