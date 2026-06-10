// AsyncChannel 单元测试：入队/出队、丢弃策略、反压水位线与统计计数。

#include "core/engine/async_channel.h"
#include "core/engine/data_batch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <variant>

namespace illuminator {
namespace {

using namespace std::chrono_literals;

// 空 channel：近似大小为 0，未处于反压状态。
TEST(AsyncChannelTest, EmptyChannelHasExpectedInitialState) {
    AsyncChannel ch(64, DropPolicy::kDropNewest, 0.8, 0.2);
    EXPECT_EQ(ch.SizeApprox(), 0u);
    EXPECT_FALSE(ch.IsBackpressured());
    EXPECT_EQ(ch.stats().enqueued.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(ch.stats().dropped.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(ch.stats().dequeued.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(ch.stats().flush_injected.load(std::memory_order_relaxed), 0u);
}

// 入队成功后 Dequeue 能取回同一批次的原始指针。
TEST(AsyncChannelTest, EnqueueThenDequeueYieldsSameBatch) {
    AsyncChannel ch(64);

    auto batch = std::make_shared<DataBatch>();
    DataBatch* raw = batch.get();
    ASSERT_TRUE(ch.TryEnqueue(batch));

    auto item = ch.Dequeue(500ms);
    ASSERT_TRUE(item.has_value());
    auto* ptr = std::get_if<DataBatchPtr>(&*item);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr->get(), raw);

    EXPECT_EQ(ch.stats().enqueued.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(ch.stats().dequeued.load(std::memory_order_relaxed), 1u);
}

// InjectFlush：队列中出现 FlushSentinel 事件。
TEST(AsyncChannelTest, InjectFlushEnqueuesFlushSentinel) {
    AsyncChannel ch(64);

    ASSERT_TRUE(ch.InjectFlush());
    auto opt = ch.TryDequeue();
    ASSERT_TRUE(opt.has_value());
    EXPECT_NE(std::get_if<FlushSentinel>(&*opt), nullptr);
    EXPECT_EQ(ch.stats().flush_injected.load(std::memory_order_relaxed), 1u);
}

// std::variant 派发：区分数据批次与控制哨兵。
TEST(AsyncChannelTest, VariantDispatchDistinguishesDataAndFlush) {
    AsyncChannel ch(64);

    auto batch = std::make_shared<DataBatch>();
    ASSERT_TRUE(ch.TryEnqueue(batch));
    ASSERT_TRUE(ch.InjectFlush());

    int kind_sum = 0;
    int steps = 0;
    while (steps < 2) {
        auto item = ch.TryDequeue();
        ASSERT_TRUE(item.has_value());
        std::visit(
            Overloaded{
                [&](const DataBatchPtr&) {
                    kind_sum += 1;
                },
                [&](FlushSentinel) {
                    kind_sum += 10;
                },
            },
            *item);
        ++steps;
    }
    EXPECT_EQ(kind_sum, 11);
}

// DropNewest：队列满时新数据被拒收并计数 dropped。
TEST(AsyncChannelTest, DropNewestDropsNewElementWhenFull) {
    AsyncChannel ch(16, DropPolicy::kDropNewest, 0.99, 0.01);

    for (size_t i = 0;; ++i) {
        auto b = std::make_shared<DataBatch>();
        if (!ch.TryEnqueue(b)) {
            break;
        }
        ASSERT_LT(i, ch.capacity() + 64u);  // 安全措施，避免极端情况下死循环测试
    }

    auto extra = std::make_shared<DataBatch>();
    ASSERT_FALSE(ch.TryEnqueue(extra));

    EXPECT_GE(ch.stats().dropped.load(std::memory_order_relaxed), 1u);
}

// DropOldest：队列满时会丢弃队头再接受新元素。
TEST(AsyncChannelTest, DropOldestEvictsOldestWhenFull) {
    AsyncChannel ch(16, DropPolicy::kDropOldest, 0.99, 0.01);

    auto oldest = std::make_shared<DataBatch>();
    ASSERT_TRUE(ch.TryEnqueue(oldest));

    for (size_t i = 1; i < ch.capacity(); ++i) {
        ASSERT_TRUE(ch.TryEnqueue(std::make_shared<DataBatch>()));
    }

    auto newest = std::make_shared<DataBatch>();
    ASSERT_TRUE(ch.TryEnqueue(newest));  // 应弹出队头再给新条目腾位

    bool saw_oldest = false;
    for (size_t n = 0; n < ch.capacity(); ++n) {
        std::optional<ChannelItem> item = ch.TryDequeue();
        ASSERT_TRUE(item.has_value());
        ASSERT_TRUE(std::holds_alternative<DataBatchPtr>(*item));
        if (std::get<DataBatchPtr>(*item).get() == oldest.get()) {
            saw_oldest = true;
        }
    }
    EXPECT_FALSE(saw_oldest);  // 最老的批次应已在腾挪过程中被淘汰
    EXPECT_GE(ch.stats().dropped.load(std::memory_order_relaxed), 1u);

    ASSERT_FALSE(ch.TryDequeue());
}

// 反压：高水位触发、低水位解除（使用较小容量与高/低阈值便于逼近边界）。
TEST(AsyncChannelTest, BackpressureSignalsHighAndClearsNearLowWatermark) {
    AsyncChannel ch(64, DropPolicy::kDropNewest, 0.8, 0.2);

    ASSERT_FALSE(ch.IsBackpressured());
    auto target = static_cast<size_t>(64 * 0.8) + 2;

    for (size_t guard = 0; ch.SizeApprox() < target && guard < ch.capacity() * 8;
         ++guard) {
        ASSERT_TRUE(ch.TryEnqueue(std::make_shared<DataBatch>()));
    }
    EXPECT_TRUE(ch.IsBackpressured());

    while (ch.SizeApprox() >= 12) {
        auto x = ch.TryDequeue();
        ASSERT_TRUE(x.has_value());
    }
    ASSERT_FALSE(ch.IsBackpressured());
    EXPECT_GT(ch.stats().backpressure_events.load(std::memory_order_relaxed), 0u);
}

// 统计数据：enqueue/drop/dequeue/flush 计数与实际行为一致。
TEST(AsyncChannelTest, StatsCountersTrackOperations) {
    AsyncChannel ch(64, DropPolicy::kDropNewest, 0.99, 0.01);

    ASSERT_TRUE(ch.TryEnqueue(std::make_shared<DataBatch>()));
    ASSERT_TRUE(ch.InjectFlush());
    ASSERT_TRUE(ch.TryEnqueue(std::make_shared<DataBatch>()));

    ASSERT_TRUE(ch.TryDequeue());
    ASSERT_TRUE(ch.TryDequeue());
    ASSERT_TRUE(ch.TryDequeue());

    EXPECT_EQ(ch.stats().enqueued.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(ch.stats().flush_injected.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(ch.stats().dequeued.load(std::memory_order_relaxed), 3u);

    AsyncChannel tiny(4, DropPolicy::kDropNewest, 0.99, 0.01);
    for (size_t i = 0; i < tiny.capacity(); ++i) {
        ASSERT_TRUE(tiny.TryEnqueue(std::make_shared<DataBatch>()));
    }
    ASSERT_FALSE(tiny.TryEnqueue(std::make_shared<DataBatch>()));  // newest dropped

    EXPECT_GE(tiny.stats().dropped.load(std::memory_order_relaxed), 1u);
}

// Dequeue 超时：空闲时应返回 std::nullopt。
TEST(AsyncChannelTest, DequeueReturnsNulloptOnTimeoutWhenEmpty) {
    AsyncChannel ch(32);
    auto dead = std::chrono::steady_clock::now();
    auto res = ch.Dequeue(80ms);
    EXPECT_FALSE(res.has_value());

    EXPECT_GE(std::chrono::steady_clock::now() - dead, 60ms);  // 应经历近似完整的超时窗口
}

// 队列满时 InjectFlush 通过丢弃一个槽位仍可优先注入 FlushSentinel。
TEST(AsyncChannelTest, InjectFlushUsesPrioritySlotWhenFull) {
    AsyncChannel ch(8, DropPolicy::kDropNewest, 0.99, 0.01);

    for (size_t i = 0; i < ch.capacity(); ++i) {
        ASSERT_TRUE(ch.TryEnqueue(std::make_shared<DataBatch>()));
    }

    ASSERT_TRUE(ch.InjectFlush());
    bool saw_flush = false;
    bool saw_data = false;
    for (size_t n = 0; n < ch.capacity(); ++n) {
        auto it = ch.TryDequeue();
        ASSERT_TRUE(it.has_value());
        if (std::holds_alternative<FlushSentinel>(*it)) {
            EXPECT_FALSE(saw_flush);
            saw_flush = true;
        } else {
            saw_data = true;
        }
    }

    EXPECT_TRUE(saw_flush);
    EXPECT_TRUE(saw_data);  // 仍然应保留了大量数据帧（FIFO 语义）
}

}  // namespace
}  // namespace illuminator
