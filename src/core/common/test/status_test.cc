// status_test.cc — 针对 core/common/status.h 中 Status 与 StatusOr 的单元测试

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "core/common/status.h"

namespace illuminator {
namespace {

// 验证 Status 默认构造等价于成功状态（OK）。
TEST(StatusTest, DefaultConstructionIsOk) {
    Status s;
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kOk);
    EXPECT_TRUE(s.message().empty());
}

// 验证 Status::Ok() 工厂方法返回成功状态。
TEST(StatusTest, OkFactoryReturnsSuccess) {
    Status s = Status::Ok();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kOk);
}

// 验证 Status::Error 能正确携带错误码与消息。
TEST(StatusTest, ErrorFactorySetsCodeAndMessage) {
    const std::string msg = "参数不合法";
    Status s = Status::Error(StatusCode::kInvalidArgument, msg);

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(s.message(), msg);
}

// 覆盖常用 StatusCode 枚举值，确保可正常用于 Error 构造。
TEST(StatusTest, CommonStatusCodesAreUsable) {
    struct Case {
        StatusCode code;
        const char* msg;
    };
    const Case cases[] = {
        {StatusCode::kCancelled, "cancelled"},
        {StatusCode::kNotFound, "not found"},
        {StatusCode::kAlreadyExists, "exists"},
        {StatusCode::kPermissionDenied, "denied"},
        {StatusCode::kResourceExhausted, "exhausted"},
        {StatusCode::kInternal, "internal"},
        {StatusCode::kUnimplemented, "unimplemented"},
        {StatusCode::kUnavailable, "unavailable"},
        {StatusCode::kDataLoss, "data loss"},
    };

    for (const auto& c : cases) {
        Status s = Status::Error(c.code, c.msg);
        EXPECT_FALSE(s.ok());
        EXPECT_EQ(s.code(), c.code);
        EXPECT_EQ(s.message(), c.msg);
    }
}

// StatusOr 在成功路径上应保存并暴露值类型 T。
TEST(StatusOrTest, HoldsSuccessValue) {
    StatusOr<int> so(123);
    ASSERT_TRUE(so.ok());
    EXPECT_EQ(so.value(), 123);
    EXPECT_EQ(*so, 123);
}

// StatusOr 在失败路径上应保存 Status，且 ok() 为 false。
TEST(StatusOrTest, HoldsErrorStatus) {
    Status err = Status::Error(StatusCode::kNotFound, "未找到资源");
    StatusOr<int> so(err);

    EXPECT_FALSE(so.ok());
    EXPECT_EQ(so.status().code(), StatusCode::kNotFound);
    EXPECT_EQ(so.status().message(), "未找到资源");
}

// 验证 StatusOr 的移动构造：资源可转移到新对象且目标状态正确。
TEST(StatusOrTest, MoveConstructionTransfersValue) {
    StatusOr<std::string> so1(std::string("move-me"));
    StatusOr<std::string> so2(std::move(so1));

    ASSERT_TRUE(so2.ok());
    EXPECT_EQ(*so2, "move-me");
}

// StatusOr 与 shared_ptr：成功时表示可共享资源的生命周期管理语义正常。
TEST(StatusOrTest, WorksWithSharedPtr) {
    auto ptr = std::make_shared<int>(42);
    StatusOr<std::shared_ptr<int>> so(std::move(ptr));

    ASSERT_TRUE(so.ok());
    ASSERT_NE((*so).get(), nullptr);
    EXPECT_EQ(**so, 42);

    StatusOr<std::shared_ptr<int>> err(
        Status::Error(StatusCode::kUnavailable, "服务不可用"));
    EXPECT_FALSE(err.ok());
}

// Status::ToString 对成功与失败状态的格式化应符合实现约定。
TEST(StatusTest, ToStringFormatting) {
    EXPECT_EQ(Status().ToString(), "OK");
    EXPECT_EQ(Status::Ok().ToString(), "OK");

    Status s = Status::Error(StatusCode::kInternal, "内部错误");
    const std::string out = s.ToString();
    const std::string expected =
        "Error(" + std::to_string(static_cast<int>(StatusCode::kInternal)) + "): 内部错误";
    EXPECT_EQ(out, expected);
}

}  // namespace
}  // namespace illuminator
