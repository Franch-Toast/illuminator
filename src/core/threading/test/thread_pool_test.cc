// ThreadPool 单元测试：验证构造/提交/排队/异常安全/析构同步等行为。

#include "core/threading/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace illuminator {
namespace {

// 基本构造与析构：合法线程数下应能正常创建并在作用域结束时销毁。
TEST(ThreadPoolTest, ConstructsAndDestructsCleanly) {
    ThreadPool pool(2, "ut-pool");
    EXPECT_EQ(pool.NumThreads(), 2u);
    EXPECT_EQ(pool.PendingTasks(), 0u);
}

// Submit 单任务：future.get() 应得到与任务返回值一致的结果。
TEST(ThreadPoolTest, SubmitReturnsFutureWithResult) {
    ThreadPool pool(2);
    auto fut = pool.Submit([](int a, int b) { return a + b; }, 19, 23);
    EXPECT_EQ(fut.get(), 42);
    EXPECT_EQ(pool.PendingTasks(), 0u);
}

// 多任务并行：多枚 future 均完成且结果符合预期。
TEST(ThreadPoolTest, SubmitRunsMultipleTasksInParallel) {
    constexpr int kTasks = 32;
    ThreadPool pool(4);
    std::vector<std::future<int>> futs;
    futs.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        futs.push_back(pool.Submit([i] { return i * i; }));
    }
    for (int i = 0; i < kTasks; ++i) {
        EXPECT_EQ(futs[i].get(), i * i);
    }
}

// PendingTasks：仅统计队列中尚未被工作线程取走的任务数。
TEST(ThreadPoolTest, PendingTasksReflectsQueuedWork) {
    ThreadPool pool(1);

    auto slow_done = pool.Submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return 0;
    });

    // 独占单工作线程后即可快速堆积队列。
    std::vector<std::future<void>> holders;
    for (int i = 0; i < 8; ++i) {
        holders.push_back(pool.Submit([] {}));
    }

    EXPECT_GE(pool.PendingTasks(), 4u);  // 至少部分任务仍待在队列内

    slow_done.wait();
    for (auto& h : holders) {
        h.wait();
    }

    EXPECT_EQ(pool.PendingTasks(), 0u);
}

// NumThreads：与构造参数一致（0 线程时实现会提升到 1）。
TEST(ThreadPoolTest, NumThreadsMatchesConstructor) {
    ThreadPool pool(5, "workers");
    EXPECT_EQ(pool.NumThreads(), 5u);
    ThreadPool single(1);
    EXPECT_EQ(single.NumThreads(), 1u);
}

// 任务抛出异常：异常应保存在 packaged_task/future 中，且工作线程继续服务后续任务。
TEST(ThreadPoolTest, TaskExceptionDoesNotKillWorkerThreads) {
    ThreadPool pool(2);
    auto bad = pool.Submit([]() -> int {
        throw std::runtime_error("expected");
        return 0;
    });
    EXPECT_THROW(static_cast<void>(bad.get()), std::runtime_error);

    std::atomic<bool> done{false};
    auto good = pool.Submit([&done] {
        done.store(true, std::memory_order_release);
        return 7;
    });
    EXPECT_EQ(good.get(), 7);
    EXPECT_TRUE(done.load(std::memory_order_acquire));
}

// 析构：在仍有“执行中”任务时销毁线程池，应阻塞直到任务结束，future 仍可取到结果。
TEST(ThreadPoolTest, DestructorWaitsForInFlightTasks) {
    std::promise<void> started;
    std::promise<void> unblock;
    std::future<void> unblock_fut = unblock.get_future();
    std::future<int> result_fut;

    {
        ThreadPool pool(1);
        result_fut = pool.Submit([&] {
            started.set_value();
            unblock_fut.wait();
            return 42;
        });
        started.get_future().wait();
        unblock.set_value();
    }

    EXPECT_EQ(result_fut.get(), 42);
}

// 单线程池：任务按提交顺序串行执行，可使用非原子共享状态验证顺序。
TEST(ThreadPoolTest, SingleThreadPoolExecutesTasksSerially) {
    ThreadPool pool(1);
    std::string order;
    std::mutex mu;

    auto a = pool.Submit([&] {
        std::lock_guard<std::mutex> lock(mu);
        order.push_back('A');
        return 0;
    });
    auto b = pool.Submit([&] {
        std::lock_guard<std::mutex> lock(mu);
        order.push_back('B');
        return 0;
    });
    auto c = pool.Submit([&] {
        std::lock_guard<std::mutex> lock(mu);
        order.push_back('C');
        return 0;
    });

    a.wait();
    b.wait();
    c.wait();
    EXPECT_EQ(order, "ABC");
}

}  // namespace
}  // namespace illuminator
