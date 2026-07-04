// console_sink_test.cc — ConsoleSink 单元测试
// 验证控制台输出 Sink 的初始化、Write 行为和插件元数据。

#include <memory>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "plugin/sinks/console_output/console_sink.h"

namespace illuminator {
namespace {

// Init 应解析 format 配置（text/json），不报错。
TEST(ConsoleSinkTest, InitParsesFormatConfigWithoutError) {
    ConsoleSink sink;
    ConfigValue cfg;
    cfg.Set("format", "json");
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// Init 默认 format 为 text，不报错。
TEST(ConsoleSinkTest, InitDefaultsToTextFormatWhenConfigEmpty) {
    ConsoleSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());
}

// Write 非空 batch（含 Record）应成功且不崩溃。
TEST(ConsoleSinkTest, WriteWithRecordsCompletesSuccessfully) {
    ConsoleSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("cpu"), batch->InternString("0")});
    rec.SetField(batch->InternString("usage"), double{85.3});

    EXPECT_TRUE(sink.Write(batch).ok());
}

// Write JSON 模式下含 Record 应成功。
TEST(ConsoleSinkTest, WriteJsonModeWithRecordsCompletesSuccessfully) {
    ConsoleSink sink;
    ConfigValue cfg;
    cfg.Set("format", "json");
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("val"), int64_t{42});

    EXPECT_TRUE(sink.Write(batch).ok());
}

// Write 含 StackSample 应成功输出堆栈信息。
TEST(ConsoleSinkTest, WriteWithStackSamplesCompletesSuccessfully) {
    ConsoleSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = batch->AddStackSample();
    s.pid = 100;
    s.tid = 200;
    s.comm = batch->InternString("test_proc");
    s.count = 5;
    StackFrame fr;
    fr.function_name = batch->InternString("main");
    fr.address = 0x1234;
    s.user_stack.push_back(fr);

    EXPECT_TRUE(sink.Write(batch).ok());
}

// Write 空 batch 和 nullptr 均应安全返回 Ok。
TEST(ConsoleSinkTest, WriteNullAndEmptyBatchReturnOk) {
    ConsoleSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    EXPECT_TRUE(sink.Write(nullptr).ok());
    EXPECT_TRUE(sink.Write(std::make_shared<DataBatch>()).ok());
}

// 插件名称和版本应与注册契约一致。
TEST(ConsoleSinkTest, PluginNameAndVersionMatchContract) {
    ConsoleSink sink;
    EXPECT_STREQ(sink.Name(), "console_output");
    EXPECT_STREQ(sink.Version(), "0.1.0");
}

}  // namespace
}  // namespace illuminator
