#include <gtest/gtest.h>
#include "core/common/yaml_config_loader.h"

namespace illuminator {
TEST(YamlConfigTest, LoadsCoreSettingsWithoutApplicationLayer) {
    auto result = YamlConfigLoader::LoadFromString(R"(
global:
  log_level: debug
  data_dir: /tmp/core-data
  plugin_dirs: [/tmp/plugins]
engine:
  collect_pool_threads: 3
  sink_pool_threads: 2
  channel:
    size: small
    drop_policy: drop_oldest
    backpressure_high: 0.7
    backpressure_low: 0.1
)");
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().log_level, "debug");
    EXPECT_EQ(result.value().data_dir, "/tmp/core-data");
    ASSERT_EQ(result.value().plugin_dirs.size(), 1u);
    EXPECT_EQ(result.value().plugin_dirs[0], "/tmp/plugins");
    EXPECT_EQ(result.value().engine.collect_pool_threads, 3u);
    EXPECT_EQ(result.value().engine.sink_pool_threads, 2u);
    EXPECT_EQ(result.value().engine.channel.size, "small");
    EXPECT_EQ(result.value().engine.channel.drop_policy, "drop_oldest");
    EXPECT_DOUBLE_EQ(result.value().engine.channel.backpressure_high, 0.7);
    EXPECT_DOUBLE_EQ(result.value().engine.channel.backpressure_low, 0.1);
}

TEST(YamlConfigTest, RejectsInvalidCoreConfiguration) {
    for (const char* invalid : {
            "global: {log_level: invalid}",
            "engine: {channel: {size: enormous}}",
            "engine: {channel: {drop_policy: ignore}}",
            "engine: {channel: {backpressure_high: 0.1, backpressure_low: 0.9}}",
            "engine: {channel: {backpressure_high: 1.1}}",
            "global: [broken"}) {
        EXPECT_FALSE(YamlConfigLoader::LoadFromString(invalid).ok()) << invalid;
    }
}
}  // namespace illuminator
