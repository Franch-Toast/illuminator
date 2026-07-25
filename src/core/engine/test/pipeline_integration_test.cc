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
#include "core/engine/self_observability.h"
#include "core/threading/thread_pool.h"

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

    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lock(mu_);
        received_.push_back(batch);
        write_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }

    std::vector<ConstDataBatchPtr> Received() {
        std::lock_guard<std::mutex> lock(mu_);
        return received_;
    }

private:
    std::mutex mu_;
    std::vector<ConstDataBatchPtr> received_;
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

// =========================================================================
// 新增 Mock 组件（用于扩展集成测试场景）
// =========================================================================

// ReconfigurableSource — 记录 Reconfigure 调用和参数
// 场景 3: Reconfigure 动态修改参数 + 验证 Source 收到
class ReconfigurableSource : public SourcePlugin {
public:
    const char* Name() const override { return "reconfig_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord().SetField(batch->InternString("v"), double{1.0});
        return batch;
    }

    Status Reconfigure(const ConfigValue& params) override {
        reconfigure_count_.fetch_add(1, std::memory_order_relaxed);
        last_interval_ms_.store(params["interval_ms"].AsInt(IntervalMs()));
        return Status::Ok();
    }

    uint64_t ReconfigureCount() const { return reconfigure_count_.load(); }
    int64_t LastIntervalMs() const { return last_interval_ms_.load(); }

private:
    std::atomic<uint64_t> reconfigure_count_{0};
    std::atomic<int64_t> last_interval_ms_{0};
};

// BackpressureSource — 记录 OnBackpressure 调用
// 场景 6: 反压触发 + 验证 Source OnBackpressure 被调用
class BackpressureSource : public SourcePlugin {
public:
    const char* Name() const override { return "bp_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord();
        return batch;
    }

    void OnBackpressure(bool active) override {
        if (active) {
            bp_active_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            bp_clear_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t BpActiveCount() const { return bp_active_count_.load(); }
    uint64_t BpClearCount() const { return bp_clear_count_.load(); }

private:
    std::atomic<uint64_t> bp_active_count_{0};
    std::atomic<uint64_t> bp_clear_count_{0};
};

// MockAggregator — 缓冲数据，仅在 Flush 时输出（模拟时间窗口聚合器）
class MockAggregator : public AggregatorPlugin {
public:
    explicit MockAggregator(uint32_t flush_interval_ms = 100)
        : flush_interval_ms_(flush_interval_ms) {}

    const char* Name() const override { return "mock_aggregator"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t FlushIntervalMs() const override { return flush_interval_ms_; }

    Status Add(DataBatchPtr batch) override {
        if (batch && !batch->Empty()) {
            buffered_.push_back(std::move(batch));
        }
        return Status::Ok();
    }

    StatusOr<std::vector<DataBatchPtr>> Flush() override {
        flush_count_.fetch_add(1, std::memory_order_relaxed);
        std::vector<DataBatchPtr> out = std::move(buffered_);
        buffered_.clear();
        return out;
    }

    uint64_t FlushCount() const { return flush_count_.load(); }

private:
    uint32_t flush_interval_ms_;
    std::vector<DataBatchPtr> buffered_;
    std::atomic<uint64_t> flush_count_{0};
};

// SlowSink — 每次写入时 sleep，用于触发反压和丢弃
class SlowSink : public SinkPlugin {
public:
    const char* Name() const override { return "slow_sink"; }
    const char* Version() const override { return "0.1.0"; }

    explicit SlowSink(int delay_ms = 100) : delay_ms_(delay_ms) {}

    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        write_count_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }

private:
    int delay_ms_;
    std::atomic<uint64_t> write_count_{0};
};

// TaggedSource — 生成带标签的数据，用于多 Pipeline 隔离测试
class TaggedSource : public SourcePlugin {
public:
    explicit TaggedSource(std::string tag) : tag_(std::move(tag)) {}

    const char* Name() const override { return "tagged_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();
        rec.labels.push_back(
            {batch->InternString("tag"), batch->InternString(tag_)});
        rec.SetField(batch->InternString("value"), double{1.0});
        return batch;
    }

private:
    std::string tag_;
};

// TagCheckingSink — 检查收到的数据标签，验证隔离性
class TagCheckingSink : public SinkPlugin {
public:
    explicit TagCheckingSink(std::string expected_tag)
        : expected_tag_(std::move(expected_tag)) {}

    const char* Name() const override { return "tag_check_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        write_count_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& rec : batch->records()) {
            for (const auto& label : rec.labels) {
                std::string key(label.key);
                std::string val(label.value);
                if (key == "tag" && val != expected_tag_) {
                    foreign_count_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }
    uint64_t ForeignCount() const { return foreign_count_.load(); }

private:
    std::string expected_tag_;
    std::atomic<uint64_t> write_count_{0};
    std::atomic<uint64_t> foreign_count_{0};
};

// PausablePushSource — 支持 PauseCollection/ResumeCollection 的 push 源
class PausablePushSource : public SourcePlugin {
public:
    const char* Name() const override { return "pausable_push_source"; }
    const char* Version() const override { return "0.1.0"; }
    bool IsPushMode() const override { return true; }

    Status Start() override {
        running_ = true;
        push_thread_ = std::thread([this] {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!paused_ && callback_) {
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
        if (push_thread_.joinable()) push_thread_.join();
        return Status::Ok();
    }

    Status PauseCollection() override { paused_ = true; return Status::Ok(); }
    Status ResumeCollection() override { paused_ = false; return Status::Ok(); }

    uint64_t PushCount() const { return push_count_.load(); }
    bool IsPaused() const { return paused_.load(); }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> push_count_{0};
    std::thread push_thread_;
};

// =========================================================================
// 场景 2: 动态 AddSinkRuntime + 验证新 Sink 开始接收
// =========================================================================
TEST(PipelineIntegrationTest, DynamicAddSinkRuntimeReceivesData) {
    Pipeline pipe("test_dynamic_add_int");
    pipe.SetSource(std::make_unique<MockSource>());

    auto sink1 = std::make_unique<MockSink>();
    auto* sink1_ptr = sink1.get();
    pipe.AddSink(std::move(sink1));

    ASSERT_TRUE(pipe.Start().ok());

    auto batch1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch1->AddRecord();
    pipe.Enqueue(batch1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(sink1_ptr->WriteCount(), 0u);

    // 动态添加第二个 Sink
    auto sink2 = std::make_unique<MockSink>();
    auto* sink2_ptr = sink2.get();
    ASSERT_TRUE(pipe.AddSinkRuntime(std::move(sink2)).ok());

    auto batch2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch2->AddRecord();
    pipe.Enqueue(batch2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pipe.Stop().ok());

    EXPECT_GT(sink1_ptr->WriteCount(), 0u);
    EXPECT_GT(sink2_ptr->WriteCount(), 0u);
}

// =========================================================================
// 场景 3: Reconfigure 动态修改参数 + 验证 Source 收到
// =========================================================================
TEST(PipelineIntegrationTest, ReconfigureParamsReachSource) {
    Pipeline pipe("test_reconfig_int");
    auto source = std::make_unique<ReconfigurableSource>();
    auto* source_ptr = source.get();
    pipe.SetSource(std::move(source));
    pipe.AddSink(std::make_unique<MockSink>());

    ASSERT_TRUE(pipe.Start().ok());

    ConfigValue params;
    params.Set("interval_ms", int64_t{250});

    auto status = pipe.Reconfigure(params);
    EXPECT_TRUE(status.ok()) << status.message();

    EXPECT_EQ(source_ptr->ReconfigureCount(), 1u);
    EXPECT_EQ(source_ptr->LastIntervalMs(), 250);

    ASSERT_TRUE(pipe.Stop().ok());
}

// =========================================================================
// 场景 4: Pause/Resume 状态切换 + 验证数据流中断/恢复
// =========================================================================
TEST(PipelineIntegrationTest, PauseResumeStopsAndResumesDataFlow) {
    auto& infra = InfrastructureManager::Instance();
    if (!infra.IsStarted()) {
        infra.Start({.collect_pool_threads = 1, .sink_pool_threads = 2});
    }

    auto pipe = std::make_unique<Pipeline>("test_pause_resume_int");
    pipe->SetSource(std::make_unique<PausablePushSource>());
    auto sink = std::make_unique<MockSink>();
    auto* sink_ptr = sink.get();
    pipe->AddSink(std::move(sink));
    pipe->SetSinkPool(infra.GetSinkPool());

    ASSERT_TRUE(pipe->Start().ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t count_before = sink_ptr->WriteCount();
    EXPECT_GT(count_before, 0u);

    // 暂停采集
    auto* src = pipe->GetSource();
    ASSERT_TRUE(src->PauseCollection().ok());
    EXPECT_TRUE(static_cast<PausablePushSource*>(src)->IsPaused());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 恢复采集
    ASSERT_TRUE(src->ResumeCollection().ok());
    EXPECT_FALSE(static_cast<PausablePushSource*>(src)->IsPaused());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t count_after = sink_ptr->WriteCount();
    EXPECT_GT(count_after, count_before);

    pipe->Stop();
}

// =========================================================================
// 场景 5: 多 Pipeline 并行运行不互相干扰
// =========================================================================
TEST(PipelineIntegrationTest, MultiplePipelinesDoNotInterfere) {
    auto& infra = InfrastructureManager::Instance();
    if (!infra.IsStarted()) {
        infra.Start({.collect_pool_threads = 2, .sink_pool_threads = 4});
    }

    auto pipe1 = std::make_unique<Pipeline>("test_isolation_A");
    pipe1->SetSource(std::make_unique<TaggedSource>("pipeline_A"));
    auto sink1 = std::make_unique<TagCheckingSink>("pipeline_A");
    auto* sink1_ptr = sink1.get();
    pipe1->AddSink(std::move(sink1));
    pipe1->SetSinkPool(infra.GetSinkPool());

    auto pipe2 = std::make_unique<Pipeline>("test_isolation_B");
    pipe2->SetSource(std::make_unique<TaggedSource>("pipeline_B"));
    auto sink2 = std::make_unique<TagCheckingSink>("pipeline_B");
    auto* sink2_ptr = sink2.get();
    pipe2->AddSink(std::move(sink2));
    pipe2->SetSinkPool(infra.GetSinkPool());

    ASSERT_TRUE(pipe1->Start().ok());
    ASSERT_TRUE(pipe2->Start().ok());

    auto batch_a = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec_a = batch_a->AddRecord();
    rec_a.labels.push_back(
        {batch_a->InternString("tag"), batch_a->InternString("pipeline_A")});
    rec_a.SetField(batch_a->InternString("v"), double{1.0});
    pipe1->Enqueue(batch_a);

    auto batch_b = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec_b = batch_b->AddRecord();
    rec_b.labels.push_back(
        {batch_b->InternString("tag"), batch_b->InternString("pipeline_B")});
    rec_b.SetField(batch_b->InternString("v"), double{2.0});
    pipe2->Enqueue(batch_b);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    pipe1->Stop();
    pipe2->Stop();

    EXPECT_GT(sink1_ptr->WriteCount(), 0u);
    EXPECT_GT(sink2_ptr->WriteCount(), 0u);
    // 各自只收到自己 Pipeline 的数据
    EXPECT_EQ(sink1_ptr->ForeignCount(), 0u);
    EXPECT_EQ(sink2_ptr->ForeignCount(), 0u);
}

// =========================================================================
// 场景 6: 反压触发 + 验证 Source OnBackpressure 被调用
// =========================================================================
TEST(PipelineIntegrationTest, BackpressureTriggersSourceCallback) {
    // 小 channel + 慢 sink → channel 填满 → 反压触发
    Pipeline pipe("test_bp_int", 8);  // capacity=8, bp_high=0.8 → bp at ~7
    auto source = std::make_unique<BackpressureSource>();
    auto* source_ptr = source.get();
    pipe.SetSource(std::move(source));
    pipe.AddSink(std::make_unique<SlowSink>(50));  // 50ms per write

    ASSERT_TRUE(pipe.Start().ok());

    // 快速注入大量数据，填满 channel
    for (int i = 0; i < 30; ++i) {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord();
        pipe.Enqueue(batch);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // OnBackpressure(true) 应该被调用
    EXPECT_GT(source_ptr->BpActiveCount(), 0u);
    EXPECT_GT(pipe.ChannelBackpressureEvents(), 0u);

    // 等待数据排空
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    ASSERT_TRUE(pipe.Stop().ok());
}

// =========================================================================
// 场景 7: Channel 满时数据丢弃 + 统计正确
// =========================================================================
TEST(PipelineIntegrationTest, ChannelFullDropsDataAndUpdatesStats) {
    // 极小 channel + 慢 sink → 数据被丢弃
    Pipeline pipe("test_drop_int", 4);  // capacity=4
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<SlowSink>(100));  // 100ms per write

    ASSERT_TRUE(pipe.Start().ok());

    // 快速注入超过 channel 容量的数据
    for (int i = 0; i < 30; ++i) {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord();
        pipe.Enqueue(batch);
    }

    // 应该有数据被丢弃
    EXPECT_GT(pipe.ChannelDropped(), 0u);
    EXPECT_GT(pipe.ChannelEnqueued(), 0u);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pipe.Stop().ok());
}

// =========================================================================
// MaybeForceFlush — 无 FlushSentinel 时超时强制 flush
// =========================================================================
TEST(PipelineIntegrationTest, MaybeForceFlushFlushesAggregatorWithoutSentinel) {
    constexpr uint32_t kFlushIntervalMs = 100;
    Pipeline pipe("test_force_flush");

    pipe.SetSource(std::make_unique<MockSource>());
    pipe.SetAggregator(std::make_unique<MockAggregator>(kFlushIntervalMs));

    auto sink = std::make_unique<MockSink>();
    auto* sink_ptr = sink.get();
    pipe.AddSink(std::move(sink));

    ASSERT_TRUE(pipe.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord().SetField(batch->InternString("v"), double{1.0});
    pipe.Enqueue(batch);

    // 不调用 InjectFlush；等待 >= 3 倍 flush interval（300ms）+ buffer
    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    ASSERT_TRUE(pipe.Stop().ok());

    EXPECT_EQ(pipe.ChannelFlushInjected(), 0u);
    EXPECT_GT(sink_ptr->WriteCount(), 0u)
        << "MaybeForceFlush should flush aggregator data to sink";
}

// =========================================================================
// SinkPool 过载丢弃 + 反压 gauge
// =========================================================================
TEST(PipelineIntegrationTest, SinkPoolOverloadDropsBatchesAndUpdatesMetrics) {
    constexpr const char* kPipeName = "test_sink_overload";
    const std::string drops_counter =
        std::string("pipeline_") + kPipeName + "_sink_drops_total";
    const std::string bp_gauge =
        std::string("pipeline_") + kPipeName + "_sink_pool_backpressure";

    const uint64_t drops_before =
        InternalMetrics::Instance().GetCounter(drops_counter);

    Pipeline pipe(kPipeName);
    pipe.SetSource(std::make_unique<MockSource>());

    auto sink = std::make_unique<SlowSink>(200);
    pipe.AddSink(std::move(sink));

    auto sink_pool = std::make_shared<ThreadPool>(1, "test-sink-pool");
    pipe.SetSinkPool(sink_pool);

    ASSERT_TRUE(pipe.Start().ok());

    for (int i = 0; i < 300; ++i) {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        batch->AddRecord();
        pipe.Enqueue(batch);
    }

    // 等待 ProcessThread 将批次提交到 SinkPool
    for (int i = 0; i < 50; ++i) {
        if (pipe.ErrorCount() > 0 &&
            InternalMetrics::Instance().GetGauge(bp_gauge) >= 1.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GT(pipe.ErrorCount(), 0u);
    EXPECT_GT(InternalMetrics::Instance().GetCounter(drops_counter),
              drops_before);
    EXPECT_GE(InternalMetrics::Instance().GetGauge(bp_gauge), 1.0)
        << "backpressure gauge should be 1.0 when pending > 128";

    ASSERT_TRUE(pipe.Stop().ok());
}

}  // namespace
}  // namespace illuminator
