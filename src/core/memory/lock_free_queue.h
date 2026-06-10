// ============================================================================
// Illuminator 无锁队列 — 高性能并发数据通道
// ============================================================================
//
// MPSC（多生产者-单消费者）无锁有界队列。
// 设计灵感来源于 LoongCollector 的高低水位队列设计。
//
// 核心特性：
// ==========
// 1. 无锁（Lock-Free）设计
//    - 使用原子变量 head_ 和 tail_ 实现无锁并发
//    - 避免传统互斥锁带来的上下文切换和优先级反转问题
//    - 特别适合 eBPF 事件回调（高频中断上下文）到用户态处理线程的场景
//
// 2. 有界环形缓冲区（Bounded Ring Buffer）
//    - 容量 Capacity 必须是 2 的幂（使用位掩码取模，比 % 运算快很多）
//    - 固定大小避免运行时内存分配
//
// 3. 水位线（Watermark）支持
//    - AboveHighWatermark(): 队列使用率超过 80% 时触发反压（backpressure）
//    - BelowLowWatermark(): 队列使用率低于 20% 时解除反压
//    - 用于流量控制和防止内存无限增长
//
// 实现细节：
// ==========
// 使用的是一种经典的"序列号"无锁队列算法：
//   - 每个 Cell 存储当前处理的序列号（sequence）
//   - 生产者通过 CAS（compare-and-swap）争抢 head_ 位置
//   - 消费者通过 CAS 争抢 tail_ 位置（单消费者场景下竞争很小）
//   - 通过对序列号的比较判断 Cell 是否可安全读写
//
// 使用场景：
// ==========
// 当前在 Pipeline 和 Plugin 间传递 DataBatch 时可能使用（预留）
// 适合在 Source 采集线程到 Processor 处理线程之间建立无锁通道
// ============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace illuminator {

namespace detail {
inline size_t RoundUpPow2(size_t v) {
    if (v == 0) return 1;
    --v;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16; v |= v >> 32;
    return v + 1;
}
}  // namespace detail

template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t requested_capacity = 4096)
        : capacity_(detail::RoundUpPow2(requested_capacity)),
          mask_(capacity_ - 1),
          head_(0),
          tail_(0),
          cells_(std::make_unique<Cell[]>(capacity_)) {
        for (size_t i = 0; i < capacity_; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LockFreeQueue() {
        T tmp;
        while (TryPop(tmp)) {}
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    template <typename U>
    bool TryPush(U&& value) {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::forward<U>(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(T& value) {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        value = std::move(cell->data);
        cell->sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    size_t SizeApprox() const noexcept {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h >= t ? h - t : 0;
    }

    bool Empty() const noexcept { return SizeApprox() == 0; }
    size_t capacity() const noexcept { return capacity_; }

    bool AboveHighWatermark(double ratio = 0.8) const noexcept {
        return SizeApprox() > static_cast<size_t>(capacity_ * ratio);
    }

    bool BelowLowWatermark(double ratio = 0.2) const noexcept {
        return SizeApprox() < static_cast<size_t>(capacity_ * ratio);
    }

private:
    const size_t capacity_;
    const size_t mask_;

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    std::unique_ptr<Cell[]> cells_;
};

}  // namespace illuminator
