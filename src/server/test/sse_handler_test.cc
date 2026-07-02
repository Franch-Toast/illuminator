// SSE Handler 单元测试

#include "server/sse_handler.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace illuminator {
namespace {

class SseHandlerTest : public ::testing::Test {
protected:
    void TearDown() override {
        (void)SseHandler::Instance();
    }
};

TEST_F(SseHandlerTest, SubscribeReturnsId) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"cpu_utilization", "io_monitor"});
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(id.find("sub_") == 0);
    handler.Unsubscribe(id);
}

TEST_F(SseHandlerTest, MultipleSubscriptionsGetUniqueIds) {
    auto& handler = SseHandler::Instance();
    auto id1 = handler.Subscribe({"cpu_utilization"});
    auto id2 = handler.Subscribe({"io_monitor"});
    EXPECT_NE(id1, id2);
    handler.Unsubscribe(id1);
    handler.Unsubscribe(id2);
}

TEST_F(SseHandlerTest, UnsubscribeRemovesSubscription) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"cpu_utilization"});
    size_t before = handler.ActiveSubscriptions();
    handler.Unsubscribe(id);
    EXPECT_LT(handler.ActiveSubscriptions(), before);
}

TEST_F(SseHandlerTest, UpdateSubscriptionAddsFeatures) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"cpu_utilization"});

    auto status = handler.UpdateSubscription(id, {"io_monitor"}, {});
    EXPECT_TRUE(status.ok());

    handler.Unsubscribe(id);
}

TEST_F(SseHandlerTest, UpdateSubscriptionRemovesFeatures) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"cpu_utilization", "io_monitor"});

    auto status = handler.UpdateSubscription(id, {}, {"cpu_utilization"});
    EXPECT_TRUE(status.ok());

    handler.Unsubscribe(id);
}

TEST_F(SseHandlerTest, UpdateNonexistentSubscriptionReturnsError) {
    auto& handler = SseHandler::Instance();
    auto status = handler.UpdateSubscription("nonexistent", {"cpu"}, {});
    EXPECT_FALSE(status.ok());
}

TEST_F(SseHandlerTest, PublishDoesNotCrashWithNoSubscribers) {
    auto& handler = SseHandler::Instance();
    handler.Publish("cpu_utilization", R"({"value": 42})");
}

TEST_F(SseHandlerTest, SseSinkWritePublishes) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"test_feature"});

    SseSink sink("test_feature");
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto status = sink.Write(batch);
    EXPECT_TRUE(status.ok());

    handler.Unsubscribe(id);
}

TEST_F(SseHandlerTest, SseSinkNameAndVersion) {
    SseSink sink("cpu_utilization");
    EXPECT_STREQ(sink.Name(), "sse_sink");
    EXPECT_STREQ(sink.Version(), "1.0.0");
}

}  // namespace
}  // namespace illuminator
