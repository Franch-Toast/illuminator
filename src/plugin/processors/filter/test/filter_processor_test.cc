// filter_processor_test.cc — FilterProcessor 单元测试

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "plugin/processors/filter/filter_processor.h"

namespace illuminator {
namespace {

// Init 应从 ConfigValue 中解析 exclude_label_key / exclude_label_value，并在 Process 中生效。
TEST(FilterProcessorTest, InitParsesExcludeLabelKeyAndValue) {
    FilterProcessor proc;
    ConfigValue cfg;
    cfg.Set("exclude_label_key", "cpu");
    cfg.Set("exclude_label_value", "cpu0");

    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

    auto& kept = batch->AddRecord();
    kept.labels.push_back({batch->InternString("cpu"), batch->InternString("cpu1")});

    auto& dropped = batch->AddRecord();
    dropped.labels.push_back({batch->InternString("cpu"), batch->InternString("cpu0")});

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto output = out.value();
    ASSERT_NE(output, batch);
    ASSERT_EQ(output->records().size(), 1u);
    EXPECT_EQ(output->records()[0].labels[0].value, batch->InternString("cpu1"));
}

// 标签键或值不匹配排除规则时，对应 Record 应保留在输出中。
TEST(FilterProcessorTest, ProcessKeepsRecordsThatDoNotMatchExcludeRule) {
    FilterProcessor proc;
    ConfigValue cfg;
    cfg.Set("exclude_label_key", "cpu");
    cfg.Set("exclude_label_value", "cpu0");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("cpu"), batch->InternString("cpu2")});

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto output = out.value();
    ASSERT_EQ(output->records().size(), 1u);
}

// exclude_label_key 为空字符串时等价于禁用过滤：应返回原始批次指针。
TEST(FilterProcessorTest, ProcessPassthroughWhenExcludeKeyIsEmpty) {
    FilterProcessor proc;
    ConfigValue cfg;
    cfg.Set("exclude_label_key", "");
    cfg.Set("exclude_label_value", "cpu0");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    batch->AddRecord();

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), batch);
}

// StackSample 不参与标签过滤，输入中的堆栈应全部出现在输出中。
TEST(FilterProcessorTest, ProcessPreservesAllStackSamplesUnaffectedByFiltering) {
    FilterProcessor proc;
    ConfigValue cfg;
    cfg.Set("exclude_label_key", "cpu");
    cfg.Set("exclude_label_value", "cpu0");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("cpu"), batch->InternString("cpu0")});

    auto& s1 = batch->AddStackSample();
    s1.pid = 1;
    s1.comm = batch->InternString("a");

    auto& s2 = batch->AddStackSample();
    s2.pid = 2;
    s2.comm = batch->InternString("b");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto output = out.value();
    EXPECT_TRUE(output->records().empty());
    ASSERT_EQ(output->stack_samples().size(), 2u);
    EXPECT_EQ(output->stack_samples()[0].pid, 1u);
    EXPECT_EQ(output->stack_samples()[1].pid, 2u);
}

// 单条 Record 含多个 label 时，只要存在一对匹配排除键值即应被丢弃。
TEST(FilterProcessorTest, ExcludesRecordWhenAnyLabelMatchesExcludePair) {
    FilterProcessor proc;
    ConfigValue cfg;
    cfg.Set("exclude_label_key", "cpu");
    cfg.Set("exclude_label_value", "cpu0");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

    auto& ok_rec = batch->AddRecord();
    ok_rec.labels.push_back({batch->InternString("host"), batch->InternString("h1")});

    auto& bad_rec = batch->AddRecord();
    bad_rec.labels.push_back({batch->InternString("host"), batch->InternString("h2")});
    bad_rec.labels.push_back({batch->InternString("cpu"), batch->InternString("cpu0")});

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto output = out.value();
    ASSERT_EQ(output->records().size(), 1u);
    EXPECT_EQ(output->records()[0].labels[0].key, batch->InternString("host"));
}

// 插件名称应为 filter，供管道配置解析使用。
TEST(FilterProcessorTest, ReportsExpectedName) {
    FilterProcessor proc;
    EXPECT_STREQ(proc.Name(), "filter");
}

}  // namespace
}  // namespace illuminator
