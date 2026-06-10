// sqlite_backend_test.cc — SqliteBackend 单元测试
// 覆盖：并发写入安全、Query 反序列化、ListProfiles 时间范围过滤、SQL 安全校验

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "storage/sqlite_backend/sqlite_backend.h"

namespace illuminator {
namespace {

class SqliteBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        backend_ = std::make_unique<SqliteBackend>();
        ConfigValue cfg;
        ASSERT_TRUE(backend_->Init("/tmp/illuminator_test_sqlite", cfg).ok());
    }

    void TearDown() override {
        backend_->Close();
        std::filesystem::remove_all("/tmp/illuminator_test_sqlite");
    }

    std::unique_ptr<SqliteBackend> backend_;
};

// P0 #2 验证：并发写入不产生 "database is locked" 错误
TEST_F(SqliteBackendTest, ConcurrentWritesDoNotFail) {
    constexpr int kThreads = 4;
    constexpr int kRecordsPerThread = 50;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kRecordsPerThread; ++i) {
                Record rec;
                rec.timestamp = std::chrono::system_clock::now();
                rec.labels.push_back({"source", "test"});
                rec.SetField("thread_id", static_cast<int64_t>(t));
                rec.SetField("seq", static_cast<int64_t>(i));

                auto st = backend_->WriteRecords("test_pipe", {rec});
                if (!st.ok()) errors.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0) << "Concurrent writes should not produce errors";

    QueryRequest req;
    req.pipeline_name = "test_pipe";
    req.limit = 10000;
    auto result = backend_->Query(req);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_count,
              static_cast<uint64_t>(kThreads * kRecordsPerThread));
}

// P0 #3 验证：Query() 正确反序列化 labels 和 fields
TEST_F(SqliteBackendTest, QueryDeserializesLabelsAndFields) {
    Record rec;
    rec.timestamp = std::chrono::system_clock::now();
    rec.labels.push_back({"source", "cpu_utilization"});
    rec.labels.push_back({"cpu", "cpu0"});
    rec.SetField("user_pct", 42.5);
    rec.SetField("idle_pct", 57.5);

    ASSERT_TRUE(backend_->WriteRecords("my_pipe", {rec}).ok());

    QueryRequest req;
    req.pipeline_name = "my_pipe";
    req.limit = 10;
    auto result = backend_->Query(req);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().records.size(), 1u);

    auto& retrieved = result.value().records[0];
    EXPECT_EQ(retrieved.labels.size(), 2u);
    bool found_source = false, found_cpu = false;
    for (auto& l : retrieved.labels) {
        if (std::string(l.key) == "source" && std::string(l.value) == "cpu_utilization")
            found_source = true;
        if (std::string(l.key) == "cpu" && std::string(l.value) == "cpu0")
            found_cpu = true;
    }
    EXPECT_TRUE(found_source);
    EXPECT_TRUE(found_cpu);

    auto it = retrieved.fields.find("user_pct");
    ASSERT_NE(it, retrieved.fields.end());
    auto* dval = std::get_if<double>(&it->second);
    ASSERT_NE(dval, nullptr);
    EXPECT_NEAR(*dval, 42.5, 0.001);
}

// P0 #3 验证：Query 时间范围过滤
TEST_F(SqliteBackendTest, QueryRespectsTimeRange) {
    auto base = std::chrono::system_clock::now();
    for (int i = 0; i < 5; ++i) {
        Record rec;
        rec.timestamp = base + std::chrono::seconds(i * 10);
        rec.labels.push_back({"seq", std::to_string(i)});
        ASSERT_TRUE(backend_->WriteRecords("time_pipe", {rec}).ok());
    }

    auto ns = [](std::chrono::system_clock::time_point tp) -> uint64_t {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                tp.time_since_epoch()).count());
    };

    QueryRequest req;
    req.pipeline_name = "time_pipe";
    req.time_range.start_ns = ns(base + std::chrono::seconds(10));
    req.time_range.end_ns = ns(base + std::chrono::seconds(30));
    req.limit = 100;
    req.descending = false;

    auto result = backend_->Query(req);
    ASSERT_TRUE(result.ok());
    EXPECT_GE(result.value().total_count, 2u);
    EXPECT_LE(result.value().total_count, 3u);
}

// P0 #3 验证：ListProfiles 应用时间范围
TEST_F(SqliteBackendTest, ListProfilesRespectsTimeRange) {
    for (int i = 0; i < 4; ++i) {
        ProfileMeta meta;
        meta.pipeline_name = "prof_pipe";
        meta.profile_type = "cpu";
        meta.start_time_ns = static_cast<uint64_t>(i * 1000);
        meta.end_time_ns = static_cast<uint64_t>(i * 1000 + 500);
        meta.sample_count = 100;
        std::vector<uint8_t> data(10, 0x42);
        ASSERT_TRUE(backend_->WriteProfile(meta, data).ok());
    }

    TimeRange range;
    range.start_ns = 1000;
    range.end_ns = 2500;
    auto result = backend_->ListProfiles("prof_pipe", range);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().size(), 2u);

    TimeRange all{};
    auto all_result = backend_->ListProfiles("prof_pipe", all);
    ASSERT_TRUE(all_result.ok());
    EXPECT_EQ(all_result.value().size(), 4u);
}

// P0 #7 验证：ExecuteRawQuery 拒绝 DML/DDL 语句
TEST_F(SqliteBackendTest, ExecuteRawQueryRejectsNonSelectStatements) {
    auto r1 = backend_->ExecuteRawQuery("DROP TABLE records");
    EXPECT_FALSE(r1.ok());

    auto r2 = backend_->ExecuteRawQuery("DELETE FROM records");
    EXPECT_FALSE(r2.ok());

    auto r3 = backend_->ExecuteRawQuery("INSERT INTO records VALUES(1,'a',1,'{}','{}')");
    EXPECT_FALSE(r3.ok());

    auto r4 = backend_->ExecuteRawQuery("UPDATE records SET pipeline='x'");
    EXPECT_FALSE(r4.ok());
}

// P0 #7 验证：ExecuteRawQuery 允许 SELECT/EXPLAIN/PRAGMA
TEST_F(SqliteBackendTest, ExecuteRawQueryAllowsReadOnlyStatements) {
    auto r1 = backend_->ExecuteRawQuery("SELECT count(*) FROM records");
    EXPECT_TRUE(r1.ok());

    auto r2 = backend_->ExecuteRawQuery("EXPLAIN QUERY PLAN SELECT * FROM records LIMIT 1");
    EXPECT_TRUE(r2.ok());

    auto r3 = backend_->ExecuteRawQuery("PRAGMA table_info(records)");
    EXPECT_TRUE(r3.ok());
}

// P0 #3 验证：Query 对空数据库返回状态正确（非默认空结果）
TEST_F(SqliteBackendTest, QueryOnEmptyDbReturnsValidEmptyResult) {
    QueryRequest req;
    req.pipeline_name = "nonexistent";
    req.limit = 10;
    auto result = backend_->Query(req);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().total_count, 0u);
    EXPECT_TRUE(result.value().records.empty());
}

}  // namespace
}  // namespace illuminator
