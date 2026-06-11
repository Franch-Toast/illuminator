#include "sinks/fanout/sink_fanout.h"

#include <gtest/gtest.h>
#include <atomic>

using namespace illuminator;

namespace {

class CountingSink : public SinkPlugin {
public:
    explicit CountingSink(const char* name) : name_(name) {}
    const char* Name() const override { return name_; }
    const char* Version() const override { return "test"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    Status Write(DataBatchPtr batch) override {
        if (batch) ++count_;
        return Status::Ok();
    }
    int count() const { return count_; }
private:
    const char* name_;
    int count_ = 0;
};

class FailingSink : public SinkPlugin {
public:
    const char* Name() const override { return "failing"; }
    const char* Version() const override { return "test"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    Status Write(DataBatchPtr) override {
        return Status::Error(StatusCode::kInternal, "write failed");
    }
};

}  // namespace

TEST(SinkFanoutTest, DistributesToAllChildren) {
    SinkFanout fanout;
    auto s1 = std::make_shared<CountingSink>("s1");
    auto s2 = std::make_shared<CountingSink>("s2");
    fanout.AddSink(s1);
    fanout.AddSink(s2);

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    EXPECT_TRUE(fanout.Write(batch).ok());

    EXPECT_EQ(s1->count(), 1);
    EXPECT_EQ(s2->count(), 1);
}

TEST(SinkFanoutTest, NullBatchIgnored) {
    SinkFanout fanout;
    auto s1 = std::make_shared<CountingSink>("s1");
    fanout.AddSink(s1);

    EXPECT_TRUE(fanout.Write(nullptr).ok());
    EXPECT_EQ(s1->count(), 0);
}

TEST(SinkFanoutTest, FailingChildDoesNotBlockOthers) {
    SinkFanout fanout;
    auto s1 = std::make_shared<CountingSink>("s1");
    auto fail = std::make_shared<FailingSink>();
    auto s2 = std::make_shared<CountingSink>("s2");
    fanout.AddSink(s1);
    fanout.AddSink(fail);
    fanout.AddSink(s2);

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    EXPECT_TRUE(fanout.Write(batch).ok());

    EXPECT_EQ(s1->count(), 1);
    EXPECT_EQ(s2->count(), 1);
}

TEST(SinkFanoutTest, RemoveSink) {
    SinkFanout fanout;
    auto s1 = std::make_shared<CountingSink>("s1");
    auto s2 = std::make_shared<CountingSink>("s2");
    fanout.AddSink(s1);
    fanout.AddSink(s2);
    EXPECT_EQ(fanout.SinkCount(), 2u);

    EXPECT_TRUE(fanout.RemoveSink("s1"));
    EXPECT_EQ(fanout.SinkCount(), 1u);

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    fanout.Write(batch);
    EXPECT_EQ(s1->count(), 0);
    EXPECT_EQ(s2->count(), 1);
}

TEST(SinkFanoutTest, SinkNames) {
    SinkFanout fanout;
    fanout.AddSink(std::make_shared<CountingSink>("alpha"));
    fanout.AddSink(std::make_shared<CountingSink>("beta"));

    auto names = fanout.SinkNames();
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "beta");
}
