// websocket_sink_test.cc — WebSocketSink 与 WebSocketSinkStore 单元测试
// 验证缓冲区管理、pipeline_key 隔离、溢出淘汰和插件元数据。

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "sinks/websocket_sink/websocket_sink.h"

namespace illuminator {
namespace {

// Init 应解析 max_buffer_size 和 pipeline_key 配置。
TEST(WebSocketSinkTest, InitParsesBufferSizeAndPipelineKeyConfig) {
    WebSocketSink sink;
    ConfigValue cfg;
    cfg.Set("max_buffer_size", int64_t{50});
    cfg.Set("pipeline_key", "my_pipeline");
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// Write 推入数据后通过 PollLatest 可获取最新批次。
TEST(WebSocketSinkTest, WriteAndPollLatestReturnsLastWrittenBatch) {
    WebSocketSink sink;
    ConfigValue cfg;
    cfg.Set("pipeline_key", "test_poll");
    cfg.Set("max_buffer_size", int64_t{10});
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto b1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    b1->SetMeta("id", "first");
    ASSERT_TRUE(sink.Write(b1).ok());

    auto b2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    b2->SetMeta("id", "second");
    ASSERT_TRUE(sink.Write(b2).ok());

    auto latest = WebSocketSink::PollLatest("test_poll");
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->GetMeta("id"), "second");
}

// PollRecent 应返回最近 N 条数据。
TEST(WebSocketSinkTest, PollRecentReturnsLastNBatches) {
    WebSocketSink sink;
    ConfigValue cfg;
    cfg.Set("pipeline_key", "test_recent");
    cfg.Set("max_buffer_size", int64_t{100});
    ASSERT_TRUE(sink.Init(cfg).ok());

    for (int i = 0; i < 5; ++i) {
        auto b = std::make_shared<DataBatch>();
        b->SetMeta("idx", std::to_string(i));
        ASSERT_TRUE(sink.Write(b).ok());
    }

    auto recent = WebSocketSink::PollRecent("test_recent", 3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0]->GetMeta("idx"), "2");
    EXPECT_EQ(recent[1]->GetMeta("idx"), "3");
    EXPECT_EQ(recent[2]->GetMeta("idx"), "4");
}

// 超过 max_buffer_size 时旧数据应被淘汰。
TEST(WebSocketSinkTest, BufferOverflowEvictsOldestBatches) {
    WebSocketSink sink;
    ConfigValue cfg;
    cfg.Set("pipeline_key", "test_evict");
    cfg.Set("max_buffer_size", int64_t{3});
    ASSERT_TRUE(sink.Init(cfg).ok());

    for (int i = 0; i < 5; ++i) {
        auto b = std::make_shared<DataBatch>();
        b->SetMeta("n", std::to_string(i));
        ASSERT_TRUE(sink.Write(b).ok());
    }

    auto all = WebSocketSink::PollRecent("test_evict", 100);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0]->GetMeta("n"), "2");
    EXPECT_EQ(all[1]->GetMeta("n"), "3");
    EXPECT_EQ(all[2]->GetMeta("n"), "4");
}

// 不同 pipeline_key 的数据应互相隔离。
TEST(WebSocketSinkTest, DifferentPipelineKeysAreIsolated) {
    WebSocketSink sinkA;
    ConfigValue cfgA;
    cfgA.Set("pipeline_key", "pipe_a");
    ASSERT_TRUE(sinkA.Init(cfgA).ok());

    WebSocketSink sinkB;
    ConfigValue cfgB;
    cfgB.Set("pipeline_key", "pipe_b");
    ASSERT_TRUE(sinkB.Init(cfgB).ok());

    auto bA = std::make_shared<DataBatch>();
    bA->SetMeta("from", "A");
    ASSERT_TRUE(sinkA.Write(bA).ok());

    auto bB = std::make_shared<DataBatch>();
    bB->SetMeta("from", "B");
    ASSERT_TRUE(sinkB.Write(bB).ok());

    EXPECT_EQ(WebSocketSink::PollLatest("pipe_a")->GetMeta("from"), "A");
    EXPECT_EQ(WebSocketSink::PollLatest("pipe_b")->GetMeta("from"), "B");
}

// Write nullptr 应安全返回 Ok 且不影响缓冲区。
TEST(WebSocketSinkTest, WriteNullptrIsNoOp) {
    WebSocketSink sink;
    ConfigValue cfg;
    cfg.Set("pipeline_key", "test_null");
    ASSERT_TRUE(sink.Init(cfg).ok());

    EXPECT_TRUE(sink.Write(nullptr).ok());
    EXPECT_EQ(WebSocketSink::PollLatest("test_null"), nullptr);
}

// 插件名称和版本应与注册契约一致。
TEST(WebSocketSinkTest, PluginNameAndVersionMatchContract) {
    WebSocketSink sink;
    EXPECT_STREQ(sink.Name(), "websocket_sink");
    EXPECT_STREQ(sink.Version(), "0.1.0");
}

// WebSocketSinkStore 单例应在多次访问中保持一致。
TEST(WebSocketSinkStoreTest, SingletonInstanceIsConsistentAcrossAccesses) {
    auto& store1 = WebSocketSinkStore::Instance();
    auto& store2 = WebSocketSinkStore::Instance();
    EXPECT_EQ(&store1, &store2);
}

// PollLatest 不存在的 pipeline_key 应返回 nullptr。
TEST(WebSocketSinkStoreTest, LatestReturnsNullptrForUnknownKey) {
    EXPECT_EQ(WebSocketSinkStore::Instance().Latest("nonexistent_key_xyz"), nullptr);
}

}  // namespace
}  // namespace illuminator
