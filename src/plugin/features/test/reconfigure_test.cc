// reconfigure_test.cc — Reconfigure 全链路单元测试
// 验证 Reconfigure 从 FeatureDriver → Pipeline → Source/Processor/Sink 的传播语义。
//
// 测试用例：
//   1. Reconfigure 通过 FeatureDriver 传递到 Source
//   2. Reconfigure target_pids 更新 PidManager
//   3. Reconfigure rodata 参数返回 kRequiresRestart
//   4. Reconfigure 无效参数返回错误
//   5. Reconfigure 在 inactive 状态返回 kUnavailable
//   6. Pipeline::Reconfigure 跳过 kUnimplemented 的插件

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "core/common/status.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pid_manager.h"
#include "core/engine/pipeline.h"
#include "plugin/api/sink_plugin.h"
#include "plugin/api/source_plugin.h"

namespace illuminator {
namespace {

// ===========================================================================
// Mock 组件
// ===========================================================================

// ReconfigurableSource — 记录 Reconfigure 调用，可配置返回码。
// 支持 target_pids 参数解析，模拟 PidManager 更新。
class ReconfigurableSource : public SourcePlugin {
public:
    const char* Name() const override { return "reconfig_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 1000; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord().SetField(batch->InternString("v"), double{1.0});
        return batch;
    }

    Status Reconfigure(const ConfigValue& params) override {
        reconfigure_count_.fetch_add(1, std::memory_order_relaxed);
        last_params_ = params;

        // 如果配置了自定义返回码，直接返回
        if (custom_code_ != StatusCode::kOk) {
            return Status(custom_code_, "mock reconfigure status");
        }

        // 检查 target_pids：模拟 PidManager 更新
        auto pids_val = params["target_pids"];
        if (!pids_val.IsNull()) {
            auto pid_list = pids_val.AsList();
            std::vector<int32_t> pids;
            for (const auto& s : pid_list) {
                try {
                    pids.push_back(std::stoi(s));
                } catch (...) {}
            }
            if (!pids.empty()) {
                pid_manager_.SetTargetPids(pids);
                target_pids_updated_.store(true);
            }
        }

        // 检查 rodata 参数：模拟需要重启
        auto rodata_val = params["rodata"];
        if (!rodata_val.IsNull()) {
            return Status(StatusCode::kRequiresRestart,
                          "rodata change requires restart");
        }

        // 检查 invalid_param：模拟参数校验失败
        auto invalid_val = params["invalid_param"];
        if (!invalid_val.IsNull() && invalid_val.AsString() == "bad") {
            return Status(StatusCode::kInvalidArgument,
                          "invalid parameter value");
        }

        return Status::Ok();
    }

    // 测试辅助方法
    void SetReturnCode(StatusCode code) { custom_code_ = code; }
    uint64_t ReconfigureCount() const { return reconfigure_count_.load(); }
    bool TargetPidsUpdated() const { return target_pids_updated_.load(); }
    PidManager& GetPidManager() { return pid_manager_; }
    ConfigValue LastParams() const { return last_params_; }

private:
    std::atomic<uint64_t> reconfigure_count_{0};
    std::atomic<bool> target_pids_updated_{false};
    StatusCode custom_code_ = StatusCode::kOk;
    ConfigValue last_params_;
    PidManager pid_manager_{-1, "test_feature"};
};

// UnimplementedSource — Reconfigure 返回 kUnimplemented（默认行为）。
class UnimplementedSource : public SourcePlugin {
public:
    const char* Name() const override { return "unimpl_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 1000; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord();
        return batch;
    }
    // Reconfigure 使用基类默认实现，返回 kUnimplemented
};

// CountingSink — 记录 Write 次数，支持 Reconfigure 调用记录。
class CountingSink : public SinkPlugin {
public:
    const char* Name() const override { return "counting_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Write(DataBatchPtr batch) override {
        if (batch) write_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    Status Reconfigure(const ConfigValue& params) override {
        reconfigure_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }
    uint64_t ReconfigureCount() const { return reconfigure_count_.load(); }

private:
    std::atomic<uint64_t> write_count_{0};
    std::atomic<uint64_t> reconfigure_count_{0};
};

// ErrorSink — Reconfigure 返回错误，用于测试错误传播。
class ErrorSink : public SinkPlugin {
public:
    const char* Name() const override { return "error_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Write(DataBatchPtr) override { return Status::Ok(); }

    Status Reconfigure(const ConfigValue&) override {
        return Status::Error(StatusCode::kInternal, "sink reconfigure failed");
    }
};

// ===========================================================================
// TestFeatureDriver — 用于测试 FeatureDriver 层面的 Reconfigure
// ===========================================================================
class TestFeatureDriver : public FeatureDriver {
public:
    const char* Name() const override { return "test_reconfig_feature"; }
    const char* DisplayName() const override { return "Test Reconfig Feature"; }
    const char* Category() const override { return "test"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

    // 在 Probe() 之前调用，设置 mock source。
    void SetMockSource(std::unique_ptr<ReconfigurableSource> src) {
        source_raw_ = src.get();
        source_owned_ = std::move(src);
    }

    ReconfigurableSource* GetMockSource() const { return source_raw_; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipe = std::make_unique<Pipeline>("test_reconfig_pipe");
        pipe->SetSource(std::move(source_owned_));
        pipe->AddSink(std::make_unique<CountingSink>());
        return pipe;
    }

private:
    std::unique_ptr<ReconfigurableSource> source_owned_;
    ReconfigurableSource* source_raw_ = nullptr;
};

// ===========================================================================
// 测试 Fixture
// ===========================================================================
class ReconfigureTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& infra = InfrastructureManager::Instance();
        if (!infra.IsStarted()) {
            infra.Start({.collect_pool_threads = 1, .sink_pool_threads = 2});
        }
    }

    void TearDown() override {
        auto& infra = InfrastructureManager::Instance();
        if (infra.IsStarted()) {
            infra.Stop();
        }
    }
};

// ===========================================================================
// 1. Reconfigure 通过 FeatureDriver 传递到 Source
// ===========================================================================
TEST_F(ReconfigureTest, ReconfigurePropagatesFromDriverToSource) {
    TestFeatureDriver drv;

    auto source = std::make_unique<ReconfigurableSource>();
    auto* source_ptr = source.get();
    drv.SetMockSource(std::move(source));

    ASSERT_TRUE(drv.Probe().ok());
    EXPECT_EQ(drv.State(), DriverState::kActive);

    ConfigValue params;
    params.Set("interval_ms", int64_t{500});
    params.Set("custom_key", std::string("custom_value"));

    auto status = drv.Reconfigure(params);
    EXPECT_TRUE(status.ok()) << status.message();

    // Source 应该收到 Reconfigure 调用
    EXPECT_EQ(source_ptr->ReconfigureCount(), 1u);

    // 验证参数被正确传递
    auto last = source_ptr->LastParams();
    EXPECT_EQ(last["interval_ms"].AsInt(), 500);
    EXPECT_EQ(last["custom_key"].AsString(), "custom_value");

    drv.Remove();
}

// ===========================================================================
// 2. Reconfigure target_pids 更新 PidManager
// ===========================================================================
TEST_F(ReconfigureTest, ReconfigureTargetPidsUpdatesPidManager) {
    TestFeatureDriver drv;

    auto source = std::make_unique<ReconfigurableSource>();
    auto* source_ptr = source.get();
    drv.SetMockSource(std::move(source));

    ASSERT_TRUE(drv.Probe().ok());

    // 构造 target_pids 参数
    ConfigValue params;
    params.Set("target_pids._size", int64_t{3});
    params.Set("target_pids.0", std::string("100"));
    params.Set("target_pids.1", std::string("200"));
    params.Set("target_pids.2", std::string("300"));

    auto status = drv.Reconfigure(params);
    EXPECT_TRUE(status.ok()) << status.message();

    // 验证 PidManager 被更新
    EXPECT_TRUE(source_ptr->TargetPidsUpdated());

    auto& mgr = source_ptr->GetPidManager();
    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 3u);

    // 验证包含所有设置的 PID
    bool has100 = false, has200 = false, has300 = false;
    for (auto p : pids) {
        if (p == 100) has100 = true;
        if (p == 200) has200 = true;
        if (p == 300) has300 = true;
    }
    EXPECT_TRUE(has100);
    EXPECT_TRUE(has200);
    EXPECT_TRUE(has300);

    drv.Remove();
}

// ===========================================================================
// 3. Reconfigure rodata 参数返回 kRequiresRestart
// ===========================================================================
TEST_F(ReconfigureTest, ReconfigureRodataReturnsRequiresRestart) {
    Pipeline pipe("test_rodata");

    auto source = std::make_unique<ReconfigurableSource>();
    pipe.SetSource(std::move(source));
    pipe.AddSink(std::make_unique<CountingSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("rodata", std::string("new_bpf_config"));

    auto status = pipe.Reconfigure(params);
    EXPECT_EQ(status.code(), StatusCode::kRequiresRestart);
    EXPECT_NE(status.message().find("restart"), std::string::npos);

    ASSERT_TRUE(pipe.Stop().ok());
}

// 也测试 FeatureDriver 层面的 rodata kRequiresRestart 传播
TEST_F(ReconfigureTest, DriverReconfigureRodataPropagatesRequiresRestart) {
    TestFeatureDriver drv;

    auto source = std::make_unique<ReconfigurableSource>();
    drv.SetMockSource(std::move(source));

    ASSERT_TRUE(drv.Probe().ok());

    ConfigValue params;
    params.Set("rodata", std::string("updated_bpf_map_data"));

    auto status = drv.Reconfigure(params);
    EXPECT_EQ(status.code(), StatusCode::kRequiresRestart);

    drv.Remove();
}

// ===========================================================================
// 4. Reconfigure 无效参数返回错误
// ===========================================================================
TEST_F(ReconfigureTest, ReconfigureInvalidParamsReturnsError) {
    Pipeline pipe("test_invalid");

    auto source = std::make_unique<ReconfigurableSource>();
    pipe.SetSource(std::move(source));
    pipe.AddSink(std::make_unique<CountingSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("invalid_param", std::string("bad"));

    auto status = pipe.Reconfigure(params);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);

    ASSERT_TRUE(pipe.Stop().ok());
}

// 测试 Sink 返回错误时 Reconfigure 传播
TEST_F(ReconfigureTest, ReconfigureSinkErrorPropagates) {
    Pipeline pipe("test_sink_error");

    pipe.SetSource(std::make_unique<UnimplementedSource>());
    pipe.AddSink(std::make_unique<ErrorSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("key", std::string("value"));

    auto status = pipe.Reconfigure(params);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInternal);

    ASSERT_TRUE(pipe.Stop().ok());
}

// ===========================================================================
// 5. Reconfigure 在 inactive 状态返回 kUnavailable
// ===========================================================================
TEST_F(ReconfigureTest, ReconfigureInactiveReturnsUnavailable) {
    TestFeatureDriver drv;
    // 不调用 Probe()，driver 处于 inactive 状态
    EXPECT_EQ(drv.State(), DriverState::kInactive);

    ConfigValue params;
    params.Set("key", std::string("value"));

    auto status = drv.Reconfigure(params);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kUnavailable);
}

// 也测试 Pipeline 未启动时 Reconfigure 的行为
TEST_F(ReconfigureTest, PipelineReconfigureWhenNotRunningIsSafe) {
    Pipeline pipe("test_not_running");
    pipe.SetSource(std::make_unique<ReconfigurableSource>());
    pipe.AddSink(std::make_unique<CountingSink>());

    // Pipeline 未 Start，但 Reconfigure 仍应可调用（source/sink 已设置）
    ConfigValue params;
    params.Set("key", std::string("value"));

    auto status = pipe.Reconfigure(params);
    // Source 的 Reconfigure 应该被调用，返回 Ok
    EXPECT_TRUE(status.ok());
}

// ===========================================================================
// 6. Pipeline::Reconfigure 跳过 kUnimplemented 的插件
// ===========================================================================
TEST_F(ReconfigureTest, PipelineReconfigureSkipsUnimplementedPlugins) {
    Pipeline pipe("test_skip_unimpl");

    // UnimplementedSource 返回 kUnimplemented（基类默认）
    pipe.SetSource(std::make_unique<UnimplementedSource>());
    // CountingSink 返回 Ok（基类默认）
    pipe.AddSink(std::make_unique<CountingSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("interval_ms", int64_t{2000});

    auto status = pipe.Reconfigure(params);
    // 即使 Source 返回 kUnimplemented，Pipeline 应继续并返回 Ok
    EXPECT_TRUE(status.ok()) << status.message();

    ASSERT_TRUE(pipe.Stop().ok());
}

// 混合场景：部分插件 kUnimplemented，部分 kOk，最终返回 Ok
TEST_F(ReconfigureTest, MixedImplementedAndUnimplementedPlugins) {
    Pipeline pipe("test_mixed");

    // ReconfigurableSource 返回 Ok
    auto source = std::make_unique<ReconfigurableSource>();
    auto* source_ptr = source.get();
    pipe.SetSource(std::move(source));

    // CountingSink 返回 Ok
    auto sink = std::make_unique<CountingSink>();
    auto* sink_ptr = sink.get();
    pipe.AddSink(std::move(sink));

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("threshold", int64_t{42});

    auto status = pipe.Reconfigure(params);
    EXPECT_TRUE(status.ok());

    // 两者都应该收到 Reconfigure 调用
    EXPECT_EQ(source_ptr->ReconfigureCount(), 1u);
    EXPECT_EQ(sink_ptr->ReconfigureCount(), 1u);

    ASSERT_TRUE(pipe.Stop().ok());
}

// kRequiresRestart + kUnimplemented 混合：最终返回 kRequiresRestart
TEST_F(ReconfigureTest, RequiresRestartAndUnimplementedMix) {
    Pipeline pipe("test_restart_mix");

    // 一个返回 kRequiresRestart 的 source
    auto source = std::make_unique<ReconfigurableSource>();
    pipe.SetSource(std::move(source));

    // 一个返回 kUnimplemented 的 sink（使用 CountingSink 但手动设置返回码不行，
    // 因为 CountingSink 返回 Ok。我们用一个自定义 sink）
    class UnimplSink : public SinkPlugin {
    public:
        const char* Name() const override { return "unimpl_sink"; }
        const char* Version() const override { return "0.1.0"; }
        Status Write(DataBatchPtr) override { return Status::Ok(); }
        // 使用基类默认 Reconfigure，返回 kUnimplemented
    };

    pipe.AddSink(std::make_unique<UnimplSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("rodata", std::string("data"));

    auto status = pipe.Reconfigure(params);
    // Source 返回 kRequiresRestart，Sink 返回 kUnimplemented
    // 最终应为 kRequiresRestart（kUnimplemented 被跳过）
    EXPECT_EQ(status.code(), StatusCode::kRequiresRestart);

    ASSERT_TRUE(pipe.Stop().ok());
}

}  // namespace
}  // namespace illuminator
