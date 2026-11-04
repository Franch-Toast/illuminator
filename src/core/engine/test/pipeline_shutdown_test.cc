#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <gtest/gtest.h>
#include "core/engine/pipeline.h"

namespace illuminator {
namespace {
class EmptySource : public SourcePlugin {
public:
    const char* Name() const override { return "empty"; }
    const char* Version() const override { return "1"; }
};
class SlowSink : public SinkPlugin {
public:
    const char* Name() const override { return "slow"; }
    const char* Version() const override { return "1"; }
    Status Write(ConstDataBatchPtr) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (stopped.load()) ++writes_after_stop;
        ++writes;
        return Status::Ok();
    }
    Status Stop() override { stopped = true; return Status::Ok(); }
    std::atomic<bool> stopped{false};
    std::atomic<int> writes{0};
    std::atomic<int> writes_after_stop{0};
};

TEST(PipelineShutdownTest, DrainsWritesBeforeStoppingSinkAndLeavesPoolUsable) {
    auto pool = std::make_shared<ThreadPool>(1);
    Pipeline pipe("shutdown");
    pipe.SetSource(std::make_unique<EmptySource>());
    auto sink = std::make_unique<SlowSink>();
    auto* observed = sink.get();
    pipe.AddSink(std::move(sink));
    pipe.SetSinkPool(pool);
    ASSERT_TRUE(pipe.Start().ok());
    for (int i = 0; i < 16; ++i) {
        auto batch = std::make_shared<DataBatch>();
        batch->AddRecord();
        pipe.Enqueue(std::move(batch));
    }
    ASSERT_TRUE(pipe.Stop().ok());
    EXPECT_EQ(observed->writes.load(), 16);
    EXPECT_EQ(observed->writes_after_stop.load(), 0);
    EXPECT_TRUE(observed->stopped.load());
    EXPECT_EQ(pool->Submit([] { return 42; }).get(), 42);
}
}  // namespace
}  // namespace illuminator
