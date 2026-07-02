#include "gtest/gtest.h"
#include "httplib.h"
#include "core/common/version_generated.h"
#include "server/api_routes.h"

#include <thread>
#include <chrono>

namespace illuminator {
namespace {

class ServerApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        srv_ = std::make_unique<httplib::Server>();
        RegisterApiRoutes(*srv_);
        server_thread_ = std::thread([this]() {
            srv_->listen("127.0.0.1", 19527);
        });
        while (!srv_->is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void TearDown() override {
        srv_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    std::unique_ptr<httplib::Server> srv_;
    std::thread server_thread_;
};

TEST_F(ServerApiTest, HealthzReturnsOkAndVersion) {
    httplib::Client cli("127.0.0.1", 19527);
    auto res = cli.Get("/healthz");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_TRUE(body.contains("version"));
    EXPECT_TRUE(body.contains("commit"));
    EXPECT_EQ(body["version"], kBuildVersion);
}

TEST_F(ServerApiTest, PipelinesEndpointRespondsWithValidJson) {
    httplib::Client cli("127.0.0.1", 19527);
    auto res = cli.Get("/api/v1/pipelines");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);
    EXPECT_NO_THROW(nlohmann::json::parse(res->body));
}

class ServerAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        srv_ = std::make_unique<httplib::Server>();
        SetupAuthMiddleware(*srv_, "test-secret-token");
        RegisterApiRoutes(*srv_);
        server_thread_ = std::thread([this]() {
            srv_->listen("127.0.0.1", 19528);
        });
        while (!srv_->is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void TearDown() override {
        srv_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    std::unique_ptr<httplib::Server> srv_;
    std::thread server_thread_;
};

TEST_F(ServerAuthTest, HealthzBypassesAuth) {
    httplib::Client cli("127.0.0.1", 19528);
    auto res = cli.Get("/healthz");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);
}

TEST_F(ServerAuthTest, ApiWithoutTokenReturns401) {
    httplib::Client cli("127.0.0.1", 19528);
    auto res = cli.Get("/api/v1/pipelines");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 401);
}

TEST_F(ServerAuthTest, ApiWithWrongTokenReturns403) {
    httplib::Client cli("127.0.0.1", 19528);
    httplib::Headers headers = {{"Authorization", "Bearer wrong-token"}};
    auto res = cli.Get("/api/v1/pipelines", headers);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 403);
}

TEST_F(ServerAuthTest, ApiWithCorrectTokenReturns200) {
    httplib::Client cli("127.0.0.1", 19528);
    httplib::Headers headers = {{"Authorization", "Bearer test-secret-token"}};
    auto res = cli.Get("/api/v1/pipelines", headers);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 200);
}

}  // namespace
}  // namespace illuminator
