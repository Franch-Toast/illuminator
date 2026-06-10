// FileExportSink 单元测试 — Start/Stop、JSONL 追加、Flush 与空 batch 语义。

#include <unistd.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "sinks/file_export/file_export_sink.h"

namespace illuminator {
namespace {

class FileExportSinkTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = "/tmp/test_file_export_" + std::to_string(getpid()) + ".jsonl";
        unlink(path_.c_str());
    }

    void TearDown() override { unlink(path_.c_str()); }

    std::string path_;
};

// Init 应解析输出 path。
TEST_F(FileExportSinkTestFixture, InitParsesOutputPathConfiguration) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// Start 以追加方式打开输出文件；Stop 关闭并最终可重复调用仍为成功。
TEST_F(FileExportSinkTestFixture, StartOpensAppendFileAndStopClosesHandleCleanly) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());
    ASSERT_TRUE(sink.Stop().ok());
    ASSERT_TRUE(sink.Stop().ok());
}

// Write 为每条 Record 输出一行合法 JSON。
TEST_F(FileExportSinkTestFixture, WriteAppendsOneJsonLinePerRecord) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& row1 = batch->AddRecord();
    row1.labels.push_back({batch->InternString("svc"), batch->InternString("unit")});
    row1.SetField(batch->InternString("value"), double{2.25});

    auto& row2 = batch->AddRecord();
    row2.SetField(batch->InternString("value"), int64_t{-7});

    ASSERT_TRUE(sink.Write(batch).ok());
    ASSERT_TRUE(sink.Stop().ok());

    std::ifstream in(path_);
    std::string line1;
    std::string line2;
    ASSERT_TRUE(std::getline(in, line1));
    ASSERT_TRUE(std::getline(in, line2));

    auto j1 = nlohmann::json::parse(line1);
    auto j2 = nlohmann::json::parse(line2);
    EXPECT_DOUBLE_EQ(j1["fields"]["value"].get<double>(), 2.25);
    EXPECT_EQ(j2["fields"]["value"].get<int64_t>(), -7);
}

// Flush 在文件仍打开时将缓冲刷入磁盘以便立刻读取校验。
TEST_F(FileExportSinkTestFixture, FlushFlushesBufferedOutputWhileRemainingOpenForMoreWrites) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    b->AddRecord().SetField(b->InternString("k"), uint64_t{123});
    ASSERT_TRUE(sink.Write(b).ok());
    ASSERT_TRUE(sink.Flush().ok());

    std::ifstream verify(path_);
    std::stringstream buf;
    buf << verify.rdbuf();
    EXPECT_NE(buf.str().find("123"), std::string::npos);

    ASSERT_TRUE(sink.Stop().ok());
}

// 未调用 Start 时 Write 不写文件且无错误返回。
TEST_F(FileExportSinkTestFixture, WriteWithoutStartIsQuietNoOperation) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord().SetField(batch->InternString("x"), double{1.0});
    ASSERT_TRUE(sink.Write(batch).ok());

    std::ifstream check(path_);
    std::stringstream ss;
    ss << check.rdbuf();
    EXPECT_TRUE(ss.str().empty());
}

// 空 batch（或 nullptr）不应产生额外 JSON 行。
TEST_F(FileExportSinkTestFixture, EmptyBatchWriteDoesNotAppendJsonLines) {
    FileExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
    ASSERT_TRUE(sink.Start().ok());

    auto seeded = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    seeded->AddRecord().SetField(seeded->InternString("a"), double{1.0});
    ASSERT_TRUE(sink.Write(seeded).ok());

    auto empty = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    ASSERT_TRUE(sink.Write(empty).ok());
    ASSERT_TRUE(sink.Write(nullptr).ok());
    ASSERT_TRUE(sink.Stop().ok());

    int lines = 0;
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    EXPECT_EQ(lines, 1);
}

// Name() 与内置注册名匹配。
TEST_F(FileExportSinkTestFixture, PluginNameIsFileExport) {
    FileExportSink sink;
    EXPECT_STREQ(sink.Name(), "file_export");
}

}  // namespace
}  // namespace illuminator
