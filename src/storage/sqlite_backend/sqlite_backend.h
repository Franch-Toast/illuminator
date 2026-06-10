// ============================================================================
// Illuminator SQLite 存储后端 — 嵌入式持久化实现
// ============================================================================
//
// 使用 sqlite3 C 库实现 StorageBackend 接口，提供完整的本地持久化能力。
//
// 数据库设计（3 张表 + 3 个索引）：
// ==================================
// records 表 — 时间序列指标记录
//   | id | pipeline | timestamp_ns | labels_json | fields_json |
//
// stack_samples 表 — 堆栈采样记录
//   | id | pipeline | timestamp_ns | pid | tid | comm | stack_json | count |
//
// profiles 表 — 原始 profile 数据
//   | id | pipeline | profile_type | start_ns | end_ns | sample_count | data (BLOB) |
//
// 优化策略：
// ==========
// - WAL 模式（Write-Ahead Log）：提高并发读写性能，允许多读者 + 一写者
// - NORMAL synchronous：平衡安全性和写入速度
// - 10MB cache_size：缓存热数据减少磁盘 I/O
// - 复合索引 (pipeline, timestamp_ns)：加速按管道和时间范围的查询
// - 批量事务（BEGIN/COMMIT）：减少 fsync 次数
// ============================================================================

#pragma once

#include <sqlite3.h>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/common/logging.h"
#include "storage/storage_backend.h"

namespace illuminator {

class SqliteBackend : public StorageBackend {
public:
    ~SqliteBackend() override { Close(); }

    const char* Name() const override { return "sqlite"; }

    // ---- 初始化 ----
    // 创建数据目录，打开/创建数据库文件，配置性能参数，建表
    Status Init(const std::string& data_dir,
                const ConfigValue& config) override {
        std::filesystem::create_directories(data_dir);  // 确保目录存在
        std::string db_path = data_dir + "/illuminator.db";

        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            return Status::Error(StatusCode::kInternal,
                std::string("SQLite open failed: ") + sqlite3_errmsg(db_));
        }

        // 性能优化 PRAGMA
        Execute("PRAGMA journal_mode=WAL");        // WAL 模式提升并发
        Execute("PRAGMA synchronous=NORMAL");      // 降低 fsync 频率
        Execute("PRAGMA cache_size=10000");        // 10MB 缓存
        sqlite3_busy_timeout(db_, 5000);           // 并发写入时自动等待重试

        auto status = CreateTables();
        if (!status.ok()) return status;

        IL_INFO("SQLite backend initialized: {}", db_path);
        db_path_ = db_path;
        return Status::Ok();
    }

    // ---- 写入指标记录 ----
    // 使用预编译语句（Prepared Statement）批量 INSERT 提高性能
    Status WriteRecords(const std::string& pipeline_name,
                        const std::vector<Record>& records) override {
        if (!db_ || records.empty()) return Status::Ok();
        std::lock_guard<std::mutex> lock(write_mutex_);

        Execute("BEGIN TRANSACTION");  // 批量事务：减少 fsync 开销

        const char* sql =
            "INSERT INTO records (pipeline, timestamp_ns, labels_json, fields_json) "
            "VALUES (?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, sql, "WriteRecords");
        if (prc != SQLITE_OK || !stmt) {
            Execute("ROLLBACK");
            return Status::Error(StatusCode::kInternal,
                                 "SQLite prepare failed: WriteRecords");
        }

        for (auto& rec : records) {
            std::string labels = SerializeLabels(rec.labels);
            std::string fields = SerializeFields(rec.fields);
            uint64_t ts = TimestampToNanos(rec.timestamp);

            // 绑定参数（SQLITE_TRANSIENT 表示 SQLite 会自行拷贝数据）
            sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ts));
            sqlite3_bind_text(stmt, 3, labels.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, fields.c_str(), -1, SQLITE_TRANSIENT);

            if (!StepExpectDone(stmt, "WriteRecords insert")) {
                sqlite3_finalize(stmt);
                Execute("ROLLBACK");
                return Status::Error(StatusCode::kInternal,
                                     "SQLite step failed: WriteRecords");
            }
            sqlite3_reset(stmt);  // 重置预编译语句状态
        }

        sqlite3_finalize(stmt);
        if (Execute("COMMIT") != SQLITE_OK) {
            return Status::Error(StatusCode::kInternal, "SQLite COMMIT failed: WriteRecords");
        }
        return Status::Ok();
    }

    // ---- 写入堆栈采样 ----
    Status WriteStackSamples(const std::string& pipeline_name,
                             const std::vector<StackSample>& samples) override {
        if (!db_ || samples.empty()) return Status::Ok();
        std::lock_guard<std::mutex> lock(write_mutex_);

        Execute("BEGIN TRANSACTION");

        const char* sql =
            "INSERT INTO stack_samples (pipeline, timestamp_ns, pid, tid, "
            "comm, stack_json, count) VALUES (?, ?, ?, ?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, sql, "WriteStackSamples");
        if (prc != SQLITE_OK || !stmt) {
            Execute("ROLLBACK");
            return Status::Error(StatusCode::kInternal,
                                 "SQLite prepare failed: WriteStackSamples");
        }

        for (auto& s : samples) {
            uint64_t ts = TimestampToNanos(s.timestamp);
            std::string stack = SerializeStack(s);

            sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ts));
            sqlite3_bind_int(stmt, 3, s.pid);
            sqlite3_bind_int(stmt, 4, s.tid);
            std::string comm(s.comm);
            sqlite3_bind_text(stmt, 5, comm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, stack.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(s.count));

            if (!StepExpectDone(stmt, "WriteStackSamples insert")) {
                sqlite3_finalize(stmt);
                Execute("ROLLBACK");
                return Status::Error(StatusCode::kInternal,
                                     "SQLite step failed: WriteStackSamples");
            }
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        if (Execute("COMMIT") != SQLITE_OK) {
            return Status::Error(StatusCode::kInternal,
                                 "SQLite COMMIT failed: WriteStackSamples");
        }
        return Status::Ok();
    }

    // ---- 写入原始 profile ----
    Status WriteProfile(const ProfileMeta& meta,
                        const std::vector<uint8_t>& data) override {
        if (!db_) return Status::Error(StatusCode::kInternal, "DB not open");
        std::lock_guard<std::mutex> lock(write_mutex_);

        const char* sql =
            "INSERT INTO profiles (pipeline, profile_type, start_ns, end_ns, "
            "sample_count, data) VALUES (?, ?, ?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, sql, "WriteProfile");
        if (prc != SQLITE_OK || !stmt) {
            return Status::Error(StatusCode::kInternal,
                                 "SQLite prepare failed: WriteProfile");
        }

        sqlite3_bind_text(stmt, 1, meta.pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, meta.profile_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(meta.start_time_ns));
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(meta.end_time_ns));
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(meta.sample_count));
        sqlite3_bind_blob(stmt, 6, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);

        if (!StepExpectDone(stmt, "WriteProfile insert")) {
            sqlite3_finalize(stmt);
            return Status::Error(StatusCode::kInternal,
                                 "SQLite step failed: WriteProfile");
        }
        sqlite3_finalize(stmt);
        return Status::Ok();
    }

    // ---- 查询历史数据 ----
    StatusOr<QueryResult> Query(const QueryRequest& req) override {
        QueryResult result;
        if (!db_) {
            return Status::Error(StatusCode::kInternal, "DB not open");
        }

        std::ostringstream sql;
        sql << "SELECT timestamp_ns, labels_json, fields_json FROM records "
            << "WHERE pipeline = ?";

        if (req.time_range.start_ns > 0)
            sql << " AND timestamp_ns >= " << req.time_range.start_ns;
        if (req.time_range.end_ns > 0)
            sql << " AND timestamp_ns <= " << req.time_range.end_ns;

        sql << " ORDER BY timestamp_ns " << (req.descending ? "DESC" : "ASC");
        sql << " LIMIT " << req.limit;

        std::string query = sql.str();
        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, query.c_str(), "Query");
        if (prc != SQLITE_OK || !stmt) {
            return Status::Error(StatusCode::kInternal,
                std::string("Query prepare failed: ") + sqlite3_errmsg(db_));
        }

        sqlite3_bind_text(stmt, 1, req.pipeline_name.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        while (rc == SQLITE_ROW) {
            Record rec;
            rec.timestamp = std::chrono::system_clock::time_point(
                std::chrono::nanoseconds(sqlite3_column_int64(stmt, 0)));

            const char* labels_str = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 1));
            const char* fields_str = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 2));

            if (labels_str) {
                try {
                    auto lj = nlohmann::json::parse(labels_str);
                    for (auto& [k, v] : lj.items()) {
                        auto key_sv = result.Intern(k);
                        auto val_sv = result.Intern(v.get<std::string>());
                        rec.labels.push_back({key_sv, val_sv});
                    }
                } catch (...) {}
            }
            if (fields_str) {
                try {
                    auto fj = nlohmann::json::parse(fields_str);
                    for (auto& [k, v] : fj.items()) {
                        auto key_sv = result.Intern(k);
                        if (v.is_number_float())
                            rec.SetField(key_sv, v.get<double>());
                        else if (v.is_number_integer())
                            rec.SetField(key_sv, v.get<int64_t>());
                        else if (v.is_number_unsigned())
                            rec.SetField(key_sv, v.get<uint64_t>());
                        else if (v.is_boolean())
                            rec.SetField(key_sv, v.get<bool>());
                        else if (v.is_string())
                            rec.SetField(key_sv, result.Intern(v.get<std::string>()));
                    }
                } catch (...) {}
            }

            result.records.push_back(std::move(rec));
            result.total_count++;
            rc = sqlite3_step(stmt);
        }
        if (rc != SQLITE_DONE) {
            StepLogError(rc, "Query fetch");
        }

        sqlite3_finalize(stmt);
        return result;
    }

    // ---- 列出 Profile 列表 ----
    StatusOr<std::vector<ProfileMeta>> ListProfiles(
        const std::string& pipeline_name,
        const TimeRange& range) override {
        std::vector<ProfileMeta> profiles;
        if (!db_) {
            return Status::Error(StatusCode::kInternal, "DB not open");
        }

        std::ostringstream oss;
        oss << "SELECT pipeline, profile_type, start_ns, end_ns, sample_count "
               "FROM profiles WHERE pipeline = ?";
        if (range.start_ns > 0)
            oss << " AND end_ns >= " << range.start_ns;
        if (range.end_ns > 0)
            oss << " AND start_ns <= " << range.end_ns;
        oss << " ORDER BY start_ns DESC";

        std::string sql = oss.str();
        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, sql.c_str(), "ListProfiles");
        if (prc != SQLITE_OK || !stmt) {
            return Status::Error(StatusCode::kInternal,
                std::string("ListProfiles prepare failed: ") + sqlite3_errmsg(db_));
        }

        sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        while (rc == SQLITE_ROW) {
            ProfileMeta meta;
            meta.pipeline_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            meta.profile_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            meta.start_time_ns = sqlite3_column_int64(stmt, 2);
            meta.end_time_ns = sqlite3_column_int64(stmt, 3);
            meta.sample_count = sqlite3_column_int64(stmt, 4);
            profiles.push_back(std::move(meta));
            rc = sqlite3_step(stmt);
        }
        if (rc != SQLITE_DONE) {
            StepLogError(rc, "ListProfiles fetch");
        }

        sqlite3_finalize(stmt);
        return profiles;
    }

    // ---- 生命周期 ----
    Status Flush() override {
        if (db_) Execute("PRAGMA wal_checkpoint(PASSIVE)");  // WAL 日志写入主文件
        return Status::Ok();
    }

    Status Compact() override {
        if (db_) Execute("VACUUM");
        return Status::Ok();
    }

    // 按时间窗口清理过期数据，释放内存和磁盘空间。
    // max_age_ns: 保留最近多少纳秒的数据（0 表示不清理）
    Status Prune(uint64_t max_age_ns) {
        if (!db_ || max_age_ns == 0) return Status::Ok();
        std::lock_guard<std::mutex> lock(write_mutex_);

        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        uint64_t cutoff_ns = now_ns - max_age_ns;

        char sql_buf[256];
        std::snprintf(sql_buf, sizeof(sql_buf),
            "DELETE FROM records WHERE timestamp_ns < %" PRIu64, cutoff_ns);
        Execute(sql_buf);

        std::snprintf(sql_buf, sizeof(sql_buf),
            "DELETE FROM stack_samples WHERE timestamp_ns < %" PRIu64, cutoff_ns);
        Execute(sql_buf);

        std::snprintf(sql_buf, sizeof(sql_buf),
            "DELETE FROM profiles WHERE start_ns < %" PRIu64, cutoff_ns);
        Execute(sql_buf);

        pruned_count_++;
        if (pruned_count_ % 12 == 0) {
            Execute("PRAGMA wal_checkpoint(TRUNCATE)");
        }
        return Status::Ok();
    }

    Status Close() override {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return Status::Ok();
    }

    uint64_t DiskUsageBytes() const override {
        if (db_path_.empty()) return 0;
        try {
            return std::filesystem::file_size(db_path_);
        } catch (...) {
            return 0;
        }
    }

    StatusOr<std::string> ExecuteRawQuery(const std::string& sql) override {
        if (!db_) return Status::Error(StatusCode::kInternal, "DB not open");

        if (!IsReadOnlyStatement(sql)) {
            return Status::Error(StatusCode::kPermissionDenied,
                "Only SELECT, EXPLAIN and PRAGMA statements are allowed");
        }

        sqlite3_stmt* stmt = nullptr;
        int prc = PrepareOrLog(&stmt, sql.c_str(), "ExecuteRawQuery");
        if (prc != SQLITE_OK || !stmt) {
            return Status::Error(StatusCode::kInternal,
                std::string("SQL prepare failed: ") + sqlite3_errmsg(db_));
        }

        if (!sqlite3_stmt_readonly(stmt)) {
            sqlite3_finalize(stmt);
            return Status::Error(StatusCode::kPermissionDenied,
                "Statement classified as non-readonly by SQLite");
        }

        nlohmann::json rows = nlohmann::json::array();
        int col_count = sqlite3_column_count(stmt);

        std::vector<std::string> col_names;
        for (int i = 0; i < col_count; ++i) {
            col_names.push_back(sqlite3_column_name(stmt, i));
        }

        int row_limit = 1000;
        int rc = sqlite3_step(stmt);
        while (rc == SQLITE_ROW && row_limit-- > 0) {
            nlohmann::json row = nlohmann::json::object();
            for (int i = 0; i < col_count; ++i) {
                int type = sqlite3_column_type(stmt, i);
                switch (type) {
                    case SQLITE_INTEGER:
                        row[col_names[i]] = sqlite3_column_int64(stmt, i);
                        break;
                    case SQLITE_FLOAT:
                        row[col_names[i]] = sqlite3_column_double(stmt, i);
                        break;
                    case SQLITE_TEXT:
                        row[col_names[i]] = reinterpret_cast<const char*>(
                            sqlite3_column_text(stmt, i));
                        break;
                    case SQLITE_NULL:
                        row[col_names[i]] = nullptr;
                        break;
                    default:
                        row[col_names[i]] = "[blob]";
                        break;
                }
            }
            rows.push_back(std::move(row));
            rc = sqlite3_step(stmt);
        }

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return Status::Error(StatusCode::kInternal,
                std::string("SQL execution failed: ") + sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return nlohmann::json{{"rows", std::move(rows)}}.dump();
    }

private:
    static bool IsReadOnlyStatement(const std::string& sql) {
        auto trimmed = sql;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return false;
        std::string prefix = trimmed.substr(start, 7);
        for (auto& c : prefix) c = static_cast<char>(::toupper(c));
        return prefix.compare(0, 6, "SELECT") == 0 ||
               prefix.compare(0, 7, "EXPLAIN") == 0 ||
               prefix.compare(0, 6, "PRAGMA") == 0;
    }

    // ---- 建表 + 索引 ----
    Status CreateTables() {
        int rc;
        rc = Execute(
            "CREATE TABLE IF NOT EXISTS records ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  pipeline TEXT NOT NULL,"
            "  timestamp_ns INTEGER NOT NULL,"
            "  labels_json TEXT,"     // JSON 存储的标签
            "  fields_json TEXT"      // JSON 存储的字段值
            ")");
        if (rc != SQLITE_OK) return Status::Error(StatusCode::kInternal, "Create records table failed");

        rc = Execute(
            "CREATE TABLE IF NOT EXISTS stack_samples ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  pipeline TEXT NOT NULL,"
            "  timestamp_ns INTEGER NOT NULL,"
            "  pid INTEGER,"
            "  tid INTEGER,"
            "  comm TEXT,"
            "  stack_json TEXT,"       // JSON 存储的堆栈帧
            "  count INTEGER DEFAULT 1"
            ")");
        if (rc != SQLITE_OK) return Status::Error(StatusCode::kInternal, "Create stack_samples table failed");

        rc = Execute(
            "CREATE TABLE IF NOT EXISTS profiles ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  pipeline TEXT NOT NULL,"
            "  profile_type TEXT NOT NULL,"
            "  start_ns INTEGER,"
            "  end_ns INTEGER,"
            "  sample_count INTEGER,"
            "  data BLOB"              // 原始二进制数据
            ")");
        if (rc != SQLITE_OK) return Status::Error(StatusCode::kInternal, "Create profiles table failed");

        // 索引加速按管道+时间范围查询
        Execute("CREATE INDEX IF NOT EXISTS idx_records_ts ON records(pipeline, timestamp_ns)");
        Execute("CREATE INDEX IF NOT EXISTS idx_samples_ts ON stack_samples(pipeline, timestamp_ns)");
        Execute("CREATE INDEX IF NOT EXISTS idx_profiles_ts ON profiles(pipeline, start_ns)");

        return Status::Ok();
    }

    // ---- 执行 SQL 语句（无返回值） ----
    int Execute(const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK && err) {
            IL_ERROR("SQLite error: {} (SQL: {})", err, sql);
            sqlite3_free(err);
        }
        return rc;
    }

    // sqlite3_prepare_v2 封装：非 SQLITE_OK 时写日志
    int PrepareOrLog(sqlite3_stmt** stmt, const char* sql, const char* context) {
        *stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr);
        if (rc != SQLITE_OK) {
            IL_ERROR("SQLite prepare failed [{}]: {} (SQL: {})", context,
                     sqlite3_errmsg(db_), sql);
            *stmt = nullptr;
        }
        return rc;
    }

    // 写路径：单行执行后应为 SQLITE_DONE
    bool StepExpectDone(sqlite3_stmt* stmt, const char* context) {
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            StepLogError(rc, context);
            return false;
        }
        return true;
    }

    void StepLogError(int rc, const char* context) {
        IL_ERROR("SQLite step failed [{}]: code={} {}", context, rc,
                 sqlite3_errmsg(db_));
    }

    static std::string SerializeLabels(const std::vector<Label>& labels) {
        nlohmann::json j = nlohmann::json::object();
        for (auto& l : labels)
            j[std::string(l.key)] = std::string(l.value);
        return j.dump();
    }

    static std::string SerializeFields(
        const std::unordered_map<std::string_view, FieldValue>& fields) {
        nlohmann::json j = nlohmann::json::object();
        for (auto& [k, v] : fields) {
            std::string key(k);
            struct Vis {
                nlohmann::json& j;
                const std::string& key;
                void operator()(std::monostate) const { j[key] = nullptr; }
                void operator()(bool b) const { j[key] = b; }
                void operator()(int64_t i) const { j[key] = i; }
                void operator()(uint64_t u) const { j[key] = u; }
                void operator()(double d) const { j[key] = d; }
                void operator()(std::string_view sv) const { j[key] = std::string(sv); }
            };
            std::visit(Vis{j, key}, v);
        }
        return j.dump();
    }

    static std::string SerializeStack(const StackSample& s) {
        nlohmann::json arr = nlohmann::json::array();
        for (auto& f : s.user_stack) {
            arr.push_back({
                {"addr", f.address},
                {"fn", std::string(f.function_name)},
                {"file", std::string(f.file_name)},
                {"line", f.line_number},
            });
        }
        return arr.dump();
    }

    sqlite3* db_ = nullptr;        // SQLite 数据库连接句柄
    std::string db_path_;          // 数据库文件路径
    std::mutex write_mutex_;       // 序列化所有写入操作，防止并发锁冲突
    uint32_t pruned_count_ = 0;    // Prune 调用计数（用于定期 WAL checkpoint）
};

// 静态初始化器：自动注册 SQLite 后端到 StorageFactory
static bool _reg_sqlite = [] {
    StorageFactory::Instance().Register("sqlite", [] {
        return std::make_unique<SqliteBackend>();
    });
    return true;
}();

}  // namespace illuminator
