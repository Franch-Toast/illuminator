// otlp_export_sink_test.cc — OtlpExportSink 单元测试
// 验证 OTLP JSON 载荷构建、stub 标识和插件元数据。

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "plugin/sinks/otlp_export/otlp_export_sink.h"

namespace illuminator {
namespace {

// Init 应解析 endpoint 配置。
TEST(OtlpExportSinkTest, InitParsesEndpointConfig) {
    OtlpExportSink sink;
    ConfigValue cfg;
    cfg.Set("endpoint", "http://otel:4318/v1/metrics");
    ASSERT_TRUE(sink.Init(cfg).ok());
}

// IsStub 应返回 true 表示 HTTP 发送尚未实现。
TEST(OtlpExportSinkTest, IsStubReturnsTrueIndicatingHttpNotImplemented) {
    OtlpExportSink sink;
    EXPECT_TRUE(sink.IsStub());
}

// Write 含 Record 后 LastPayload 应包含 resourceMetrics 结构。
TEST(OtlpExportSinkTest, WriteProducesOtlpJsonPayloadWithResourceMetrics) {
    OtlpExportSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("host"), batch->InternString("n1")});
    rec.SetField(batch->InternString("cpu_pct"), double{42.5});

    ASSERT_TRUE(sink.Write(batch).ok());

    const std::string& payload = sink.LastPayload();
    EXPECT_NE(payload.find("resourceMetrics"), std::string::npos);
    EXPECT_NE(payload.find("scopeMetrics"), std::string::npos);
    EXPECT_NE(payload.find("cpu_pct"), std::string::npos);
    EXPECT_NE(payload.find("42.5"), std::string::npos);
    EXPECT_NE(payload.find("host"), std::string::npos);
    EXPECT_NE(payload.find("n1"), std::string::npos);
}

// PayloadsBuilt 计数器应随 Write 调用递增。
TEST(OtlpExportSinkTest, PayloadsBuiltCounterIncrementsOnEachWrite) {
    OtlpExportSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    EXPECT_EQ(sink.PayloadsBuilt(), 0u);

    auto b1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    b1->AddRecord().SetField(b1->InternString("x"), double{1.0});
    ASSERT_TRUE(sink.Write(b1).ok());
    EXPECT_EQ(sink.PayloadsBuilt(), 1u);

    auto b2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    b2->AddRecord().SetField(b2->InternString("y"), double{2.0});
    ASSERT_TRUE(sink.Write(b2).ok());
    EXPECT_EQ(sink.PayloadsBuilt(), 2u);
}

// 空 batch 和 nullptr 不应增加 PayloadsBuilt。
TEST(OtlpExportSinkTest, EmptyAndNullBatchDoNotIncrementCounter) {
    OtlpExportSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    ASSERT_TRUE(sink.Write(nullptr).ok());
    ASSERT_TRUE(sink.Write(std::make_shared<DataBatch>()).ok());
    EXPECT_EQ(sink.PayloadsBuilt(), 0u);
}

// OTLP JSON 中应包含 attributes 列表，反映 Record 的 labels。
TEST(OtlpExportSinkTest, PayloadContainsAttributesFromRecordLabels) {
    OtlpExportSink sink;
    ASSERT_TRUE(sink.Init(ConfigValue{}).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("env"), batch->InternString("prod")});
    rec.SetField(batch->InternString("latency"), double{1.5});
    ASSERT_TRUE(sink.Write(batch).ok());

    const std::string& p = sink.LastPayload();
    EXPECT_NE(p.find("attributes"), std::string::npos);
    EXPECT_NE(p.find("stringValue"), std::string::npos);
    EXPECT_NE(p.find("prod"), std::string::npos);
}

// 插件名称应为 otlp_export。
TEST(OtlpExportSinkTest, PluginNameMatchesRegistration) {
    OtlpExportSink sink;
    EXPECT_STREQ(sink.Name(), "otlp_export");
    EXPECT_STREQ(sink.Version(), "0.1.0");
}

}  // namespace
}  // namespace illuminator
