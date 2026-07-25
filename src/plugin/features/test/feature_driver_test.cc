// FeatureDriver 集成测试 — 验证各 Driver 正确构建 Pipeline

#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"
#include "plugin/features/cpu/cpu_utilization/cpu_utilization_driver.h"
#include "plugin/infra/feature_registry.h"
#include <gtest/gtest.h>

namespace illuminator {
namespace {

class NoOpSink : public SinkPlugin {
public:
    const char* Name() const override { return "noop"; }
    const char* Version() const override { return "1.0"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    Status Write(ConstDataBatchPtr) override { return Status::Ok(); }
};

class FeatureDriverIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        FeatureDriver::SetSseSinkFactory([](const char*) -> std::unique_ptr<SinkPlugin> {
            return std::make_unique<NoOpSink>();
        });
        InfrastructureManager::Instance().Start(
            {.collect_pool_threads = 2, .sink_pool_threads = 2});
    }

    void TearDown() override {
        FeatureBus::Instance().RemoveAll();
        InfrastructureManager::Instance().Stop();
    }
};

TEST_F(FeatureDriverIntegrationTest, CpuUtilizationDriverMetadata) {
    CpuUtilizationDriver drv;
    EXPECT_STREQ(drv.Name(), "cpu_utilization");
    EXPECT_STREQ(drv.DisplayName(), "CPU Utilization");
    EXPECT_STREQ(drv.Category(), "cpu");
    EXPECT_EQ(drv.Tier(), DriverTier::kMonitoring);
}

TEST_F(FeatureDriverIntegrationTest, CpuUtilizationDriverBuildsPipeline) {
    CpuUtilizationDriver drv;
    auto status = drv.Probe();
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_NE(drv.GetPipeline(), nullptr);
    EXPECT_EQ(drv.State(), DriverState::kActive);

    auto info = drv.Info();
    EXPECT_EQ(info.name, "cpu_utilization");
    EXPECT_EQ(info.state, DriverState::kActive);

    drv.Remove();
}

TEST_F(FeatureDriverIntegrationTest, CpuUtilizationDriverConfigSchema) {
    CpuUtilizationDriver drv;
    auto schema = drv.ConfigSchema();
    EXPECT_FALSE(schema.empty());
    EXPECT_NE(schema.find("interval_ms"), std::string::npos);
    EXPECT_NE(schema.find("collect_per_core"), std::string::npos);
}

TEST_F(FeatureDriverIntegrationTest, FeatureRegistryRegistersDrivers) {
    auto& registry = FeatureRegistry::Instance();
    // The CpuUtilizationDriver auto-registers via REGISTER_FEATURE macro.
    // After calling RegisterAll, it should be in the FeatureBus.
    FeatureRegistry::RegisterAll();
    auto* drv = FeatureBus::Instance().GetDriver("cpu_utilization");
    EXPECT_NE(drv, nullptr);
}

TEST_F(FeatureDriverIntegrationTest, DriverLifecycleFullCycle) {
    CpuUtilizationDriver drv;
    EXPECT_EQ(drv.State(), DriverState::kInactive);

    EXPECT_TRUE(drv.Probe().ok());
    EXPECT_EQ(drv.State(), DriverState::kActive);

    EXPECT_TRUE(drv.Pause().ok());
    EXPECT_EQ(drv.State(), DriverState::kPaused);

    EXPECT_TRUE(drv.Resume().ok());
    EXPECT_EQ(drv.State(), DriverState::kActive);

    EXPECT_TRUE(drv.Remove().ok());
    EXPECT_EQ(drv.State(), DriverState::kInactive);
}

}  // namespace
}  // namespace illuminator
