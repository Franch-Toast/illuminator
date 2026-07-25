// recording_lifecycle_test.cc — 录制生命周期单元测试
// 验证 FeatureDriver 的 StartRecording / StopRecording / IsRecording 语义。
//
// 测试用例：
//   1. StartRecording 在 active 状态成功
//   2. StartRecording 在 inactive 状态失败
//   3. StopRecording 正确清理
//   4. StartRecording 重复调用返回 kAlreadyExists
//   5. Remove() 自动清理录制
//   6. IsRecording() 状态正确

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "core/common/status.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"
#include "plugin/api/sink_plugin.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/recording_interface.h"
#include "plugin/infra/feature_driver_factories.h"

namespace illuminator {
namespace {

// ===========================================================================
// Mock 组件
// ===========================================================================

// SimpleMockSource — 生成简单数据，支持 Pull 模式。
class SimpleMockSource : public SourcePlugin {
public:
    const char* Name() const override { return "simple_mock_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        collect_count_.fetch_add(1, std::memory_order_relaxed);
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();
        rec.SetField(batch->InternString("value"), double{42.0});
        return batch;
    }

    uint64_t CollectCount() const { return collect_count_.load(); }

private:
    std::atomic<uint64_t> collect_count_{0};
};

// CountingSink — 记录 Write 次数。
class CountingSink : public SinkPlugin {
public:
    const char* Name() const override { return "counting_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Write(ConstDataBatchPtr batch) override {
        if (batch) write_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }

private:
    std::atomic<uint64_t> write_count_{0};
};

// ===========================================================================
// TestRecordingDriver — 用于测试 FeatureDriver 录制接口
// ===========================================================================
class TestRecordingDriver : public FeatureDriver {
public:
    const char* Name() const override { return "test_recording_feature"; }
    const char* DisplayName() const override { return "Test Recording Feature"; }
    const char* Category() const override { return "test"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipe = std::make_unique<Pipeline>("test_recording_pipe");
        pipe->SetSource(std::make_unique<SimpleMockSource>());
        pipe->AddSink(std::make_unique<CountingSink>());
        return pipe;
    }
};

// ===========================================================================
// 测试 Fixture
// ===========================================================================
class RecordingLifecycleTest : public ::testing::Test {
protected:
    static constexpr const char* kTestOutputDir = "/tmp/illuminator_test_recordings";

    void SetUp() override {
        auto& infra = InfrastructureManager::Instance();
        if (!infra.IsStarted()) {
            infra.Start({.collect_pool_threads = 1, .sink_pool_threads = 2});
        }
        // 清理注册表中的残留
        RecordingSinkRegistry::Instance().Unregister("test_recording_feature");
        FeatureDriver::SetRecordingSinkFactory(
            [](const std::string& feature, const std::string& dir) {
                return MakeFeatureRecordingSinkPair(feature, dir);
            });
    }

    void TearDown() override {
        RecordingSinkRegistry::Instance().Unregister("test_recording_feature");
        auto& infra = InfrastructureManager::Instance();
        if (infra.IsStarted()) {
            infra.Stop();
        }
    }
};

// ===========================================================================
// 1. StartRecording 在 active 状态成功
// ===========================================================================
TEST_F(RecordingLifecycleTest, StartRecordingSucceedsWhenActive) {
    TestRecordingDriver drv;

    ASSERT_TRUE(drv.Probe().ok());
    EXPECT_EQ(drv.State(), DriverState::kActive);

    auto status = drv.StartRecording(kTestOutputDir);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_TRUE(drv.IsRecording());

    // 验证 RecordingSink 已注册
    auto rec = RecordingSinkRegistry::Instance().Get("test_recording_feature");
    EXPECT_NE(rec, nullptr);

    // 等待数据流入录制
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 停止录制
    EXPECT_TRUE(drv.StopRecording().ok());

    drv.Remove();
}

// ===========================================================================
// 2. StartRecording 在 inactive 状态失败
// ===========================================================================
TEST_F(RecordingLifecycleTest, StartRecordingFailsWhenInactive) {
    TestRecordingDriver drv;
    // 不调用 Probe()，driver 处于 inactive 状态
    EXPECT_EQ(drv.State(), DriverState::kInactive);

    auto status = drv.StartRecording(kTestOutputDir);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);

    // 确认没有注册
    EXPECT_FALSE(drv.IsRecording());
    auto rec = RecordingSinkRegistry::Instance().Get("test_recording_feature");
    EXPECT_EQ(rec, nullptr);
}

// 也测试 Paused 状态下 StartRecording
TEST_F(RecordingLifecycleTest, StartRecordingSucceedsWhenPaused) {
    TestRecordingDriver drv;

    ASSERT_TRUE(drv.Probe().ok());
    EXPECT_TRUE(drv.Pause().ok());
    EXPECT_EQ(drv.State(), DriverState::kPaused);

    // Paused 状态下 pipeline_ 仍然存在，StartRecording 应该可以工作
    auto status = drv.StartRecording(kTestOutputDir);
    // 根据 FeatureDriver::StartRecording 的实现，只要 state_ != kInactive 且 pipeline_ 存在即可
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_TRUE(drv.IsRecording());

    EXPECT_TRUE(drv.StopRecording().ok());
    drv.Remove();
}

// ===========================================================================
// 3. StopRecording 正确清理
// ===========================================================================
TEST_F(RecordingLifecycleTest, StopRecordingCleansUpProperly) {
    TestRecordingDriver drv;

    ASSERT_TRUE(drv.Probe().ok());
    ASSERT_TRUE(drv.StartRecording(kTestOutputDir).ok());
    EXPECT_TRUE(drv.IsRecording());

    // 验证注册表中存在
    EXPECT_NE(RecordingSinkRegistry::Instance().Get("test_recording_feature"), nullptr);

    // 停止录制
    auto status = drv.StopRecording();
    EXPECT_TRUE(status.ok());

    // 验证清理
    EXPECT_FALSE(drv.IsRecording());
    EXPECT_EQ(RecordingSinkRegistry::Instance().Get("test_recording_feature"), nullptr);

    // Pipeline 仍然在运行
    EXPECT_EQ(drv.State(), DriverState::kActive);
    EXPECT_NE(drv.GetPipeline(), nullptr);

    drv.Remove();
}

// StopRecording 在未录制时返回 kNotFound
TEST_F(RecordingLifecycleTest, StopRecordingWhenNotRecordingReturnsNotFound) {
    TestRecordingDriver drv;
    ASSERT_TRUE(drv.Probe().ok());

    auto status = drv.StopRecording();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kNotFound);

    drv.Remove();
}

// ===========================================================================
// 4. StartRecording 重复调用返回 kAlreadyExists
// ===========================================================================
TEST_F(RecordingLifecycleTest, DuplicateStartRecordingReturnsAlreadyExists) {
    TestRecordingDriver drv;

    ASSERT_TRUE(drv.Probe().ok());

    // 第一次 StartRecording 成功
    auto status1 = drv.StartRecording(kTestOutputDir);
    EXPECT_TRUE(status1.ok());
    EXPECT_TRUE(drv.IsRecording());

    // 第二次 StartRecording 应返回 kAlreadyExists
    auto status2 = drv.StartRecording(kTestOutputDir);
    EXPECT_FALSE(status2.ok());
    EXPECT_EQ(status2.code(), StatusCode::kAlreadyExists);

    // 仍然在录制状态
    EXPECT_TRUE(drv.IsRecording());

    // 清理
    EXPECT_TRUE(drv.StopRecording().ok());
    drv.Remove();
}

// ===========================================================================
// 5. Remove() 自动清理录制
// ===========================================================================
TEST_F(RecordingLifecycleTest, RemoveAutoCleansRecording) {
    TestRecordingDriver drv;

    ASSERT_TRUE(drv.Probe().ok());
    ASSERT_TRUE(drv.StartRecording(kTestOutputDir).ok());
    EXPECT_TRUE(drv.IsRecording());

    // 等待一些数据流入
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Remove 应该自动停止录制
    auto status = drv.Remove();
    EXPECT_TRUE(status.ok());

    // 验证清理
    EXPECT_EQ(drv.State(), DriverState::kInactive);
    EXPECT_FALSE(drv.IsRecording());
    EXPECT_EQ(drv.GetPipeline(), nullptr);

    // 注册表中应该没有残留
    EXPECT_EQ(RecordingSinkRegistry::Instance().Get("test_recording_feature"), nullptr);
}

// ===========================================================================
// 6. IsRecording() 状态正确
// ===========================================================================
TEST_F(RecordingLifecycleTest, IsRecordingStateTransitions) {
    TestRecordingDriver drv;

    // 初始状态：未录制
    EXPECT_FALSE(drv.IsRecording());

    ASSERT_TRUE(drv.Probe().ok());

    // Probe 后仍未录制
    EXPECT_FALSE(drv.IsRecording());

    // StartRecording 后：正在录制
    ASSERT_TRUE(drv.StartRecording(kTestOutputDir).ok());
    EXPECT_TRUE(drv.IsRecording());

    // StopRecording 后：未录制
    ASSERT_TRUE(drv.StopRecording().ok());
    EXPECT_FALSE(drv.IsRecording());

    // 再次 StartRecording：可以重新开始
    ASSERT_TRUE(drv.StartRecording(kTestOutputDir).ok());
    EXPECT_TRUE(drv.IsRecording());

    // Remove 后：未录制
    drv.Remove();
    EXPECT_FALSE(drv.IsRecording());
}

// 多次 Start/Stop 循环
TEST_F(RecordingLifecycleTest, MultipleStartStopCycles) {
    TestRecordingDriver drv;
    ASSERT_TRUE(drv.Probe().ok());

    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(drv.IsRecording());

        auto start_status = drv.StartRecording(kTestOutputDir);
        EXPECT_TRUE(start_status.ok()) << "Cycle " << i << ": " << start_status.message();
        EXPECT_TRUE(drv.IsRecording());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto stop_status = drv.StopRecording();
        EXPECT_TRUE(stop_status.ok()) << "Cycle " << i << ": " << stop_status.message();
        EXPECT_FALSE(drv.IsRecording());
    }

    drv.Remove();
}

}  // namespace
}  // namespace illuminator
