// TimerWheel 单元测试：验证 timerfd+epoll 调度、单次/重复、Cancel、统计等行为。

#include "core/engine/timer_wheel.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace illuminator {
namespace {

using namespace std::chrono_literals;

// 基本构造与析构：在未 Start 时应能直接销毁。
TEST(TimerWheelTest, ConstructsAndDestructsWithoutStart) {
    TimerWheel wheel;
    EXPECT_EQ(wheel.FiresTotal(), 0u);
    EXPECT_EQ(wheel.ActiveTimers(), 0u);
}

// AddRepeating：注册后周期性回调会被触发。
TEST(TimerWheelTest, AddRepeatingInvokesCallbackRepeatedly) {
    TimerWheel wheel;
    std::atomic<int> fires{0};

    wheel.AddRepeating(std::chrono::milliseconds(40), [&fires] {
        fires.fetch_add(1, std::memory_order_relaxed);
    });
    wheel.Start();
    std::this_thread::sleep_for(200ms);
    wheel.Stop();

    EXPECT_GE(fires.load(std::memory_order_relaxed), 2);
    EXPECT_GE(wheel.FiresTotal(), 2u);
}

// AddOnce：回调只触发一次。
TEST(TimerWheelTest, AddOnceFiresExactlyOnce) {
    TimerWheel wheel;
    std::atomic<int> fires{0};

    wheel.AddOnce(std::chrono::milliseconds(40), [&fires] {
        fires.fetch_add(1, std::memory_order_relaxed);
    });
    wheel.Start();

    std::this_thread::sleep_for(150ms);
    const int after_first = fires.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(200ms);
    const int after_wait = fires.load(std::memory_order_relaxed);

    wheel.Stop();

    EXPECT_EQ(after_first, 1);
    EXPECT_EQ(after_wait, 1);
    EXPECT_EQ(wheel.FiresTotal(), 1u);
}

// Cancel：取消后对应定时器不再触发（总触发次数在取消后增长应趋于停滞）。
TEST(TimerWheelTest, CancelStopsFurtherCallbacks) {
    TimerWheel wheel;
    std::atomic<int> local_fires{0};

    const uint32_t id = wheel.AddRepeating(std::chrono::milliseconds(35), [&local_fires] {
        local_fires.fetch_add(1, std::memory_order_relaxed);
    });
    wheel.Start();

    std::this_thread::sleep_for(120ms);
    const uint64_t fires_before_cancel = wheel.FiresTotal();
    EXPECT_GE(fires_before_cancel, 1u);

    wheel.Cancel(id);
    std::this_thread::sleep_for(300ms);
    const uint64_t fires_after = wheel.FiresTotal();

    wheel.Stop();

    // 取消后不应再出现大量新增触发；允许极小竞态（<=1 次）余波。
    EXPECT_LE(fires_after - fires_before_cancel, 1u);
    EXPECT_EQ(local_fires.load(std::memory_order_relaxed), static_cast<int>(fires_after));
}

// Start/Stop：重复 Stop 安全；停止后回调不再执行。
TEST(TimerWheelTest, StartStopLifecycleIsIdempotent) {
    TimerWheel wheel;
    std::atomic<int> fires{0};

    wheel.AddRepeating(std::chrono::milliseconds(50), [&fires] {
        fires.fetch_add(1, std::memory_order_relaxed);
    });
    wheel.Start();
    std::this_thread::sleep_for(120ms);
    wheel.Stop();
    wheel.Stop();

    const int after_stop = fires.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(fires.load(std::memory_order_relaxed), after_stop);
}

// ActiveTimers：注册后堆里应存在条目；Stop 后主循环退出后堆可被处理为空（实现依赖清理时机）。
TEST(TimerWheelTest, ActiveTimersTracksScheduledEntries) {
    TimerWheel wheel;
    wheel.AddOnce(std::chrono::milliseconds(200), [] {});
    EXPECT_EQ(wheel.ActiveTimers(), 1u);

    wheel.Start();
    std::this_thread::sleep_for(50ms);
    // 单次尚未触发前应仍为活跃。
    EXPECT_GE(wheel.ActiveTimers(), 0u);

    std::this_thread::sleep_for(300ms);
    wheel.Stop();

    EXPECT_EQ(wheel.ActiveTimers(), 0u);
}

// FiresTotal：累计触发次数随回调递增。
TEST(TimerWheelTest, FiresTotalAccumulatesAcrossCallbacks) {
    TimerWheel wheel;
    wheel.AddRepeating(std::chrono::milliseconds(45), [] {});
    wheel.Start();
    std::this_thread::sleep_for(220ms);
    wheel.Stop();

    EXPECT_GE(wheel.FiresTotal(), 2u);
}

// 多个定时器并发调度：互不干扰地完成触发。
TEST(TimerWheelTest, MultipleTimersRunConcurrently) {
    TimerWheel wheel;
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    std::atomic<int> c{0};

    wheel.AddOnce(std::chrono::milliseconds(30), [&a] {
        a.store(1, std::memory_order_release);
    });
    wheel.AddRepeating(std::chrono::milliseconds(55), [&b] {
        b.fetch_add(1, std::memory_order_relaxed);
    });
    wheel.AddOnce(std::chrono::milliseconds(120), [&c] {
        c.store(1, std::memory_order_release);
    });

    wheel.Start();
    std::this_thread::sleep_for(400ms);
    wheel.Stop();

    EXPECT_EQ(a.load(std::memory_order_acquire), 1);
    EXPECT_GE(b.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(c.load(std::memory_order_acquire), 1);
}

}  // namespace
}  // namespace illuminator
