// ============================================================================
// Illuminator AsyncChannel — 支持事件多态的管道异步通道
// ============================================================================
//
// 封装 LockFreeQueue，传输 variant<DataBatchPtr, FlushSentinel>，
// 使 ProcessThread 成为纯事件处理器 (Event Handler)。
//
// Channel 传输两种消息:
//   - DataBatchPtr: 正常数据批次
//   - FlushSentinel: Aggregator flush 触发信号（由 TimerWheel 注入）
//
// 设计要点:
//   1. 数据入队 TryEnqueue: 无锁 CAS，纳秒级
//   2. Sentinel 注入 InjectFlush: 优先级高于普通数据
//   3. 消费端 Dequeue: spin → yield → sleep 三级退避
//   4. 水位线反压: 超过 high watermark 时触发 backpressured
//   5. 全量统计: enqueued / dequeued / dropped / flush_injected
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <variant>

#include "core/engine/data_batch.h"
#include "core/memory/lock_free_queue.h"

namespace illuminator {

// Overloaded helper for std::visit (C++17)
template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

struct FlushSentinel {};

using ChannelItem = std::variant<DataBatchPtr, FlushSentinel>;

enum class DropPolicy : uint8_t {
    kDropNewest,
    kDropOldest,
};

template <size_t Capacity = 4096>
class AsyncChannel {
public:
    struct Stats {
        std::atomic<uint64_t> enqueued{0};
        std::atomic<uint64_t> dropped{0};
        std::atomic<uint64_t> dequeued{0};
        std::atomic<uint64_t> flush_injected{0};
        std::atomic<uint64_t> backpressure_events{0};
    };

    explicit AsyncChannel(DropPolicy policy = DropPolicy::kDropNewest,
                          double high_wm = 0.8, double low_wm = 0.2)
        : drop_policy_(policy), high_wm_(high_wm), low_wm_(low_wm) {}

    bool TryEnqueue(DataBatchPtr batch) {
        ChannelItem item(std::move(batch));
        if (queue_.TryPush(std::move(item))) {
            stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            UpdateBackpressure();
            return true;
        }

        if (drop_policy_ == DropPolicy::kDropOldest) {
            ChannelItem discarded;
            if (queue_.TryPop(discarded)) {
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            ChannelItem retry(std::move(batch));
            if (queue_.TryPush(std::move(retry))) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return true;
            }
        }

        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        UpdateBackpressure();
        return false;
    }

    bool InjectFlush() {
        ChannelItem item(FlushSentinel{});
        if (queue_.TryPush(std::move(item))) {
            stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        ChannelItem discarded;
        if (queue_.TryPop(discarded)) {
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        }
        ChannelItem retry(FlushSentinel{});
        if (queue_.TryPush(std::move(retry))) {
            stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    std::optional<ChannelItem> Dequeue(std::chrono::milliseconds timeout) {
        ChannelItem result;

        for (int i = 0; i < 100; ++i) {
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return result;
            }
        }

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

    std::optional<ChannelItem> TryDequeue() {
        ChannelItem result;
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

    LockFreeQueue<ChannelItem, Capacity> queue_;
    Stats stats_;
    std::atomic<bool> backpressured_{false};
    DropPolicy drop_policy_;
    double high_wm_;
    double low_wm_;
};

}  // namespace illuminator
