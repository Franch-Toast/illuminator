// FeatureBus + FeatureDriver 单元测试

#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

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
