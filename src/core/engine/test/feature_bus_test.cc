// FeatureBus + FeatureDriver 单元测试

#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace illuminator {
namespace {

// Mock Source for testing
class MockSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_source"; }
    const char* Version() const override { return "1.0.0"; }
    uint32_t IntervalMs() const override { return 100; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        collect_count_.fetch_add(1);
        return batch;
    }

    int CollectCount() const { return collect_count_.load(); }

private:
    std::atomic<int> collect_count_{0};
};

// Mock Sink for testing
class MockSink : public SinkPlugin {
public:
    const char* Name() const override { return "mock_sink"; }
    const char* Version() const override { return "1.0.0"; }

    Status Write(DataBatchPtr batch) override {
        write_count_.fetch_add(1);
        return Status::Ok();
    }

    int WriteCount() const { return write_count_.load(); }

private:
    std::atomic<int> write_count_{0};
};

// Test Driver implementation
class TestDriver : public FeatureDriver {
public:
    const char* Name() const override { return "test_feature"; }
    const char* DisplayName() const override { return "Test Feature"; }
    const char* Category() const override { return "test"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("test_feature");
        pipeline->SetSource(std::make_unique<MockSource>());
        pipeline->AddSink(std::make_unique<MockSink>());
        return pipeline;
    }
};

class TestDriverTier3 : public FeatureDriver {
public:
    const char* Name() const override { return "test_profiler"; }
    const char* DisplayName() const override { return "Test Profiler"; }
    const char* Category() const override { return "cpu"; }
    DriverTier Tier() const override { return DriverTier::kProfiling; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("test_profiler");
        pipeline->SetSource(std::make_unique<MockSource>());
        pipeline->AddSink(std::make_unique<MockSink>());
        return pipeline;
    }
};

class FeatureDriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean bus state from any prior test
        FeatureBus::Instance().Unregister("test_feature");
        FeatureBus::Instance().Unregister("test_profiler");
        InfrastructureManager::Instance().Start(
            {.collect_pool_threads = 2, .sink_pool_threads = 2});
    }

    void TearDown() override {
        FeatureBus::Instance().RemoveAll();
        FeatureBus::Instance().Unregister("test_feature");
        FeatureBus::Instance().Unregister("test_profiler");
        InfrastructureManager::Instance().Stop();
    }
};

TEST_F(FeatureDriverTest, ProbeAndRemove) {
    TestDriver driver;
    EXPECT_EQ(driver.State(), DriverState::kInactive);

    auto status = driver.Probe();
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(driver.State(), DriverState::kActive);
    EXPECT_NE(driver.GetPipeline(), nullptr);

    status = driver.Remove();
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(driver.State(), DriverState::kInactive);
    EXPECT_EQ(driver.GetPipeline(), nullptr);
}

TEST_F(FeatureDriverTest, DoubleProbeReturnsError) {
    TestDriver driver;
    EXPECT_TRUE(driver.Probe().ok());
    EXPECT_FALSE(driver.Probe().ok());
    driver.Remove();
}

TEST_F(FeatureDriverTest, PauseAndResume) {
    TestDriver driver;
    EXPECT_TRUE(driver.Probe().ok());

    EXPECT_TRUE(driver.Pause().ok());
    EXPECT_EQ(driver.State(), DriverState::kPaused);

    EXPECT_TRUE(driver.Resume().ok());
    EXPECT_EQ(driver.State(), DriverState::kActive);

    driver.Remove();
}

TEST_F(FeatureDriverTest, PauseWhenNotActiveReturnsError) {
    TestDriver driver;
    EXPECT_FALSE(driver.Pause().ok());
}

TEST_F(FeatureDriverTest, InfoReportsCorrectMetadata) {
    TestDriver driver;
    driver.Probe();
    auto info = driver.Info();

    EXPECT_EQ(info.name, "test_feature");
    EXPECT_EQ(info.display_name, "Test Feature");
    EXPECT_EQ(info.category, "test");
    EXPECT_EQ(info.tier, DriverTier::kMonitoring);
    EXPECT_EQ(info.state, DriverState::kActive);

    driver.Remove();
}

// FeatureBus tests

TEST_F(FeatureDriverTest, BusRegisterAndProbe) {
    auto& bus = FeatureBus::Instance();

    auto status = bus.Register(std::make_unique<TestDriver>());
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(bus.Size(), 1u);

    status = bus.Probe("test_feature");
    EXPECT_TRUE(status.ok());

    auto* drv = bus.GetDriver("test_feature");
    EXPECT_NE(drv, nullptr);
    EXPECT_EQ(drv->State(), DriverState::kActive);

    bus.Remove("test_feature");
}

TEST_F(FeatureDriverTest, BusDuplicateRegisterReturnsError) {
    auto& bus = FeatureBus::Instance();
    EXPECT_TRUE(bus.Register(std::make_unique<TestDriver>()).ok());
    EXPECT_FALSE(bus.Register(std::make_unique<TestDriver>()).ok());
}

TEST_F(FeatureDriverTest, BusProbeAllSkipsTier3) {
    auto& bus = FeatureBus::Instance();
    bus.Register(std::make_unique<TestDriver>());
    bus.Register(std::make_unique<TestDriverTier3>());

    bus.ProbeAll();

    EXPECT_EQ(bus.GetDriver("test_feature")->State(), DriverState::kActive);
    EXPECT_EQ(bus.GetDriver("test_profiler")->State(), DriverState::kInactive);
}

TEST_F(FeatureDriverTest, BusListDrivers) {
    auto& bus = FeatureBus::Instance();
    bus.Register(std::make_unique<TestDriver>());
    bus.Register(std::make_unique<TestDriverTier3>());

    auto infos = bus.ListDrivers();
    EXPECT_EQ(infos.size(), 2u);
}

// ========================================================================
// Push-mode Pause/Resume 测试
// ========================================================================

class MockPushSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_push"; }
    const char* Version() const override { return "1.0.0"; }
    bool IsPushMode() const override { return true; }

    Status Start() override {
        running_ = true;
        push_thread_ = std::thread([this] {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                if (running_ && !paused_ && callback_) {
                    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
                    batch->AddRecord();
                    callback_(std::move(batch));
                    push_count_.fetch_add(1);
                }
            }
        });
        return Status::Ok();
    }

    Status Stop() override {
        running_ = false;
        paused_ = false;
        if (push_thread_.joinable()) push_thread_.join();
        return Status::Ok();
    }

    Status PauseCollection() override {
        paused_ = true;
        pause_called_.fetch_add(1);
        return Status::Ok();
    }

    Status ResumeCollection() override {
        paused_ = false;
        resume_called_.fetch_add(1);
        return Status::Ok();
    }

    int PushCount() const { return push_count_.load(); }
    int PauseCalled() const { return pause_called_.load(); }
    int ResumeCalled() const { return resume_called_.load(); }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> push_count_{0};
    std::atomic<int> pause_called_{0};
    std::atomic<int> resume_called_{0};
    std::thread push_thread_;
};

class PushTestDriver : public FeatureDriver {
public:
    const char* Name() const override { return "push_test"; }
    const char* DisplayName() const override { return "Push Test"; }
    const char* Category() const override { return "test"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    MockPushSource* GetMockSource() { return mock_src_; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("push_test");
        auto src = std::make_unique<MockPushSource>();
        mock_src_ = src.get();
        pipeline->SetSource(std::move(src));
        pipeline->AddSink(std::make_unique<MockSink>());
        return pipeline;
    }

private:
    MockPushSource* mock_src_ = nullptr;
};

TEST_F(FeatureDriverTest, PushModePauseCallsPauseCollection) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    EXPECT_EQ(driver.GetMockSource()->PauseCalled(), 0);

    EXPECT_TRUE(driver.Pause().ok());
    EXPECT_EQ(driver.State(), DriverState::kPaused);
    EXPECT_EQ(driver.GetMockSource()->PauseCalled(), 1);

    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModeResumeCallsResumeCollection) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    ASSERT_TRUE(driver.Pause().ok());

    EXPECT_TRUE(driver.Resume().ok());
    EXPECT_EQ(driver.State(), DriverState::kActive);
    EXPECT_EQ(driver.GetMockSource()->ResumeCalled(), 1);

    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModePauseStopsDataFlow) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    int before_pause = driver.GetMockSource()->PushCount();
    EXPECT_GT(before_pause, 0);

    ASSERT_TRUE(driver.Pause().ok());

    int at_pause = driver.GetMockSource()->PushCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    int after_pause = driver.GetMockSource()->PushCount();

    EXPECT_EQ(at_pause, after_pause)
        << "Push source should not emit events while paused";

    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModePauseResumeFullCycle) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(driver.Pause().ok());
    int paused_count = driver.GetMockSource()->PushCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(driver.GetMockSource()->PushCount(), paused_count);

    ASSERT_TRUE(driver.Resume().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(driver.GetMockSource()->PushCount(), paused_count)
        << "Push source should resume emitting after resume";

    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModePauseTwiceReturnsError) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    EXPECT_TRUE(driver.Pause().ok());
    EXPECT_FALSE(driver.Pause().ok());
    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModeResumeWithoutPauseReturnsError) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    EXPECT_FALSE(driver.Resume().ok());
    driver.Remove();
}

TEST_F(FeatureDriverTest, PushModeRemoveWhilePaused) {
    PushTestDriver driver;
    FeatureBus::Instance().Unregister("push_test");

    ASSERT_TRUE(driver.Probe().ok());
    ASSERT_TRUE(driver.Pause().ok());
    EXPECT_TRUE(driver.Remove().ok());
    EXPECT_EQ(driver.State(), DriverState::kInactive);
}

// Pull-mode 回归测试：确认 PauseCollection 也被调用但是 no-op
TEST_F(FeatureDriverTest, PullModePauseStillCallsPauseCollection) {
    TestDriver driver;
    ASSERT_TRUE(driver.Probe().ok());
    EXPECT_TRUE(driver.Pause().ok());
    EXPECT_EQ(driver.State(), DriverState::kPaused);
    EXPECT_TRUE(driver.Resume().ok());
    EXPECT_EQ(driver.State(), DriverState::kActive);
    driver.Remove();
}

TEST_F(FeatureDriverTest, BusStateChangeCallback) {
    auto& bus = FeatureBus::Instance();
    std::string last_feature;
    DriverState last_from, last_to;

    bus.SetStateChangeCallback([&](const std::string& f, DriverState from, DriverState to) {
        last_feature = f;
        last_from = from;
        last_to = to;
    });

    bus.Register(std::make_unique<TestDriver>());
    bus.Probe("test_feature");

    EXPECT_EQ(last_feature, "test_feature");
    EXPECT_EQ(last_from, DriverState::kInactive);
    EXPECT_EQ(last_to, DriverState::kActive);
}

}  // namespace
}  // namespace illuminator
