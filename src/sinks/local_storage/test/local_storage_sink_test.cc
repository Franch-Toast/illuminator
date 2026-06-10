// local_storage_sink_test.cc — LocalStorageSink 单元测试
// 验证配置解析、Start/Stop 生命周期、Write 与 SQLite 后端交互。

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "sinks/local_storage/local_storage_sink.h"
#include "storage/storage_backend.h"
#include "storage/sqlite_backend/sqlite_backend.h"

namespace illuminator {
namespace {

// Init 应解析 backend/path/pipeline 配置。
TEST(LocalStorageSinkTest, InitParsesBackendPathPipelineConfig) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_test_storage");
    cfg.Set("pipeline", "cpu_profiler");
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// Start 应通过 StorageFactory 创建 SQLite 后端实例。
TEST(LocalStorageSinkTest, StartCreatesSqliteBackendAndInitsSuccessfully) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_lstest_" + std::to_string(getpid()));
    cfg.Set("pipeline", "test");
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto status = sink.Start();
    ASSERT_TRUE(status.ok()) << status.message();

    ASSERT_TRUE(sink.Stop().ok());
}

// Start 使用不存在的 backend 名称应返回 kNotFound 错误。
TEST(LocalStorageSinkTest, StartFailsForUnknownBackend) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "nonexistent_backend_xyz");
    cfg.Set("path", "/tmp/test");
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto status = sink.Start();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kNotFound);
}

// Write 含 Record 的 batch 应成功写入 SQLite。
TEST(LocalStorageSinkTest, WriteRecordsToSqliteBackend) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_lstest_wr_" + std::to_string(getpid()));
    cfg.Set("pipeline", "test_write");
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("cpu"), batch->InternString("0")});
    rec.SetField(batch->InternString("usage"), double{75.0});

    EXPECT_TRUE(sink.Write(batch).ok());
    EXPECT_TRUE(sink.Flush().ok());
    ASSERT_TRUE(sink.Stop().ok());
}

// Write 含 StackSample 的 batch 应成功写入 SQLite。
TEST(LocalStorageSinkTest, WriteStackSamplesToSqliteBackend) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_lstest_ss_" + std::to_string(getpid()));
    cfg.Set("pipeline", "test_stack");
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = batch->AddStackSample();
    s.pid = 100;
    s.tid = 200;
    s.comm = batch->InternString("test");
    s.count = 5;
    StackFrame fr;
    fr.function_name = batch->InternString("main");
    fr.address = 0x1234;
    s.user_stack.push_back(fr);

    EXPECT_TRUE(sink.Write(batch).ok());
    ASSERT_TRUE(sink.Stop().ok());
}

// Write nullptr 和空 batch 均应安全返回 Ok。
TEST(LocalStorageSinkTest, WriteNullAndEmptyBatchReturnOk) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_lstest_null_" + std::to_string(getpid()));
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    EXPECT_TRUE(sink.Write(nullptr).ok());
    EXPECT_TRUE(sink.Write(std::make_shared<DataBatch>()).ok());
    ASSERT_TRUE(sink.Stop().ok());
}

// Stop 可多次调用且不报错。
TEST(LocalStorageSinkTest, StopCanBeCalledMultipleTimes) {
    LocalStorageSink sink;
    ConfigValue cfg;
    cfg.Set("backend", "sqlite");
    cfg.Set("path", "/tmp/illuminator_lstest_stop_" + std::to_string(getpid()));
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());
    EXPECT_TRUE(sink.Stop().ok());
    EXPECT_TRUE(sink.Stop().ok());
}

// 插件名称和版本应与注册契约一致。
TEST(LocalStorageSinkTest, PluginNameAndVersionMatchContract) {
    LocalStorageSink sink;
    EXPECT_STREQ(sink.Name(), "local_storage");
    EXPECT_STREQ(sink.Version(), "0.1.0");
}

}  // namespace
}  // namespace illuminator
