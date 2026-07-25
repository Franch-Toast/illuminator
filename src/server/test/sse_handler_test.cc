// SSE Handler 单元测试

#include "server/sse_handler.h"
#include "plugin/sinks/sse_sink/sse_sink.h"

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

    SseSink sink("test_feature", [&handler](const std::string& feature, const std::string& data) {
        handler.Publish(feature, data);
    });
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto status = sink.Write(batch);
    EXPECT_TRUE(status.ok());

    handler.Unsubscribe(id);
}

TEST_F(SseHandlerTest, SseSinkNameAndVersion) {
    SseSink sink("cpu_utilization", [](const std::string&, const std::string&) {});
    EXPECT_STREQ(sink.Name(), "sse_sink");
    EXPECT_STREQ(sink.Version(), "1.0.0");
}

TEST_F(SseHandlerTest, ReplaysMessagesAfterLastEventId) {
    auto& handler = SseHandler::Instance();
    auto id = handler.Subscribe({"test_feature"});

    handler.Publish("test_feature", R"({"seq":1})");
    handler.Publish("test_feature", R"({"seq":2})");
    handler.Publish("test_feature", R"({"seq":3})");

    uint64_t last_event_id = 0;
    {
        auto sub = handler.GetSubscription(id);
        std::lock_guard lock(sub->mu);
        ASSERT_EQ(sub->recent_messages.size(), 3);
        last_event_id = sub->recent_messages[1].first;
        // Simulate that the original connection already consumed the outbox.
        while (!sub->outbox.empty()) sub->outbox.pop();
        sub->replay_done.store(false);
    }

    httplib::Request req;
    req.path = "/api/v1/events/" + id;
    req.headers.insert({"Last-Event-ID", std::to_string(last_event_id)});
    httplib::Response res;
    handler.HandleSseConnection(req, id, res);

    std::string body;
    httplib::DataSink sink;
    sink.write = [&body](const char* data, size_t len) {
        body.append(data, len);
        return true;
    };
    sink.is_writable = []() { return true; };

    auto sub = handler.GetSubscription(id);
    std::thread provider_thread([&res, &sink]() {
        res.content_provider_(0, 0, sink);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sub->active.store(false);
    sub->cv.notify_all();
    provider_thread.join();

    EXPECT_NE(body.find(R"("seq":3)"), std::string::npos);
    EXPECT_EQ(body.find(R"("seq":1)"), std::string::npos);
    EXPECT_EQ(body.find(R"("seq":2)"), std::string::npos);

    handler.Unsubscribe(id);
}

}  // namespace
}  // namespace illuminator
