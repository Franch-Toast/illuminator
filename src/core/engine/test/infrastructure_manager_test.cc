// InfrastructureManager 单元测试：验证基础设施生命周期管理。

#include "core/engine/infrastructure_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace illuminator {
namespace {

class InfrastructureManagerTest : public ::testing::Test {
protected:
    void TearDown() override {
        InfrastructureManager::Instance().Stop();
    }
};

TEST_F(InfrastructureManagerTest, SingletonReturnsConsistentInstance) {
    auto& a = InfrastructureManager::Instance();
    auto& b = InfrastructureManager::Instance();
    EXPECT_EQ(&a, &b);
}

TEST_F(InfrastructureManagerTest, StartAndStopCleanly) {
    auto& mgr = InfrastructureManager::Instance();
    EXPECT_FALSE(mgr.IsStarted());

    auto status = mgr.Start({.collect_pool_threads = 2, .sink_pool_threads = 2});
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(mgr.IsStarted());

    EXPECT_NE(mgr.GetCollectPool(), nullptr);
    EXPECT_NE(mgr.GetSinkPool(), nullptr);

    auto stop_status = mgr.Stop();
    EXPECT_TRUE(stop_status.ok());
    EXPECT_FALSE(mgr.IsStarted());
}

TEST_F(InfrastructureManagerTest, DoubleStartReturnsError) {
    auto& mgr = InfrastructureManager::Instance();
    EXPECT_TRUE(mgr.Start({.sink_pool_threads = 2}).ok());
    EXPECT_FALSE(mgr.Start({.sink_pool_threads = 2}).ok());
}

TEST_F(InfrastructureManagerTest, StopBeforeStartIsNoOp) {
    auto& mgr = InfrastructureManager::Instance();
    EXPECT_TRUE(mgr.Stop().ok());
}

TEST_F(InfrastructureManagerTest, PoolsExecuteTasks) {
    auto& mgr = InfrastructureManager::Instance();
    mgr.Start({.collect_pool_threads = 2, .sink_pool_threads = 2});

    std::atomic<int> counter{0};

    auto fut1 = mgr.GetCollectPool()->Submit([&] { counter.fetch_add(1); return 0; });
    auto fut2 = mgr.GetSinkPool()->Submit([&] { counter.fetch_add(10); return 0; });

    fut1.get();
    fut2.get();

    EXPECT_EQ(counter.load(), 11);
}

TEST_F(InfrastructureManagerTest, TimerWheelAccessible) {
    auto& mgr = InfrastructureManager::Instance();
    mgr.Start({.sink_pool_threads = 2});

    auto& tw = mgr.GetTimerWheel();
    (void)tw;  // Just verify it doesn't crash
}

TEST_F(InfrastructureManagerTest, AutoThreadCountWhenZero) {
    auto& mgr = InfrastructureManager::Instance();
    mgr.Start({.collect_pool_threads = 2, .sink_pool_threads = 0});

    EXPECT_NE(mgr.GetSinkPool(), nullptr);
    EXPECT_GE(mgr.GetSinkPool()->NumThreads(), 2u);
}

}  // namespace
}  // namespace illuminator
