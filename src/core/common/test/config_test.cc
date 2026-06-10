// config_test.cc — 针对 core/common/config.h 中 ConfigValue 的单元测试

#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "core/common/config.h"

namespace illuminator {
namespace {

// 默认构造的 ConfigValue 应为空（IsNull）。
TEST(ConfigValueTest, DefaultConstructionIsEmpty) {
    ConfigValue c;
    EXPECT_TRUE(c.IsNull());
    EXPECT_EQ(c.AsString(), "");
    EXPECT_EQ(c.AsInt(), 0);
    EXPECT_EQ(c.AsDouble(), 0.0);
    EXPECT_FALSE(c.AsBool());
}

// 标量构造函数：int64_t、const char*、std::string 写入空键并可通过 As* 读取。
TEST(ConfigValueTest, ScalarConstructorsStorePrimitiveValues) {
    ConfigValue from_int(42);
    EXPECT_FALSE(from_int.IsNull());
    EXPECT_EQ(from_int.AsInt(), 42);

    ConfigValue from_cstr("hello");
    EXPECT_EQ(from_cstr.AsString(), "hello");

    ConfigValue from_str(std::string("world"));
    EXPECT_EQ(from_str.AsString(), "world");
}

// Set 与通过子键读取：基本字符串、C 字符串、整型写入后应能读回。
TEST(ConfigValueTest, SetAndReadScalarKeys) {
    ConfigValue c;
    c.Set("name", std::string("illuminator"));
    c.Set("host", "127.0.0.1");
    c.Set("port", static_cast<int64_t>(9527));

    EXPECT_EQ(c["name"].AsString(), "illuminator");
    EXPECT_EQ(c["host"].AsString(), "127.0.0.1");
    EXPECT_EQ(c["port"].AsInt(), 9527);
}

// 嵌套语义：使用点号扁平键名时，多层 operator[] 应能定位到叶子值。
TEST(ConfigValueTest, NestedKeysUseDotNotation) {
    ConfigValue c;
    c.Set("server.http.listen", "0.0.0.0:9527");

    ConfigValue server = c["server"];
    ConfigValue http = server["http"];
    EXPECT_EQ(http["listen"].AsString(), "0.0.0.0:9527");

    // 亦可链式访问
    EXPECT_EQ(c["server"]["http"]["listen"].AsString(), "0.0.0.0:9527");
}

// operator[]：既匹配精确键，也收集带前缀的子键，形成子 ConfigValue。
TEST(ConfigValueTest, SubscriptExtractsChildSection) {
    ConfigValue c;
    c.Set("app", "root");           // 与前缀 "app." 共存的精确键
    c.Set("app.name", "demo");
    c.Set("app.version", "1.0");

    ConfigValue app = c["app"];
    // 精确键 "app" 映射到空键标量
    EXPECT_EQ(app.AsString(), "root");
    EXPECT_EQ(app["name"].AsString(), "demo");
    EXPECT_EQ(app["version"].AsString(), "1.0");
}

// AsInt / AsDouble / AsBool / AsString 在合法字符串上应正确解析。
TEST(ConfigValueTest, TypedAccessorsParseValidStrings) {
    EXPECT_EQ(ConfigValue(7).AsInt(), 7);
    EXPECT_DOUBLE_EQ(ConfigValue(std::string("3.14")).AsDouble(), 3.14);
    EXPECT_TRUE(ConfigValue(std::string("true")).AsBool());
    EXPECT_TRUE(ConfigValue(std::string("1")).AsBool());
    EXPECT_FALSE(ConfigValue(std::string("false")).AsBool());
    EXPECT_EQ(ConfigValue(std::string("text")).AsString(), "text");
}

// 类型转换失败时应返回调用方提供的默认值。
TEST(ConfigValueTest, TypedAccessorsReturnDefaultOnConversionFailure) {
    ConfigValue bad_int(std::string("not_a_number"));
    EXPECT_EQ(bad_int.AsInt(-1), -1);

    ConfigValue bad_double(std::string("pi"));
    EXPECT_DOUBLE_EQ(bad_double.AsDouble(2.71), 2.71);

    ConfigValue bad_bool(std::string("maybe"));
    // 存在空键标量但值不是 "true"/"1" 时，AsBool 返回 false（与默认值参数无关）
    EXPECT_FALSE(bad_bool.AsBool(false));
    EXPECT_FALSE(bad_bool.AsBool(true));

    ConfigValue missing_bool;
    EXPECT_TRUE(missing_bool.AsBool(true));   // 无标量时返回默认值
    EXPECT_FALSE(missing_bool.AsBool(false));

    ConfigValue empty;
    EXPECT_EQ(empty.AsString("default-str"), "default-str");
}

// Raw() 应暴露底层 string→string 映射，便于遍历与校验。
TEST(ConfigValueTest, RawExposesUnderlyingMap) {
    ConfigValue c;
    c.Set("k1", "v1");
    c.Set("k2", "v2");

    const std::unordered_map<std::string, std::string>& raw = c.Raw();
    ASSERT_EQ(raw.size(), 2u);
    EXPECT_EQ(raw.at("k1"), "v1");
    EXPECT_EQ(raw.at("k2"), "v2");
}

}  // namespace
}  // namespace illuminator
