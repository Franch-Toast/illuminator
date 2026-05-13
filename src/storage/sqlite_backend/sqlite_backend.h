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
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

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

        auto status = CreateTables();
        if (!status.ok()) return status;

        IL_INFO("SQLite backend initialized: %s", db_path.c_str());
        db_path_ = db_path;
        return Status::Ok();
    }

    // ---- 写入指标记录 ----
    // 使用预编译语句（Prepared Statement）批量 INSERT 提高性能
    Status WriteRecords(const std::string& pipeline_name,
                        const std::vector<Record>& records) override {
        if (!db_ || records.empty()) return Status::Ok();

        Execute("BEGIN TRANSACTION");  // 批量事务：减少 fsync 开销

        const char* sql =
            "INSERT INTO records (pipeline, timestamp_ns, labels_json, fields_json) "
            "VALUES (?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

        for (auto& rec : records) {
            std::string labels = SerializeLabels(rec.labels);
            std::string fields = SerializeFields(rec.fields);
            uint64_t ts = TimestampToNanos(rec.timestamp);

            // 绑定参数（SQLITE_TRANSIENT 表示 SQLite 会自行拷贝数据）
            sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ts));
            sqlite3_bind_text(stmt, 3, labels.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, fields.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);  // 重置预编译语句状态
        }

        sqlite3_finalize(stmt);
        Execute("COMMIT");
        return Status::Ok();
    }

    // ---- 写入堆栈采样 ----
    Status WriteStackSamples(const std::string& pipeline_name,
                             const std::vector<StackSample>& samples) override {
        if (!db_ || samples.empty()) return Status::Ok();

        Execute("BEGIN TRANSACTION");

        const char* sql =
            "INSERT INTO stack_samples (pipeline, timestamp_ns, pid, tid, "
            "comm, stack_json, count) VALUES (?, ?, ?, ?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

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

            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        Execute("COMMIT");
        return Status::Ok();
    }

    // ---- 写入原始 profile ----
    Status WriteProfile(const ProfileMeta& meta,
                        const std::vector<uint8_t>& data) override {
        if (!db_) return Status::Error(StatusCode::kInternal, "DB not open");

        const char* sql =
            "INSERT INTO profiles (pipeline, profile_type, start_ns, end_ns, "
            "sample_count, data) VALUES (?, ?, ?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

        sqlite3_bind_text(stmt, 1, meta.pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, meta.profile_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(meta.start_time_ns));
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(meta.end_time_ns));
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(meta.sample_count));
        sqlite3_bind_blob(stmt, 6, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return Status::Ok();
    }

    // ---- 查询历史数据 ----
    StatusOr<QueryResult> Query(const QueryRequest& req) override {
        QueryResult result;
        if (!db_) return result;

        // 构建动态 SQL
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
        sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, req.pipeline_name.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Record rec;
            // 基础反序列化（当前为简化实现）
            result.records.push_back(std::move(rec));
            result.total_count++;
        }

        sqlite3_finalize(stmt);
        return result;
    }

    // ---- 列出 Profile 列表 ----
    StatusOr<std::vector<ProfileMeta>> ListProfiles(
        const std::string& pipeline_name,
        const TimeRange& range) override {
        std::vector<ProfileMeta> profiles;
        if (!db_) return profiles;

        std::string sql =
            "SELECT pipeline, profile_type, start_ns, end_ns, sample_count "
            "FROM profiles WHERE pipeline = ? ORDER BY start_ns DESC";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ProfileMeta meta;
            meta.pipeline_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            meta.profile_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            meta.start_time_ns = sqlite3_column_int64(stmt, 2);
            meta.end_time_ns = sqlite3_column_int64(stmt, 3);
            meta.sample_count = sqlite3_column_int64(stmt, 4);
            profiles.push_back(std::move(meta));
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
        if (db_) Execute("VACUUM");  // 压缩数据库，回收碎片空间
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

private:
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
            IL_ERROR("SQLite error: %s (SQL: %s)", err, sql);
            sqlite3_free(err);
        }
        return rc;
    }

    // ---- 序列化标签为 JSON ----
    static std::string SerializeLabels(const std::vector<Label>& labels) {
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        for (auto& l : labels) {
            if (!first) ss << ",";
            ss << "\"" << l.key << "\":\"" << l.value << "\"";
            first = false;
        }
        ss << "}";
        return ss.str();
    }

    // ---- 序列化字段值为 JSON ----
    static std::string SerializeFields(
        const std::unordered_map<std::string_view, FieldValue>& fields) {
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        for (auto& [k, v] : fields) {
            if (!first) ss << ",";
            ss << "\"" << k << "\":";
            // std::visit 模式匹配处理所有 FieldValue 变体
            struct Vis {
                std::ostringstream& s;
                void operator()(std::monostate) const { s << "null"; }
                void operator()(bool b) const { s << (b ? "true" : "false"); }
                void operator()(int64_t i) const { s << i; }
                void operator()(uint64_t u) const { s << u; }
                void operator()(double d) const { s << d; }
                void operator()(std::string_view sv) const { s << "\"" << sv << "\""; }
            };
            std::visit(Vis{ss}, v);
            first = false;
        }
        ss << "}";
        return ss.str();
    }

    // ---- 序列化堆栈为 JSON ----
    static std::string SerializeStack(const StackSample& s) {
        std::ostringstream ss;
        ss << "[";
        bool first = true;
        for (auto& f : s.user_stack) {
            if (!first) ss << ",";
            ss << "{\"addr\":" << f.address
               << ",\"fn\":\"" << f.function_name << "\""
               << ",\"file\":\"" << f.file_name << "\""
               << ",\"line\":" << f.line_number << "}";
            first = false;
        }
        ss << "]";
        return ss.str();
    }

    sqlite3* db_ = nullptr;        // SQLite 数据库连接句柄
    std::string db_path_;          // 数据库文件路径
};

// 静态初始化器：自动注册 SQLite 后端到 StorageFactory
static bool _reg_sqlite = [] {
    StorageFactory::Instance().Register("sqlite", [] {
        return std::make_unique<SqliteBackend>();
    });
    return true;
}();

}  // namespace illuminator
