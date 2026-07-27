// ============================================================================
// CpuUtilizationSource 单元测试
// ============================================================================
//
// 测试覆盖范围：
//   1. Init 配置解析（默认值、自定义值）
//   2. Collect 首次调用返回空批次（差分需要前值）
//   3. Collect 第二次调用返回包含指标的批次
//   4. 系统总计 Record 的字段完整性校验
//   5. Per-Core 数据采集开关
//   6. Reconfigure 热更新
//   7. 数值范围合理性（0~100%）
//   8. 系统计数器非负
//   9. Load Average 非负
// ============================================================================

#include "plugin/features/cpu/cpu_utilization/cpu_utilization_source.h"

#include <gtest/gtest.h>
#include <string>
#include <thread>

namespace illuminator {
namespace {

// ============================================================================
// 辅助函数：从 Record 的 labels 中查找指定 key 的 value
// ============================================================================
std::string_view FindLabel(const Record& rec, std::string_view key) {
    for (const auto& label : rec.labels) {
        if (label.key == key) return label.value;
    }
    return {};
}

// ============================================================================
// 辅助函数：从 Record 的 fields 中获取 double 字段值
// ============================================================================
double GetDouble(const Record& rec, std::string_view key) {
    const auto* fv = rec.GetField(key);
    if (!fv) return -999.0;
    if (auto* d = std::get_if<double>(fv)) return *d;
    if (auto* u = std::get_if<uint64_t>(fv)) return static_cast<double>(*u);
    if (auto* i = std::get_if<int64_t>(fv)) return static_cast<double>(*i);
    return -999.0;
}

// ============================================================================
// 辅助函数：按 type label 过滤 Record
// ============================================================================
std::vector<const Record*> FilterByType(const DataBatch& batch, std::string_view type) {
    std::vector<const Record*> result;
    for (const auto& rec : batch.records()) {
        if (FindLabel(rec, "type") == type) {
            result.push_back(&rec);
        }
    }
    return result;
}

// ============================================================================
// 测试 1：默认配置初始化
// ============================================================================
TEST(CpuUtilizationSourceTest, DefaultInit) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    auto status = source.Init(cfg);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(source.IntervalMs(), 1000u);
    EXPECT_STREQ(source.Name(), "cpu_utilization");
}

// ============================================================================
// 测试 2：自定义配置初始化
// ============================================================================
TEST(CpuUtilizationSourceTest, CustomInit) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("interval_ms", int64_t(2000));
    cfg.Set("collect_per_core", "false");
    auto status = source.Init(cfg);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(source.IntervalMs(), 2000u);
}

// ============================================================================
// 测试 3：首次 Collect 返回空批次（差分计算需要两次快照）
// ============================================================================
TEST(CpuUtilizationSourceTest, FirstCollectReturnsEmpty) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    source.Init(cfg);

    auto result = source.Collect();
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value()->Empty());
}

// ============================================================================
// 测试 4：第二次 Collect 返回包含系统总计的批次
// ============================================================================
TEST(CpuUtilizationSourceTest, SecondCollectHasSystemTotal) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("collect_per_core", "false");
    source.Init(cfg);

    source.Collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto r2 = source.Collect();
    ASSERT_TRUE(r2.ok());
    auto& batch = *r2.value();

    auto totals = FilterByType(batch, "system_total");
    ASSERT_GE(totals.size(), 1u) << "应包含至少一条 system_total 记录";

    const auto& rec = *totals[0];
    EXPECT_NE(rec.GetField("user_pct"), nullptr);
    EXPECT_NE(rec.GetField("system_pct"), nullptr);
    EXPECT_NE(rec.GetField("idle_pct"), nullptr);
    EXPECT_NE(rec.GetField("busy_pct"), nullptr);
    EXPECT_NE(rec.GetField("load_1m"), nullptr);
    EXPECT_NE(rec.GetField("load_5m"), nullptr);
    EXPECT_NE(rec.GetField("load_15m"), nullptr);
}

// ============================================================================
// 测试 5：CPU 百分比值在合理范围内（0~100）
// ============================================================================
TEST(CpuUtilizationSourceTest, PercentagesInRange) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("collect_per_core", "false");
    source.Init(cfg);

    source.Collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto result = source.Collect();
    ASSERT_TRUE(result.ok());

    auto totals = FilterByType(*result.value(), "system_total");
    if (totals.empty()) GTEST_SKIP() << "无法在此环境获取 CPU 数据";

    const auto& rec = *totals[0];
    double busy = GetDouble(rec, "busy_pct");
    double idle = GetDouble(rec, "idle_pct");
    double user = GetDouble(rec, "user_pct");

    EXPECT_GE(busy, -0.1);
    EXPECT_LE(busy, 100.1);
    EXPECT_GE(idle, -0.1);
    EXPECT_LE(idle, 100.1);
    EXPECT_GE(user, -0.1);
    EXPECT_LE(user, 100.1);

    double iowait = GetDouble(rec, "iowait_pct");
    if (iowait > -100) {
        double sum = busy + idle + iowait;
        EXPECT_NEAR(sum, 100.0, 1.0) << "busy + idle + iowait 应约等于 100%";
    }
}

// ============================================================================
// 测试 6：Per-Core 数据开关
// ============================================================================
TEST(CpuUtilizationSourceTest, PerCoreToggle) {
    // 开启 per-core
    {
        CpuUtilizationSource source;
        ConfigValue cfg;
        cfg.Set("collect_per_core", "true");
        source.Init(cfg);

        source.Collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto result = source.Collect();
        ASSERT_TRUE(result.ok());

        auto cores = FilterByType(*result.value(), "cpu_core");
        EXPECT_GE(cores.size(), 1u);
    }

    // 关闭 per-core
    {
        CpuUtilizationSource source;
        ConfigValue cfg;
        cfg.Set("collect_per_core", "false");
        source.Init(cfg);

        source.Collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto result = source.Collect();
        ASSERT_TRUE(result.ok());

        auto cores = FilterByType(*result.value(), "cpu_core");
        EXPECT_EQ(cores.size(), 0u) << "关闭 per-core 后不应有 cpu_core 记录";
    }
}

// ============================================================================
// 测试 7：Reconfigure 热更新
// ============================================================================
TEST(CpuUtilizationSourceTest, Reconfigure) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    source.Init(cfg);
    EXPECT_EQ(source.IntervalMs(), 1000u);

    ConfigValue new_cfg;
    new_cfg.Set("interval_ms", int64_t(2000));
    auto status = source.Reconfigure(new_cfg);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(source.IntervalMs(), 2000u);
}

// ============================================================================
// 测试 8：系统计数器速率非负
// ============================================================================
TEST(CpuUtilizationSourceTest, SystemCountersNonNegative) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("collect_per_core", "false");
    source.Init(cfg);

    source.Collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto result = source.Collect();
    ASSERT_TRUE(result.ok());

    auto totals = FilterByType(*result.value(), "system_total");
    if (totals.empty()) GTEST_SKIP();

    const auto& rec = *totals[0];
    double ctxt = GetDouble(rec, "ctxt_per_sec");
    double intr = GetDouble(rec, "intr_per_sec");

    EXPECT_GE(ctxt, 0.0) << "上下文切换速率应 >= 0";
    EXPECT_GE(intr, 0.0) << "中断速率应 >= 0";
}

// ============================================================================
// 测试 9：Load Average 值非负
// ============================================================================
TEST(CpuUtilizationSourceTest, LoadAverageNonNegative) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("collect_per_core", "false");
    source.Init(cfg);

    source.Collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto result = source.Collect();
    ASSERT_TRUE(result.ok());

    auto totals = FilterByType(*result.value(), "system_total");
    if (totals.empty()) GTEST_SKIP();

    const auto& rec = *totals[0];
    EXPECT_GE(GetDouble(rec, "load_1m"), 0.0);
    EXPECT_GE(GetDouble(rec, "load_5m"), 0.0);
    EXPECT_GE(GetDouble(rec, "load_15m"), 0.0);
}

// ============================================================================
// 测试 10：不包含进程级数据（已拆分到 process_cpu）
// ============================================================================
TEST(CpuUtilizationSourceTest, NoProcessRecords) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    source.Init(cfg);

    source.Collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto result = source.Collect();
    ASSERT_TRUE(result.ok());

    auto procs = FilterByType(*result.value(), "process");
    EXPECT_EQ(procs.size(), 0u) << "cpu_utilization 不应包含 process 类型的记录";
}

}  // namespace
}  // namespace illuminator
