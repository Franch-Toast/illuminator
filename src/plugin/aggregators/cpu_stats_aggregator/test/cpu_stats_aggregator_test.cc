// CpuStatsAggregator 单元测试 — 校验窗口配置、聚合统计与 profile 合并行为

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "plugin/aggregators/cpu_stats_aggregator/cpu_stats_aggregator.h"
#include "core/common/config.h"
#include "core/common/data_batch.h"

namespace illuminator {
namespace {

// 将 FieldValue 中的数值读出为 double（测试断言用）。
std::optional<double> FieldAsDouble(const FieldValue& fv) {
    if (auto* d = std::get_if<double>(&fv)) return *d;
    if (auto* u = std::get_if<uint64_t>(&fv)) return static_cast<double>(*u);
    if (auto* i = std::get_if<int64_t>(&fv)) return static_cast<double>(*i);
    return std::nullopt;
}

// 在 Record 中取字段的数值；不存在则nullopt。
std::optional<double> GetNumericField(const Record& rec,
                                      const DataBatchPtr& owns_keys,
                                      std::string_view key) {
    const FieldValue* p = rec.GetField(owns_keys->InternString(key));
    if (!p) return std::nullopt;
    return FieldAsDouble(*p);
}

constexpr double kEps = 1e-9;

// ---- Init / 元数据 ----

// Init应解析 window_sec 并换算为 FlushIntervalMs（毫秒）。
TEST(CpuStatsAggregatorTest, InitParsesWindowSecIntoFlushIntervalMs) {
    CpuStatsAggregator agg;
    ConfigValue cfg;
    cfg.Set("window_sec", int64_t{7});
    ASSERT_TRUE(agg.Init(cfg).ok());
    EXPECT_EQ(agg.FlushIntervalMs(), 7000u);
}

// window_sec 为 0 时实现会回退为默认 10s 窗口。
TEST(CpuStatsAggregatorTest, InitUsesDefaultWindowWhenWindowSecZero) {
    CpuStatsAggregator agg;
    ConfigValue cfg;
    cfg.Set("window_sec", int64_t{0});
    ASSERT_TRUE(agg.Init(cfg).ok());
    EXPECT_EQ(agg.FlushIntervalMs(), 10000u);
}

// Name() / Version() 与插件契约一致。
TEST(CpuStatsAggregatorTest, ReportsCorrectPluginNameAndVersion) {
    CpuStatsAggregator agg;
    EXPECT_STREQ(agg.Name(), "cpu_stats_aggregator");
    EXPECT_STREQ(agg.Version(), "0.2.0");
}

// ---- Add / Flush 边界 ----

// 空 batch（无记录、无栈）追加后 Flush 不产生任何输出批次。
TEST(CpuStatsAggregatorTest, AddEmptyBatchThenFlushProducesNoOutputBatches) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    ASSERT_TRUE(agg.Add(batch).ok());

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().empty());
}

// Add(nullptr) 应安全返回 Ok，不抛出、不写入缓冲。
TEST(CpuStatsAggregatorTest, AddAcceptsNullBatchAsNoOp) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());
    EXPECT_TRUE(agg.Add(nullptr).ok());
}

// 单字段时间序列 Flush 后应得到 avg/min/max/count；样本不足 4 条时不含 p50/p99。
TEST(CpuStatsAggregatorTest, FlushProducesAvgMinMaxCountForRecordedFieldValues) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    for (double load : {10.0, 30.0, 20.0}) {
        auto in = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& r = in->AddRecord();
        r.labels.push_back({in->InternString("host"), in->InternString("a")});
        r.SetField(in->InternString("load"), load);
        ASSERT_TRUE(agg.Add(in).ok());
    }

    StatusOr<std::vector<DataBatchPtr>> flushed = agg.Flush();
    ASSERT_TRUE(flushed.ok());
    ASSERT_EQ(flushed.value().size(), 1u);
    const DataBatchPtr& metrics = flushed.value().front();
    ASSERT_EQ(metrics->type(), DataBatch::Type::kMetrics);
    ASSERT_EQ(metrics->records().size(), 1u);

    const Record& out_rec = metrics->records().front();
    ASSERT_NEAR(GetNumericField(out_rec, metrics, "load_avg").value_or(-1), 20.0, kEps);
    ASSERT_NEAR(GetNumericField(out_rec, metrics, "load_min").value_or(-1), 10.0, kEps);
    ASSERT_NEAR(GetNumericField(out_rec, metrics, "load_max").value_or(-1), 30.0, kEps);
    EXPECT_EQ(static_cast<uint64_t>(GetNumericField(out_rec, metrics, "load_count").value_or(-1)), 3u);
    EXPECT_FALSE(GetNumericField(out_rec, metrics, "load_p50").has_value());
    EXPECT_FALSE(GetNumericField(out_rec, metrics, "load_p99").has_value());
}

// 多条 batch、相同标签的序列应合并到同一时间序列后再统计。
TEST(CpuStatsAggregatorTest, FlushMergesMultipleBatchesWithSameLabels) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    auto b1 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& x = b1->AddRecord();
    x.labels.push_back({b1->InternString("k"), b1->InternString("v")});
    x.SetField(b1->InternString("t"), double{100.0});
    ASSERT_TRUE(agg.Add(b1).ok());

    auto b2 = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& y = b2->AddRecord();
    y.labels.push_back({b2->InternString("k"), b2->InternString("v")});
    y.SetField(b2->InternString("t"), double{200.0});
    ASSERT_TRUE(agg.Add(b2).ok());

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 1u);
    const DataBatchPtr& m = out.value().front();
    const Record& rec = m->records().front();
    ASSERT_NEAR(GetNumericField(rec, m, "t_avg").value_or(0), 150.0, kEps);
    EXPECT_EQ(static_cast<uint64_t>(GetNumericField(rec, m, "t_count").value_or(-1)), 2u);
}

// 样本数 >= 4 时输出百分位字段 p50 / p99（与 ComputeStats 线性插值一致）。
TEST(CpuStatsAggregatorTest, FlushAddsP50AndP99WhenSampleCountAtLeastFour) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    std::vector<double> samples = {1.0, 2.0, 3.0, 4.0};
    for (double v : samples) {
        auto b = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& r = b->AddRecord();
        r.labels.push_back({b->InternString("q"), b->InternString("1")});
        r.SetField(b->InternString("x"), v);
        ASSERT_TRUE(agg.Add(b).ok());
    }

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 1u);
    const DataBatchPtr& m = out.value().front();
    const Record& rec = m->records().front();

    // 序列 [1,2,3,4]：p50 idx=1.5 -> 2.5；p99 idx=2.97 -> 线性插值为 3.97
    ASSERT_NEAR(GetNumericField(rec, m, "x_p50").value_or(-1), 2.5, kEps);
    ASSERT_NEAR(GetNumericField(rec, m, "x_p99").value_or(-1), 3.97, kEps);
}

// 首次 Flush 输出后再次 Flush，缓冲已清空故结果为空向量。
TEST(CpuStatsAggregatorTest, SecondFlushAfterSuccessfulFlushReturnsEmpty) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    auto& r = b->AddRecord();
    r.SetField(b->InternString("x"), double{1.0});
    ASSERT_TRUE(agg.Add(b).ok());

    StatusOr<std::vector<DataBatchPtr>> first = agg.Flush();
    ASSERT_TRUE(first.ok());
    ASSERT_FALSE(first.value().empty());

    StatusOr<std::vector<DataBatchPtr>> second = agg.Flush();
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().empty());
}

// ---- StackSample ----

// 输入 profile 采样后 Flush 应产生 kProfile 批次并保留栈结构。
TEST(CpuStatsAggregatorTest, FlushProducesMergedProfileBatchFromStackSamples) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = b->AddStackSample();
    s.pid = 100;
    s.tid = 200;
    s.comm = b->InternString("myproc");
    s.count = 2;
    StackFrame cf;
    cf.address = 0x1000;
    cf.function_name = b->InternString("callee");
    StackFrame pf;
    pf.address = 0x2000;
    pf.function_name = b->InternString("caller");
    // 自下而上：caller -> callee
    s.user_stack.push_back(cf);
    s.user_stack.push_back(pf);
    ASSERT_TRUE(agg.Add(b).ok());

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 1u);
    const DataBatchPtr& pb = out.value().front();
    ASSERT_EQ(pb->type(), DataBatch::Type::kProfile);
    ASSERT_EQ(pb->stack_samples().size(), 1u);
    const StackSample& out_s = pb->stack_samples().front();
    EXPECT_EQ(out_s.pid, 100u);
    EXPECT_EQ(out_s.tid, 200u);
    EXPECT_EQ(out_s.count, 2u);
    EXPECT_EQ(out_s.comm, pb->InternString("myproc"));
    ASSERT_EQ(out_s.user_stack.size(), 2u);
}

// 相同 PID/TID 与栈地址序列的 StackSample 应累加 count。
TEST(CpuStatsAggregatorTest, AccumulatesCountForMatchingStackKeys) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    auto make_batch = [&](uint64_t cnt) {
        auto b = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        auto& s = b->AddStackSample();
        s.pid = 1;
        s.tid = 2;
        s.comm = b->InternString("t");
        s.count = cnt;
        StackFrame f;
        f.address = 0xabc;
        f.function_name = b->InternString("f");
        s.user_stack.push_back(f);
        return b;
    };

    ASSERT_TRUE(agg.Add(make_batch(3)).ok());
    ASSERT_TRUE(agg.Add(make_batch(5)).ok());

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 1u);
    EXPECT_EQ(out.value().front()->stack_samples().front().count, 8u);
}

// 同时具备 Record 与 StackSample 时应返回两个批次（metrics 与 profile）。
TEST(CpuStatsAggregatorTest, FlushOutputsMetricsAndProfileWhenBothPresent) {
    CpuStatsAggregator agg;
    ASSERT_TRUE(agg.Init(ConfigValue{}).ok());

    auto b = std::make_shared<DataBatch>(DataBatch::Type::kGeneric);
    auto& r = b->AddRecord();
    r.SetField(b->InternString("m"), double{2.0});
    auto& s = b->AddStackSample();
    s.pid = 3;
    s.tid = 4;
    s.comm = b->InternString("p");
    StackFrame fr;
    fr.address = 1;
    s.user_stack.push_back(fr);
    ASSERT_TRUE(agg.Add(b).ok());

    StatusOr<std::vector<DataBatchPtr>> out = agg.Flush();
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value().size(), 2u);
    EXPECT_EQ(out.value()[0]->type(), DataBatch::Type::kMetrics);
    EXPECT_EQ(out.value()[1]->type(), DataBatch::Type::kProfile);
}

}  // namespace
}  // namespace illuminator
