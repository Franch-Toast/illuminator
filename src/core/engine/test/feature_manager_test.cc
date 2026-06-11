#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "core/engine/feature_manager.h"
#include "plugin/builtin/builtin_plugins.h"

namespace illuminator {
namespace {

class FeatureManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        RegisterBuiltinPlugins();
        controller_.InitSinkPool(1);
        controller_.InitCollectPool(1);
    }

    PipelineController controller_;
};

TEST_F(FeatureManagerTest, RegisterAndListFeatures) {
    FeatureManager fm(controller_);

    FeatureConfig fc1;
    fc1.name = "test_cpu";
    fc1.display_name = "Test CPU";
    fc1.category = "cpu";
    fc1.pipeline.name = "test_cpu";
    fc1.pipeline.source.type = "cpu_utilization";
    fc1.pipeline.source.config.Set("interval_ms", int64_t{500});

    fm.RegisterFeature(std::move(fc1));

    auto list = fm.ListFeatures();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "test_cpu");
    EXPECT_EQ(list[0].display_name, "Test CPU");
    EXPECT_EQ(list[0].category, "cpu");
    EXPECT_EQ(list[0].state, FeatureState::kInactive);
}

TEST_F(FeatureManagerTest, StartAndStopFeature) {
    FeatureManager fm(controller_);

    FeatureConfig fc;
    fc.name = "test_feature";
    fc.display_name = "Test Feature";
    fc.category = "cpu";
    fc.pipeline.name = "test_feature";
    fc.pipeline.source.type = "cpu_utilization";
    fc.pipeline.source.config.Set("interval_ms", int64_t{500});
    fc.pipeline.source.config.Set("collect_per_core", false);

    fm.RegisterFeature(std::move(fc));

    EXPECT_EQ(fm.GetState("test_feature"), FeatureState::kInactive);

    auto status = fm.Start("test_feature");
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(fm.GetState("test_feature"), FeatureState::kActive);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto list = fm.ListFeatures();
    EXPECT_EQ(list[0].state, FeatureState::kActive);
    EXPECT_GE(list[0].uptime_ms, 1u);

    status = fm.Stop("test_feature");
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(fm.GetState("test_feature"), FeatureState::kInactive);
}

TEST_F(FeatureManagerTest, PauseAndResumeFeature) {
    FeatureManager fm(controller_);

    FeatureConfig fc;
    fc.name = "pause_test";
    fc.display_name = "Pause Test";
    fc.category = "cpu";
    fc.pipeline.name = "pause_test";
    fc.pipeline.source.type = "cpu_utilization";
    fc.pipeline.source.config.Set("interval_ms", int64_t{500});
    fc.pipeline.source.config.Set("collect_per_core", false);

    fm.RegisterFeature(std::move(fc));
    fm.Start("pause_test");

    auto status = fm.Pause("pause_test");
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(fm.GetState("pause_test"), FeatureState::kPaused);

    status = fm.Resume("pause_test");
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(fm.GetState("pause_test"), FeatureState::kActive);

    fm.Stop("pause_test");
}

TEST_F(FeatureManagerTest, StartNonExistentFeatureReturnsError) {
    FeatureManager fm(controller_);

    auto status = fm.Start("nonexistent");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kNotFound);
}

TEST_F(FeatureManagerTest, StopAllStopsAllActive) {
    FeatureManager fm(controller_);

    for (int i = 0; i < 3; ++i) {
        FeatureConfig fc;
        fc.name = "multi_" + std::to_string(i);
        fc.display_name = "Multi " + std::to_string(i);
        fc.category = "cpu";
        fc.pipeline.name = fc.name;
        fc.pipeline.source.type = "cpu_utilization";
        fc.pipeline.source.config.Set("interval_ms", int64_t{500});
        fc.pipeline.source.config.Set("collect_per_core", false);
        fm.RegisterFeature(std::move(fc));
        fm.Start("multi_" + std::to_string(i));
    }

    fm.StopAll();

    auto list = fm.ListFeatures();
    for (const auto& f : list) {
        EXPECT_EQ(f.state, FeatureState::kInactive);
    }
}

TEST_F(FeatureManagerTest, StateChangeCallbackFires) {
    FeatureManager fm(controller_);

    std::vector<std::pair<FeatureState, FeatureState>> transitions;
    fm.SetStateChangeCallback(
        [&transitions](const std::string&, FeatureState from, FeatureState to) {
            transitions.emplace_back(from, to);
        });

    FeatureConfig fc;
    fc.name = "cb_test";
    fc.display_name = "Callback Test";
    fc.category = "cpu";
    fc.pipeline.name = "cb_test";
    fc.pipeline.source.type = "cpu_utilization";
    fc.pipeline.source.config.Set("interval_ms", int64_t{500});
    fc.pipeline.source.config.Set("collect_per_core", false);
    fm.RegisterFeature(std::move(fc));

    fm.Start("cb_test");
    fm.Stop("cb_test");

    ASSERT_GE(transitions.size(), 4u);
    EXPECT_EQ(transitions[0].first, FeatureState::kInactive);
    EXPECT_EQ(transitions[0].second, FeatureState::kStarting);
}

}  // namespace
}  // namespace illuminator
