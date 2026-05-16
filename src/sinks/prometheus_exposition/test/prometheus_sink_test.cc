// PrometheusSink 单元测试 — 校验 prefix 初始化、Expose 文本与标签转义/Sanitize 行为。

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "sinks/prometheus_exposition/prometheus_sink.h"

namespace illuminator {
namespace {

// Init 应从配置读取 prefix。
TEST(PrometheusSinkTest, InitParsesPrefixConfig) {
    PrometheusSink sink;
    ConfigValue cfg;
    cfg.Set("prefix", "custom_metrics");
    ASSERT_TRUE(sink.Init(cfg).ok());
    // 前缀通过 Expose 中的指标名体现（见下文 Write+Expose 用例）。
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("x"), double{1.0});
    ASSERT_TRUE(sink.Write(batch).ok());

    std::string out = sink.Expose();
    EXPECT_NE(out.find("custom_metrics_x"), std::string::npos);
}

// Write 后跟 Expose 应输出 Prometheus 文本（HELP/TYPE/data 三段结构）。
TEST(PrometheusSinkTest, ExposeProducesPrometheusExpositionLines) {
    PrometheusSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("cpu_usage"), double{0.875});
    ASSERT_TRUE(sink.Write(batch).ok());

    std::string txt = sink.Expose();
    EXPECT_NE(txt.find("# HELP "), std::string::npos);
    EXPECT_NE(txt.find("# TYPE "), std::string::npos);
    EXPECT_NE(txt.find(" gauge\n"), std::string::npos);
    // 默认 prefix 与字段名连成合法指标前缀
    EXPECT_NE(txt.find("illuminator_cpu_usage"), std::string::npos);
}

// Sanitize：字段名中非字母数字且非下划线的字符应变为下划线。
TEST(PrometheusSinkTest, MetricNameSanitizesSpecialCharacters) {
    PrometheusSink sink;
    ConfigValue cfg;
    cfg.Set("prefix", "pfx");
    ASSERT_TRUE(sink.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("bad@key#mode"), double{99.0});
    ASSERT_TRUE(sink.Write(batch).ok());

    std::string expose = sink.Expose();
    // "bad@key#mode" -> bad_key_mode（非法字符均被替换）
    EXPECT_NE(expose.find("pfx_bad_key_mode"), std::string::npos);
}

// 标签应按 {k="v",...} 形式输出并对引号换行等进行转义。
TEST(PrometheusSinkTest, LabelsAreFormattedAndEscapedCorrectly) {
    PrometheusSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("job"), batch->InternString("illuminator\"")});
    rec.labels.push_back({batch->InternString("instance"), batch->InternString("h1")});
    rec.SetField(batch->InternString("n"), double{3.14});
    ASSERT_TRUE(sink.Write(batch).ok());

    std::string out = sink.Expose();
    EXPECT_NE(out.find("job=\"illuminator\\\"\""), std::string::npos);
    EXPECT_NE(out.find("instance=\"h1\""), std::string::npos);
}

// 多次 Write 对同一 gauge 键应覆盖为新值。
TEST(PrometheusSinkTest, MultipleWritesUpdateGaugeValues) {
    PrometheusSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto b1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& r1 = b1->AddRecord();
    r1.labels.push_back({b1->InternString("host"), b1->InternString("n1")});
    r1.SetField(b1->InternString("temp"), double{10.5});
    ASSERT_TRUE(sink.Write(b1).ok());

    auto b2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& r2 = b2->AddRecord();
    r2.labels = r1.labels;
    r2.SetField(b2->InternString("temp"), double{42.125});
    ASSERT_TRUE(sink.Write(b2).ok());

    std::string exp = sink.Expose();
    EXPECT_NE(exp.find(" 42.125"), std::string::npos);
    EXPECT_EQ(exp.find(" 10.5"), std::string::npos);
}

// 空 batch 不应报错，且不改变先前已写入指标的 Expose。
TEST(PrometheusSinkTest, EmptyBatchWriteDoesNotAffectExistingMetricsExpose) {
    PrometheusSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto seeded = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    seeded->AddRecord().SetField(seeded->InternString("stay"), double{7.0});
    ASSERT_TRUE(sink.Write(seeded).ok());

    auto empty_records = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    ASSERT_TRUE(sink.Write(empty_records).ok());
    ASSERT_TRUE(sink.Write(nullptr).ok());

    EXPECT_NE(sink.Expose().find("stay"), std::string::npos);
}

// Name() 与插件标识一致。
TEST(PrometheusSinkTest, PluginNameMatchesSinkIdentifier) {
    PrometheusSink sink;
    EXPECT_STREQ(sink.Name(), "prometheus_exposition");
}

}  // namespace
}  // namespace illuminator
