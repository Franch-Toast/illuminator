#include "sinks/stream_sink/stream_sink.h"

#include <gtest/gtest.h>
#include <thread>

using namespace illuminator;

TEST(StreamBufferTest, PushAndLatest) {
    StreamBuffer buf(5);
    EXPECT_EQ(buf.Latest(), nullptr);
    EXPECT_EQ(buf.Size(), 0u);

    auto b1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    buf.Push(b1);
    EXPECT_EQ(buf.Latest(), b1);
    EXPECT_EQ(buf.Size(), 1u);
    EXPECT_EQ(buf.Sequence(), 1u);
}

TEST(StreamBufferTest, RingBufferEviction) {
    StreamBuffer buf(3);
    for (int i = 0; i < 5; ++i) {
        buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    }
    EXPECT_EQ(buf.Size(), 3u);
    EXPECT_EQ(buf.Sequence(), 5u);
}

TEST(StreamBufferTest, PollSinceIncremental) {
    StreamBuffer buf(10);
    for (int i = 0; i < 5; ++i) {
        buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    }

    uint64_t cursor = 0;
    auto result = buf.PollSince(cursor);
    EXPECT_EQ(result.size(), 5u);
    EXPECT_EQ(cursor, 5u);

    // Second poll with no new data
    result = buf.PollSince(cursor);
    EXPECT_TRUE(result.empty());

    // Push more
    buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));

    result = buf.PollSince(cursor);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(cursor, 7u);
}

TEST(StreamBufferTest, PollSinceAfterEviction) {
    StreamBuffer buf(3);
    for (int i = 0; i < 10; ++i) {
        buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    }

    uint64_t cursor = 2;  // stale cursor
    auto result = buf.PollSince(cursor);
    EXPECT_EQ(result.size(), 3u);  // can only get the 3 most recent
    EXPECT_EQ(cursor, 10u);
}

TEST(StreamBufferTest, RecentSubset) {
    StreamBuffer buf(10);
    for (int i = 0; i < 7; ++i) {
        buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    }

    auto recent = buf.Recent(3);
    EXPECT_EQ(recent.size(), 3u);

    auto all = buf.Recent(100);
    EXPECT_EQ(all.size(), 7u);
}

TEST(StreamSinkStoreTest, GetBufferCreatesOnDemand) {
    auto& store = StreamSinkStore::Instance();
    auto& buf = store.GetBuffer("test_feature_1");
    buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
    EXPECT_EQ(buf.Size(), 1u);

    // Same name returns same buffer
    auto& buf2 = store.GetBuffer("test_feature_1");
    EXPECT_EQ(buf2.Size(), 1u);

    store.RemoveBuffer("test_feature_1");
}

TEST(StreamSinkTest, WriteToBuffer) {
    StreamSink sink;
    sink.SetFeatureName("test_stream_write");

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    EXPECT_TRUE(sink.Write(batch).ok());

    auto& store = StreamSinkStore::Instance();
    auto latest = store.GetBuffer("test_stream_write").Latest();
    EXPECT_NE(latest, nullptr);
    EXPECT_EQ(latest, batch);

    store.RemoveBuffer("test_stream_write");
}

TEST(StreamBufferTest, ThreadSafety) {
    StreamBuffer buf(100);
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            buf.Push(std::make_shared<DataBatch>(DataBatch::Type::kMetrics));
        }
        done = true;
    });

    std::thread reader([&] {
        uint64_t cursor = 0;
        while (!done.load()) {
            buf.PollSince(cursor);
            std::this_thread::yield();
        }
        auto final_result = buf.PollSince(cursor);
        (void)final_result;
    });

    writer.join();
    reader.join();
    EXPECT_EQ(buf.Sequence(), 1000u);
}
