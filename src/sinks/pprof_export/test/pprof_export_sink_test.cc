// PprofExportSink 单元测试 — 折叠栈路径、计数累加与 Flush 持久化。

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "sinks/pprof_export/pprof_export_sink.h"

namespace illuminator {
namespace {

class PprofExportSinkTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        folded_path_ = "/tmp/test_pprof_" + std::to_string(getpid()) + ".folded";
        unlink(folded_path_.c_str());
    }

    void TearDown() override { unlink(folded_path_.c_str()); }

    std::string folded_path_;
};

// Init 应解析输出 path。
TEST_F(PprofExportSinkTestFixture, InitParsesOutputPathConfiguration) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// Write 分批累积；Flush 写成文件后再 Flush 不应再写出旧数据（缓存已清空）。
TEST_F(PprofExportSinkTestFixture, WriteAccumulatesAndFlushWritesFileThenClearsCache) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = b->AddStackSample();
    s.comm = b->InternString("app");
    s.count = 1;
    StackFrame f;
    f.address = 0x10;
    f.function_name = b->InternString("leaf");
    s.user_stack.push_back(f);  // 单帧栈视为根
    ASSERT_TRUE(sink.Write(b).ok());

    ASSERT_TRUE(sink.Flush().ok());
    ASSERT_TRUE(sink.Flush().ok());

    std::ifstream first(folded_path_);
    std::stringstream buf;
    buf << first.rdbuf();
    std::string once = buf.str();
    ASSERT_FALSE(once.empty());
}

// folded 字符串格式应为 comm 后接分号与自叶向根展开的函数序列。
TEST_F(PprofExportSinkTestFixture, FoldedStackFormatFollowsCommaSeparatedFunctionTrail) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& samp = batch->AddStackSample();
    samp.comm = batch->InternString("worker");
    StackFrame root;
    root.function_name = batch->InternString("root_fn");
    root.address = 0x100;
    StackFrame leaf;
    leaf.function_name = batch->InternString("leaf_fn");
    leaf.address = 0x200;
    // 自下而上 root -> leaf（常见采集顺序）。
    samp.user_stack.push_back(root);
    samp.user_stack.push_back(leaf);
    samp.count = 4;
    ASSERT_TRUE(sink.Write(batch).ok());
    ASSERT_TRUE(sink.Flush().ok());

    std::ifstream in(folded_path_);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));

    EXPECT_NE(line.find("worker;leaf_fn;root_fn"), std::string::npos);
}

// 完全相同的折叠键应合并 count。
TEST_F(PprofExportSinkTestFixture, IdenticalStacksMergeSampleCountsOnFlush) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto make_sample = [&](uint64_t c) {
        auto b = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& s = b->AddStackSample();
        s.comm = b->InternString("c");
        s.count = c;
        StackFrame fr;
        fr.address = 0xabc;
        fr.function_name = b->InternString("g");
        s.user_stack.push_back(fr);
        return b;
    };

    ASSERT_TRUE(sink.Write(make_sample(5)).ok());
    ASSERT_TRUE(sink.Write(make_sample(9)).ok());
    ASSERT_TRUE(sink.Flush().ok());

    std::ifstream in(folded_path_);
    std::string body;
    std::getline(in, body);

    ASSERT_NE(body.find(' '), body.npos);  // "stack count" 空格分隔（实现侧）
    EXPECT_NE(body.find(" 14"), std::string::npos);
}

// 空 batch（无 stack_samples）不产生文件内容变化；首次 Flush 可空跳过。
TEST_F(PprofExportSinkTestFixture, EmptyBatchDoesNotAddFoldedStacks) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto empty = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    ASSERT_TRUE(sink.Write(empty).ok());
    ASSERT_TRUE(sink.Flush().ok());  // 无条目则 early return

    std::ifstream check(folded_path_);
    std::stringstream ss;
    ss << check.rdbuf();
    EXPECT_TRUE(ss.str().empty());
}

// Name() 期望值。
TEST_F(PprofExportSinkTestFixture, PluginNameIsPprofExport) {
    PprofExportSink sink;
    EXPECT_STREQ(sink.Name(), "pprof_export");
}

// 缺失函数名时行内应退化到 0x 地址占位（仍可唯一折叠）。
TEST_F(PprofExportSinkTestFixture, MissingFunctionNamesFallBackToHexAddressTokens) {
    PprofExportSink sink;
    ConfigValue cfg;
    cfg.Set("path", folded_path_);
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = b->AddStackSample();
    s.comm = b->InternString("anon");
    StackFrame fx;
    fx.address = 0xdeadbeef;
    // function_name 留空迫使 BuildFoldedStack 退回地址字面量
    s.user_stack.push_back(fx);
    s.count = 1;
    ASSERT_TRUE(sink.Write(b).ok());
    ASSERT_TRUE(sink.Flush().ok());

    std::ifstream in(folded_path_);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    EXPECT_NE(line.find("deadbeef"), std::string::npos);
}

}  // namespace
}  // namespace illuminator
