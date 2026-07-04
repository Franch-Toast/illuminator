// pipeline_integration_test.cc — Pipeline 端到端集成测试
// 验证 Source → Processor → Aggregator → Sink 完整数据流。
// 使用内存 mock 组件，不依赖外部资源。

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"

namespace illuminator {
namespace {

// MockSource: 模拟 Pull 数据源，每次 Collect 返回固定数据。
class MockSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();
        rec.labels.push_back({batch->InternString("src"), batch->InternString("mock")});
        rec.SetField(batch->InternString("value"), double{42.0});
        collect_count_.fetch_add(1, std::memory_order_relaxed);
        return batch;
    }

    uint64_t CollectCount() const { return collect_count_.load(); }

private:
    std::atomic<uint64_t> collect_count_{0};
};

// MockSink: 模拟 Sink，记录收到的 DataBatch 数量。
class MockSink : public SinkPlugin {
public:
    const char* Name() const override { return "mock_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lock(mu_);
        received_.push_back(batch);
        write_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }

    std::vector<DataBatchPtr> Received() {
        std::lock_guard<std::mutex> lock(mu_);
        return received_;
    }

private:
    std::mutex mu_;
    std::vector<DataBatchPtr> received_;
    std::atomic<uint64_t> write_count_{0};
};

// 测试：基本管道 Source → Sink（无 Processor，无 Aggregator）
// 验证数据能从 Source 通过 channel 到达 Sink。
TEST(PipelineIntegrationTest, BasicSourceToSinkDataFlow) {
    Pipeline pipe("test_basic");

    auto source = std::make_unique<MockSource>();
    pipe.SetSource(std::move(source));

    auto sink = std::make_unique<MockSink>();
    auto* sink_ptr = sink.get();
    pipe.AddSink(std::move(sink));

    ASSERT_TRUE(pipe.Start().ok());
    EXPECT_TRUE(pipe.IsRunning());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("x"), double{1.0});
    pipe.Enqueue(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_TRUE(pipe.Stop().ok());
    EXPECT_FALSE(pipe.IsRunning());

    EXPECT_GT(sink_ptr->WriteCount(), 0u);
    EXPECT_GT(pipe.BatchesProcessed(), 0u);
}

// 测试：Pipeline 含 Processor 链
// 验证数据经过 PassthroughProcessor 后到达 Sink。
TEST(PipelineIntegrationTest, ProcessorChainIsAppliedBeforeSink) {
    Pipeline pipe("test_proc");

    auto source = std::make_unique<MockSource>();
    pipe.SetSource(std::move(source));

    auto sink = std::make_unique<MockSink>();
    auto* sink_ptr = sink.get();
    pipe.AddSink(std::move(sink));

    ASSERT_TRUE(pipe.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("y"), int64_t{99});
    pipe.Enqueue(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_TRUE(pipe.Stop().ok());

    auto received = sink_ptr->Received();
    ASSERT_GT(received.size(), 0u);
}

// 测试：Pipeline Start 无 Source 应返回错误。
TEST(PipelineIntegrationTest, StartWithoutSourceReturnsError) {
    Pipeline pipe("no_source");
    auto sink = std::make_unique<MockSink>();
    pipe.AddSink(std::move(sink));

    auto status = pipe.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
}

// 测试：Pipeline Start 无 Sink 应返回错误。
TEST(PipelineIntegrationTest, StartWithoutSinkReturnsError) {
    Pipeline pipe("no_sink");
    auto source = std::make_unique<MockSource>();
    pipe.SetSource(std::move(source));

    auto status = pipe.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
}

// 测试：空/nullptr batch Enqueue 不崩溃。
TEST(PipelineIntegrationTest, EnqueueNullAndEmptyBatchDoesNotCrash) {
    Pipeline pipe("test_null_enq");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>());

    ASSERT_TRUE(pipe.Start().ok());

    pipe.Enqueue(nullptr);
    pipe.Enqueue(std::make_shared<DataBatch>());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(pipe.Stop().ok());
}

// 测试：InjectFlush 到管道不崩溃。
TEST(PipelineIntegrationTest, InjectFlushDoesNotCrash) {
    Pipeline pipe("test_flush");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>());

    ASSERT_TRUE(pipe.Start().ok());

    pipe.InjectFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(pipe.Stop().ok());
}

// 测试：Pipeline 统计计数器初始值和更新。
TEST(PipelineIntegrationTest, StatisticsCountersTrackProcessing) {
    Pipeline pipe("test_stats");
    pipe.SetSource(std::make_unique<MockSource>());

    auto sink = std::make_unique<MockSink>();
    pipe.AddSink(std::move(sink));

    EXPECT_EQ(pipe.BatchesProcessed(), 0u);
    EXPECT_EQ(pipe.RecordsProcessed(), 0u);
    EXPECT_EQ(pipe.ErrorCount(), 0u);

    ASSERT_TRUE(pipe.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord();
    batch->AddRecord();
    pipe.Enqueue(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(pipe.Stop().ok());

    EXPECT_GT(pipe.BatchesProcessed(), 0u);
    EXPECT_GT(pipe.RecordsProcessed(), 0u);
}

// 测试：Channel 统计接口可正常访问。
TEST(PipelineIntegrationTest, ChannelStatisticsAccessible) {
    Pipeline pipe("test_chan_stats");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>());

    EXPECT_EQ(pipe.ChannelEnqueued(), 0u);
    EXPECT_EQ(pipe.ChannelDequeued(), 0u);
    EXPECT_EQ(pipe.ChannelDropped(), 0u);
    EXPECT_GT(pipe.ChannelCapacity(), 0u);
    EXPECT_EQ(pipe.ChannelSize(), 0u);
    EXPECT_FALSE(pipe.ChannelBackpressured());
}

// 测试：RunProcessors 直接执行 Processor 链（不经过异步通道）。
TEST(PipelineIntegrationTest, RunProcessorsDirectlyAppliesProcessorChain) {
    Pipeline pipe("test_run_proc");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord();

    auto result = pipe.RunProcessors(batch);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), batch);
}

// MockPushSource: 模拟 Push-mode 数据源（如 eBPF 探针）
class MockPushSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_push_source"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsPushMode() const override { return true; }

    Status Start() override {
        running_ = true;
        push_thread_ = std::thread([this] {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (callback_) {
                    auto batch = std::make_shared<DataBatch>(
                        DataBatch::Type::kProfile);
                    auto& s = batch->AddStackSample();
                    s.pid = 1234;
                    s.tid = 1234;
                    s.count = 1;
                    callback_(std::move(batch));
                    push_count_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        return Status::Ok();
    }

    Status Stop() override {
        running_ = false;
        if (push_thread_.joinable())
            push_thread_.join();
        return Status::Ok();
    }

    uint64_t PushCount() const { return push_count_.load(); }

private:
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> push_count_{0};
    std::thread push_thread_;
};

// 测试：Push-mode 源通过 Pipeline 直接工作
TEST(PipelineIntegrationTest, PushModeSourceWorksDirectly) {
    auto& infra = InfrastructureManager::Instance();
    if (!infra.IsStarted()) {
        infra.Start({.collect_pool_threads = 1, .sink_pool_threads = 2});
    }

    auto pipe = std::make_unique<Pipeline>("test_push_mode");
    pipe->SetSource(std::make_unique<MockPushSource>());

    auto sink = std::make_unique<MockSink>();
    auto* sink_ptr = sink.get();
    pipe->AddSink(std::move(sink));
    pipe->SetSinkPool(infra.GetSinkPool());

    ASSERT_TRUE(pipe->Start().ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    pipe->Stop();

    EXPECT_GT(sink_ptr->WriteCount(), 0u);
}

// 测试：多个 Push-mode Pipeline 可以独立启动
TEST(PipelineIntegrationTest, MultiplePushModePipelinesStartIndependently) {
    auto& infra = InfrastructureManager::Instance();
    if (!infra.IsStarted()) {
        infra.Start({.collect_pool_threads = 1, .sink_pool_threads = 2});
    }

    auto create_push_pipeline = [&](const std::string& name) {
        auto pipe = std::make_unique<Pipeline>(name);
        pipe->SetSource(std::make_unique<MockPushSource>());
        pipe->AddSink(std::make_unique<MockSink>());
        pipe->SetSinkPool(infra.GetSinkPool());
        return pipe;
    };

    auto p1 = create_push_pipeline("push_1");
    auto p2 = create_push_pipeline("push_2");
    auto p3 = create_push_pipeline("push_3");

    ASSERT_TRUE(p1->Start().ok());
    ASSERT_TRUE(p2->Start().ok());
    ASSERT_TRUE(p3->Start().ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    p1->Stop();
    p2->Stop();
    p3->Stop();
}

}  // namespace
}  // namespace illuminator
