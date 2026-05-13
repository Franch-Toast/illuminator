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
#include <optional>
#include <new>
#include <type_traits>

namespace illuminator {

template <typename T, size_t Capacity = 4096>
class LockFreeQueue {
    // 编译期保证 Capacity 是 2 的幂（位掩码取模的前提）
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    LockFreeQueue() : head_(0), tail_(0) {
        // 初始化每个 Cell 的序列号为位置索引
        // 这使得初次 Push/Pop 时序列号与位置匹配
        for (size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LockFreeQueue() {
        // 安全退出：消费完所有剩余元素
        T tmp;
        while (TryPop(tmp)) {}
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    // ---- 左值入队 ----
    // 尝试推入一个元素（拷贝），成功返回 true，队列满返回 false
    bool TryPush(const T& value) {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];  // 位掩码取模
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // 序列号匹配：该 Cell 可以写入
                // CAS 尝试获取 head_ 的所有权
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;  // 获取成功
                }
            } else if (diff < 0) {
                // 序列号小于位置：队列已满（消费者落后太多）
                return false;
            } else {
                // 其他生产者正在写入，重新加载 head_ 重试
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        // 写入数据并更新序列号（表示该位置已填充）
        cell->data = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // ---- 右值入队 ----
    // 移动语义版本，避免不必要的拷贝
    bool TryPush(T&& value) {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
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
        cell->data = std::move(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // ---- 出队 ----
    // 仅限单消费者调用（MPSC 中的 Single Consumer）。
    // 成功返回 true 并通过引用参数传出数据，队列空返回 false。
    bool TryPop(T& value) {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                // 序列号匹配：该 Cell 有数据可以读取
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;  // 获取读取权成功
                }
            } else if (diff < 0) {
                // 序列号小于 pos+1：队列为空
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        value = std::move(cell->data);
        // 将序列号前移 Capacity，表示该位置可被生产者重新使用
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    // ---- 状态查询 ----
    // 近似大小（可能略有不准确，无锁读取）
    size_t SizeApprox() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h >= t ? h - t : 0;
    }

    bool Empty() const { return SizeApprox() == 0; }
    static constexpr size_t capacity() { return Capacity; }

    // ---- 水位线查询 ----
    // 高水位：默认 80%，用于触发反压
    bool AboveHighWatermark(double ratio = 0.8) const {
        return SizeApprox() > static_cast<size_t>(Capacity * ratio);
    }

    // 低水位：默认 20%，用于解除反压
    bool BelowLowWatermark(double ratio = 0.2) const {
        return SizeApprox() < static_cast<size_t>(Capacity * ratio);
    }

private:
    static constexpr size_t kMask = Capacity - 1;  // 位掩码（因为 Capacity 是 2 的幂）

    // 队列中的每个槽位
    struct Cell {
        std::atomic<size_t> sequence;  // 当前序列号（用于无锁协调）
        T data;                        // 存储的数据
    };

    // head_ 和 tail_ 各占 64 字节对齐，避免伪共享（False Sharing）
    // 伪共享：当两个不相关的变量在同一缓存行时，一个 CPU 修改会导致
    //         另一个 CPU 的缓存行失效，造成不必要的性能损失
    alignas(64) std::atomic<size_t> head_;   // 生产者索引
    alignas(64) std::atomic<size_t> tail_;   // 消费者索引
    Cell cells_[Capacity];                    // 环形缓冲区
};

}  // namespace illuminator
