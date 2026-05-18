// ============================================================================
// LockFreeQueue 单元测试 — 校验 MPSC 无锁环形缓冲区的语义与安全边界
//
// 覆盖要点：
// - 空队列语义、TryPush/TryPop 成功与失败分支
// - FIFO 顺序、容量与水位线
// - 多生产者并发入队（生产者通过 CAS 争抢 head_/tail_）下的安全性
// - std::unique_ptr 等只可移动类型的入队/出队
// - 析构函数通过循环 TryPop 自动排空队列
// ============================================================================

#include "core/memory/lock_free_queue.h"

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace illuminator {
namespace {

// 校验空队列在未入队时应满足 Empty 与近似长度为 0
TEST(LockFreeQueueTest, EmptyQueue_HasZeroApproxSizeAndIsEmpty) {
    LockFreeQueue<int> queue(16);
    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.SizeApprox(), 0U);
    EXPECT_FALSE(queue.AboveHighWatermark());
    EXPECT_TRUE(queue.BelowLowWatermark());

    int v = 0;
    EXPECT_FALSE(queue.TryPop(v));
}

// 单元素出入队路径：TryPush/TryPop 成功，出队后置空队列
TEST(LockFreeQueueTest, SinglePushPop_RoundTripPreservesValueAndEmpties) {
    LockFreeQueue<int> queue(16);
    ASSERT_TRUE(queue.TryPush(42));

    EXPECT_FALSE(queue.Empty());
    EXPECT_EQ(queue.SizeApprox(), 1U);

    int v = 0;
    ASSERT_TRUE(queue.TryPop(v));
    EXPECT_EQ(v, 42);
    EXPECT_TRUE(queue.Empty());

    ASSERT_FALSE(queue.TryPop(v));
}

// 在未出队前提下持续入队直至失败，校验满队列后 TryPush 失败且仍可按 FIFO 出队
TEST(LockFreeQueueTest, SaturateQueue_SubsequentTryPushFailsStillFifo) {
    LockFreeQueue<int> queue(64);
    size_t inserted = 0;
    while (queue.TryPush(static_cast<int>(inserted))) {
        ++inserted;
        ASSERT_LT(inserted, 100000U) << "TryPush 一直成功，可能存在实现缺陷。";
    }

    EXPECT_EQ(inserted, queue.capacity()) << "本实现下可同时存放 capacity() 条消息。";
    ASSERT_FALSE(queue.TryPush(-1));

    for (size_t i = 0; i < inserted; ++i) {
        int v = -999;
        ASSERT_TRUE(queue.TryPop(v)) << "索引 " << i;
        EXPECT_EQ(static_cast<size_t>(v), i);
    }
    EXPECT_TRUE(queue.Empty());
}

// 串行校验 FIFO：严格按递增序列入队并按相同顺序弹出
TEST(LockFreeQueueTest, Fifo_OrderPreservedUnderSerialPushPop) {
    LockFreeQueue<int> queue(128);
    const int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(queue.TryPush(i));
    }
    for (int i = 0; i < kCount; ++i) {
        int v = 0;
        ASSERT_TRUE(queue.TryPop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(queue.Empty());
}

// capacity() 应与构造参数一致（向上取整到 2 的幂）
TEST(LockFreeQueueTest, Capacity_MatchesConstructorParameter) {
    LockFreeQueue<int> q1024(1024);
    EXPECT_EQ(q1024.capacity(), 1024U);

    LockFreeQueue<int> q8(8);
    EXPECT_EQ(q8.capacity(), 8U);

    LockFreeQueue<int> q7(7);
    EXPECT_EQ(q7.capacity(), 8U);
}

// 水位线：AboveHighWatermark / BelowLowWatermark 与近似长度的一致性（默认 80% / 20%）
TEST(LockFreeQueueTest, Watermarks_ReflectApproxFillLevel) {
    constexpr size_t kCap = 32;
    LockFreeQueue<int> queue(kCap);

    // 填入 13 条：Ceil(13/32)>0.8 阈值使用严格大于比较，等价于近似长度跨过 80%×Capacity
    const size_t kHighFill = static_cast<size_t>(kCap * 0.8) + 1;
    for (size_t i = 0; i < kHighFill; ++i) {
        ASSERT_TRUE(queue.TryPush(static_cast<int>(i)));
    }
    EXPECT_GE(queue.SizeApprox(), kHighFill);
    EXPECT_TRUE(queue.AboveHighWatermark());

    while (!queue.Empty()) {
        int v = 0;
        ASSERT_TRUE(queue.TryPop(v));
    }
    EXPECT_TRUE(queue.BelowLowWatermark());

    const size_t kLowFill =
        std::max<size_t>(1, static_cast<size_t>(kCap * 0.2) - 1);  // 严格小于低水位阈值
    for (size_t i = 0; i < kLowFill; ++i) {
        ASSERT_TRUE(queue.TryPush(static_cast<int>(i)));
    }
    EXPECT_TRUE(queue.SizeApprox() < static_cast<size_t>(kCap * 0.2));
    EXPECT_TRUE(queue.BelowLowWatermark());
}

// 多生产者并发入队（单消费者在旁等待），验证无丢失、无额外元素
TEST(LockFreeQueueTest, ConcurrentProducers_AllValuesReceivedSingleConsumerDrain) {
    constexpr size_t kCapacity = 2048;
    constexpr int kThreads = 8;
    constexpr int kPushesPerThread = 250;
    constexpr int kExpectedTotal = kThreads * kPushesPerThread;

    LockFreeQueue<int> queue(kCapacity);

    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&queue, t]() {
            const int base = t * kPushesPerThread;
            for (int i = 0; i < kPushesPerThread; ++i) {
                const int payload = base + i;
                // 队列足够大，正常应一次成功；若失败则自旋重试以排除偶发竞态带来的瞬时满队列读法
                while (!queue.TryPush(payload)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& th : producers) {
        th.join();
    }

    EXPECT_EQ(queue.SizeApprox(), static_cast<size_t>(kExpectedTotal));

    std::vector<int> received;
    received.reserve(kExpectedTotal);
    int v = 0;
    while (queue.TryPop(v)) {
        received.push_back(v);
    }
    ASSERT_EQ(received.size(), static_cast<size_t>(kExpectedTotal));

    std::sort(received.begin(), received.end());
    for (int i = 0; i < kExpectedTotal; ++i) {
        EXPECT_EQ(received[static_cast<size_t>(i)], i) << "缺失或重复元素，说明并发入队不安全。";
    }
}

// 大量 push/pop 交错后队列应回到空且 TryPop 失败
TEST(LockFreeQueueTest, HeavyChurn_EndsEmptyAndConsistent) {
    LockFreeQueue<int> queue(256);
    const int kCycles = 5000;
    for (int c = 0; c < kCycles; ++c) {
        ASSERT_TRUE(queue.TryPush(c));
        int v = 0;
        ASSERT_TRUE(queue.TryPop(v));
        EXPECT_EQ(v, c);
    }
    EXPECT_TRUE(queue.Empty());
    int v = 0;
    EXPECT_FALSE(queue.TryPop(v));
}

// 只可移动类型：std::unique_ptr 走移动路径，确保无法拷贝时仍可工作
TEST(LockFreeQueueTest, MoveOnly_UniquePtrPushPop) {
    LockFreeQueue<std::unique_ptr<int>> queue(16);
    auto p = std::make_unique<int>(7);
    ASSERT_TRUE(queue.TryPush(std::move(p)));
    EXPECT_EQ(p, nullptr);

    std::unique_ptr<int> out;
    ASSERT_TRUE(queue.TryPop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}

// 析构时自动清空：~LockFreeQueue 内部 drain 循环应 Pop 残留元素并让资源析构计数准确
TEST(LockFreeQueueTest, Destructor_DrainsRemainingElements) {
    std::atomic<int> destroyed{0};

    struct CounterNode {
        std::atomic<int>* ctr;
        explicit CounterNode(std::atomic<int>* c) noexcept : ctr(c) {}
        ~CounterNode() { ctr->fetch_add(1, std::memory_order_relaxed); }
    };

    destroyed.store(0, std::memory_order_relaxed);
    {
        LockFreeQueue<std::unique_ptr<CounterNode>> queue(8);
        ASSERT_TRUE(queue.TryPush(std::make_unique<CounterNode>(&destroyed)));
        ASSERT_TRUE(queue.TryPush(std::make_unique<CounterNode>(&destroyed)));
        ASSERT_TRUE(queue.TryPush(std::make_unique<CounterNode>(&destroyed)));
        // 故意不显式调用 TryPop —— drain 必须由析构函数完成
    }
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 3);
}

}  // namespace
}  // namespace illuminator
