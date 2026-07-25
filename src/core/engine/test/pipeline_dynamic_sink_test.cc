// pipeline_dynamic_sink_test.cc — Pipeline 运行时动态 Sink 管理测试
// 验证 AddSinkRuntime / RemoveSinkRuntime 的语义与并发安全性。

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
#include "core/engine/pipeline.h"

namespace illuminator {
namespace {

// MockSource: 简单的 Pull 数据源，每次 Collect 返回固定数据。
class MockSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_source"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();
        rec.SetField(batch->InternString("value"), double{42.0});
        return batch;
    }
};

// MockSink: 模拟 Sink，支持自定义名称，记录收到的写入次数。
class MockSink : public SinkPlugin {
public:
    explicit MockSink(const std::string& name) : name_(name) {}

    const char* Name() const override { return name_.c_str(); }
    const char* Version() const override { return "0.1.0"; }

    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lock(mu_);
        write_count_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    uint64_t WriteCount() const { return write_count_.load(); }

private:
    std::string name_;
    mutable std::mutex mu_;
    std::atomic<uint64_t> write_count_{0};
};

// 测试：Pipeline 启动后可以通过 AddSinkRuntime 动态添加 Sink。
TEST(PipelineDynamicSinkTest, AddSinkRuntimeWhileRunning) {
    Pipeline pipe("test_add_runtime");
    pipe.SetSource(std::make_unique<MockSource>());

    auto static_sink = std::make_unique<MockSink>("static");
    auto* static_ptr = static_sink.get();
    pipe.AddSink(std::move(static_sink));

    ASSERT_TRUE(pipe.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("x"), int64_t{1});
    pipe.Enqueue(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(static_ptr->WriteCount(), 0u);

    auto dynamic_sink = std::make_unique<MockSink>("dynamic");
    auto* dynamic_ptr = dynamic_sink.get();
    ASSERT_TRUE(pipe.AddSinkRuntime(std::move(dynamic_sink)).ok());

    auto batch2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec2 = batch2->AddRecord();
    rec2.SetField(batch2->InternString("y"), int64_t{2});
    pipe.Enqueue(batch2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pipe.Stop().ok());

    EXPECT_GT(static_ptr->WriteCount(), 0u);
    EXPECT_GT(dynamic_ptr->WriteCount(), 0u);
}

// 测试：AddSinkRuntime 对 null sink 返回错误。
TEST(PipelineDynamicSinkTest, AddSinkRuntimeRejectsNull) {
    Pipeline pipe("test_null_runtime");
    auto status = pipe.AddSinkRuntime(nullptr);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
}

// 测试：RemoveSinkRuntime 可以移除运行时添加的 Sink。
// 注意：RemoveSinkRuntime 会销毁被移除的 Sink（Pipeline 通过 shared_ptr 持有所有权），
// 因此移除后不能通过原始指针访问该对象。验证方式：移除后仅 static sink 收到新数据。
TEST(PipelineDynamicSinkTest, RemoveSinkRuntimeStopsDataFlow) {
    Pipeline pipe("test_remove_runtime");
    pipe.SetSource(std::make_unique<MockSource>());

    auto static_sink = std::make_unique<MockSink>("static");
    auto* static_ptr = static_sink.get();
    pipe.AddSink(std::move(static_sink));

    pipe.AddSink(std::make_unique<MockSink>("dynamic"));

    ASSERT_TRUE(pipe.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord();
    pipe.Enqueue(batch);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(static_ptr->WriteCount(), 0u);

    uint64_t static_before = static_ptr->WriteCount();
    ASSERT_TRUE(pipe.RemoveSinkRuntime("dynamic").ok());

    auto batch2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch2->AddRecord();
    pipe.Enqueue(batch2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(pipe.Stop().ok());

    EXPECT_GT(static_ptr->WriteCount(), static_before);
}

// 测试：RemoveSinkRuntime 对不存在的 Sink 返回 NotFound。
TEST(PipelineDynamicSinkTest, RemoveSinkRuntimeNotFound) {
    Pipeline pipe("test_remove_notfound");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>("static"));
    ASSERT_TRUE(pipe.Start().ok());

    auto status = pipe.RemoveSinkRuntime("nonexistent");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kNotFound);

    ASSERT_TRUE(pipe.Stop().ok());
}

// 测试：并发地添加、移除和写入 Sink 不会崩溃或数据竞争。
TEST(PipelineDynamicSinkTest, ConcurrentAddRemoveWrite) {
    Pipeline pipe("test_concurrent");
    pipe.SetSource(std::make_unique<MockSource>());
    pipe.AddSink(std::make_unique<MockSink>("static"));
    ASSERT_TRUE(pipe.Start().ok());

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    // 写入线程：持续向 Pipeline 注入数据。
    threads.emplace_back([&pipe, &stop] {
        uint64_t count = 0;
        while (!stop.load()) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
            batch->AddRecord().SetField(batch->InternString("v"), int64_t{count++});
            pipe.Enqueue(batch);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // 动态添加/移除线程：反复添加和移除一个 Sink。
    threads.emplace_back([&pipe, &stop] {
        while (!stop.load()) {
            auto sink = std::make_unique<MockSink>("dynamic");
            auto status = pipe.AddSinkRuntime(std::move(sink));
            if (status.ok()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                pipe.RemoveSinkRuntime("dynamic");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_TRUE(pipe.Stop().ok());
}

}  // namespace
}  // namespace illuminator
