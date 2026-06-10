#include "gtest/gtest.h"
#include "sources/cpu/cpu_utilization/cpu_utilization.h"

namespace illuminator {
namespace {

TEST(CpuUtilizationSourceTest, BasicProperties) {
    CpuUtilizationSource source;
    EXPECT_STREQ(source.Name(), "cpu_utilization");
    EXPECT_STREQ(source.Version(), "1.0.0");
    EXPECT_EQ(source.Type(), PluginType::kSource);
    EXPECT_FALSE(source.IsPushMode());
}

TEST(CpuUtilizationSourceTest, InitWithDefaults) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    auto status = source.Init(cfg);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(source.IntervalMs(), 1000u);
}

TEST(CpuUtilizationSourceTest, InitWithCustomInterval) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("interval_ms", int64_t{500});
    auto status = source.Init(cfg);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(source.IntervalMs(), 500u);
}

TEST(CpuUtilizationSourceTest, CollectReturnsValidBatch) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("interval_ms", int64_t{100});
    cfg.Set("collect_per_core", true);
    source.Init(cfg);

    // First collect primes prev_ state, second produces utilization data
    auto result1 = source.Collect();
    ASSERT_TRUE(result1.ok());

    auto result2 = source.Collect();
    ASSERT_TRUE(result2.ok());
    auto& batch = result2.value();
    EXPECT_FALSE(batch->records().empty());
}

TEST(CpuUtilizationSourceTest, CollectProducesLoadAvg) {
    CpuUtilizationSource source;
    ConfigValue cfg;
    cfg.Set("collect_per_core", false);
    source.Init(cfg);

    source.Collect();
    auto result = source.Collect();
    ASSERT_TRUE(result.ok());

    bool found_load = false;
    for (auto& rec : result.value()->records()) {
        for (auto& label : rec.labels) {
            if (label.key == "type" &&
                std::string(label.value) == "loadavg") {
                found_load = true;
            }
        }
    }
    EXPECT_TRUE(found_load);
}

}  // namespace
}  // namespace illuminator
