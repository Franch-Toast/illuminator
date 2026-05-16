// DataBatch 单元测试：批次类型、记录/堆栈采样、Arena 字符串内部化、元数据与字段值类型。

#include "core/engine/data_batch.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace illuminator {
namespace {

// 默认构造函数应产出 kGeneric 批次类型。
TEST(DataBatchTest, DefaultConstructionUsesGenericType) {
    DataBatch batch;
    EXPECT_EQ(batch.type(), DataBatch::Type::kGeneric);
}

// 显式指定批次类型时应正确保存。
TEST(DataBatchTest, ConstructionPreservesExplicitType) {
    DataBatch m{DataBatch::Type::kMetrics};
    DataBatch p{DataBatch::Type::kProfile};
    DataBatch t{DataBatch::Type::kTrace};
    DataBatch l{DataBatch::Type::kLog};
    DataBatch g{DataBatch::Type::kGeneric};

    EXPECT_EQ(m.type(), DataBatch::Type::kMetrics);
    EXPECT_EQ(p.type(), DataBatch::Type::kProfile);
    EXPECT_EQ(t.type(), DataBatch::Type::kTrace);
    EXPECT_EQ(l.type(), DataBatch::Type::kLog);
    EXPECT_EQ(g.type(), DataBatch::Type::kGeneric);
}

// 使用外部共享 Arena 时，多个批次应指向同一块 Arena 内存。
TEST(DataBatchTest, SharedArenaConstructionUsesSameAllocator) {
    auto arena = std::make_shared<Arena>();
    DataBatch a(DataBatch::Type::kTrace, arena);
    DataBatch b(DataBatch::Type::kLog, arena);

    EXPECT_EQ(&a.arena(), &b.arena());
    EXPECT_EQ(&a.arena(), arena.get());
}

// AddRecord：新记录的自动时间戳应在调用时刻附近。
TEST(DataBatchTest, AddRecordSetsTimestampAutomatically) {
    DataBatch batch;
    const Timestamp before = NowTimestamp();
    Record& rec = batch.AddRecord();
    const Timestamp after = NowTimestamp();

    EXPECT_GE(rec.timestamp, before);
    EXPECT_LE(rec.timestamp, after);
    EXPECT_GT(TimestampToNanos(rec.timestamp), 0u);  // 纳秒纪元值应为正
    EXPECT_EQ(batch.records().size(), 1u);
}

// AddStackSample：新采样的自动时间戳应在调用时刻附近。
TEST(DataBatchTest, AddStackSampleSetsTimestampAutomatically) {
    DataBatch batch;
    const Timestamp before = NowTimestamp();
    StackSample& s = batch.AddStackSample();
    const Timestamp after = NowTimestamp();

    EXPECT_GE(s.timestamp, before);
    EXPECT_LE(s.timestamp, after);
    EXPECT_GT(TimestampToNanos(s.timestamp), 0u);
    EXPECT_EQ(batch.stack_samples().size(), 1u);
}

// InternString：应将内容复制到 Arena，源缓冲区修改不影响内部化视图。
TEST(DataBatchTest, InternStringCopiesIntoArena) {
    DataBatch batch;
    std::string mutable_src = "original-payload";
    const std::string_view interned = batch.InternString(mutable_src);

    mutable_src.assign(32, '#');
    EXPECT_EQ(interned, "original-payload");
}

// InternString：返回的 string_view 在原 std::string 析构后仍有效（生命周期绑定 Arena）。
TEST(DataBatchTest, InternStringViewOutlivesSourceStringLifetime) {
    DataBatch batch;
    std::optional<std::string_view> held;

    {
        std::string ephemeral = "must-survive-past-scope";
        held = batch.InternString(ephemeral);
    }

    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(*held, "must-survive-past-scope");
}

// Size：记录与堆栈采样数量之和。
TEST(DataBatchTest, SizeCountsRecordsAndStackSamplesTogether) {
    DataBatch batch;
    EXPECT_EQ(batch.Size(), 0u);

    batch.AddRecord();
    batch.AddRecord();
    batch.AddStackSample();
    EXPECT_EQ(batch.Size(), 3u);

    batch.stack_samples().emplace_back();  // 直接插入也应计入总数
    EXPECT_EQ(batch.Size(), 4u);
}

// Empty：记录在两侧皆空时为真。
TEST(DataBatchTest, EmptyTrueOnlyWhenBothVectorsEmpty) {
    DataBatch batch;
    EXPECT_TRUE(batch.Empty());

    batch.AddRecord();
    EXPECT_FALSE(batch.Empty());
    batch.Clear();
    EXPECT_TRUE(batch.Empty());

    batch.AddStackSample();
    EXPECT_FALSE(batch.Empty());
}

// Clear：清空向量并重置 Arena（已分配量归零）。
TEST(DataBatchTest, ClearRemovesPayloadAndResetsArena) {
    DataBatch batch;

    batch.AddRecord();
    batch.AddStackSample();
    (void)batch.InternString("interned-before-clear");

    EXPECT_FALSE(batch.Empty());
    EXPECT_GT(batch.arena().TotalAllocated(), 0u);

    batch.Clear();

    EXPECT_TRUE(batch.records().empty());
    EXPECT_TRUE(batch.stack_samples().empty());
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(batch.arena().TotalAllocated(), 0u);
}

// SetMeta/GetMeta：元数据的写入与读出。
TEST(DataBatchTest, SetMetaAndGetMetaRoundTripStrings) {
    DataBatch batch;
    batch.SetMeta("pipe", "cpu_util");
    batch.SetMeta("interval_ms", "1000");

    EXPECT_EQ(batch.GetMeta("pipe"), "cpu_util");
    EXPECT_EQ(batch.GetMeta("interval_ms"), "1000");
}

// GetMeta：键不存在时应返回默认值。
TEST(DataBatchTest, GetMetaReturnsDefaultWhenKeyMissing) {
    const DataBatch batch;
    EXPECT_EQ(batch.GetMeta("no_such_key", "fallback"), "fallback");
}

// Record::SetField/GetField：基本读写与缺失返回 nullptr。
TEST(DataBatchTest, RecordSetFieldAndGetFieldReadWriteKeys) {
    DataBatch batch;
    Record& r = batch.AddRecord();

    const std::string_view k = batch.InternString("temperature_c");
    r.SetField(k, FieldValue{int64_t{42}});

    const FieldValue* got = r.GetField(k);
    ASSERT_NE(got, nullptr);
    ASSERT_TRUE(std::holds_alternative<int64_t>(*got));
    EXPECT_EQ(std::get<int64_t>(*got), 42);

    const std::string_view missing_key = batch.InternString("absent");
    EXPECT_EQ(r.GetField(missing_key), nullptr);
}

// Label：键值均应通过 InternString 引用 Arena；Record 仍可正确保存键值语义。
TEST(DataBatchTest, LabelsUseInternStringForStableKeyValueViews) {
    DataBatch batch;
    Record& r = batch.AddRecord();

    auto k = batch.InternString("host");
    auto v = batch.InternString("edge-07");
    r.labels.push_back(Label{k, v});

    ASSERT_EQ(r.labels.size(), 1u);
    EXPECT_EQ(r.labels[0].key, k);
    EXPECT_EQ(r.labels[0].value, v);
}

// FieldValue：bool/int64_t/uint64_t/double/string_view 均能存取。
TEST(DataBatchTest, FieldValueStoresAndRetrievesAllSupportedTypes) {
    DataBatch batch;
    Record& r = batch.AddRecord();

    const std::string_view kb = batch.InternString("b");
    const std::string_view ki64 = batch.InternString("i64");
    const std::string_view ku64 = batch.InternString("u64");
    const std::string_view kdbl = batch.InternString("dbl");
    const std::string_view ksv = batch.InternString("sv");
    const std::string_view vs = batch.InternString("hello-view");

    r.SetField(kb, FieldValue{true});
    r.SetField(ki64, FieldValue{int64_t{-7}});
    r.SetField(ku64, FieldValue{uint64_t{999}});
    r.SetField(kdbl, FieldValue{3.25});
    r.SetField(ksv, FieldValue{vs});

    ASSERT_TRUE(std::holds_alternative<bool>(*r.GetField(kb)));
    EXPECT_TRUE(std::get<bool>(*r.GetField(kb)));

    ASSERT_TRUE(std::holds_alternative<int64_t>(*r.GetField(ki64)));
    EXPECT_EQ(std::get<int64_t>(*r.GetField(ki64)), -7);

    ASSERT_TRUE(std::holds_alternative<uint64_t>(*r.GetField(ku64)));
    EXPECT_EQ(std::get<uint64_t>(*r.GetField(ku64)), 999u);

    ASSERT_TRUE(std::holds_alternative<double>(*r.GetField(kdbl)));
    EXPECT_DOUBLE_EQ(std::get<double>(*r.GetField(kdbl)), 3.25);

    ASSERT_TRUE(std::holds_alternative<std::string_view>(*r.GetField(ksv)));
    EXPECT_EQ(std::get<std::string_view>(*r.GetField(ksv)), vs);
}

// NowTimestamp 与 TimestampToNanos：时间上单调且不崩溃；纳秒量级应随短暂休眠增长。
TEST(DataBatchTest, NowTimestampAndTimestampToNanosIncreaseOverSleepWindow) {
    const Timestamp a = NowTimestamp();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const Timestamp b = NowTimestamp();

    EXPECT_GE(TimestampToNanos(b), TimestampToNanos(a));

    constexpr uint64_t kNanosPerSec = UINT64_C(1'000'000'000);
    const uint64_t delta = TimestampToNanos(b) - TimestampToNanos(a);
    // 下限：至少覆盖几个毫秒量级（避免因调度抖动导致误判）。
    EXPECT_GE(delta, kNanosPerSec / 600);  // > ~1.6ms
}

}  // namespace
}  // namespace illuminator
