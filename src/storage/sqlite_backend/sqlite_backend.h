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

    Status Init(const std::string& data_dir,
                const ConfigValue& config) override {
        std::filesystem::create_directories(data_dir);
        std::string db_path = data_dir + "/illuminator.db";

        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            return Status::Error(StatusCode::kInternal,
                std::string("SQLite open failed: ") + sqlite3_errmsg(db_));
        }

        // WAL mode for better concurrent read/write performance
        Execute("PRAGMA journal_mode=WAL");
        Execute("PRAGMA synchronous=NORMAL");
        Execute("PRAGMA cache_size=10000");

        auto status = CreateTables();
        if (!status.ok()) return status;

        IL_INFO("SQLite backend initialized: %s", db_path.c_str());
        db_path_ = db_path;
        return Status::Ok();
    }

    Status WriteRecords(const std::string& pipeline_name,
                        const std::vector<Record>& records) override {
        if (!db_ || records.empty()) return Status::Ok();

        Execute("BEGIN TRANSACTION");

        const char* sql =
            "INSERT INTO records (pipeline, timestamp_ns, labels_json, fields_json) "
            "VALUES (?, ?, ?, ?)";

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

        for (auto& rec : records) {
            std::string labels = SerializeLabels(rec.labels);
            std::string fields = SerializeFields(rec.fields);
            uint64_t ts = TimestampToNanos(rec.timestamp);

            sqlite3_bind_text(stmt, 1, pipeline_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ts));
            sqlite3_bind_text(stmt, 3, labels.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, fields.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        Execute("COMMIT");
        return Status::Ok();
    }

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

    StatusOr<QueryResult> Query(const QueryRequest& req) override {
        QueryResult result;
        if (!db_) return result;

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
            // Basic deserialization
            result.records.push_back(std::move(rec));
            result.total_count++;
        }

        sqlite3_finalize(stmt);
        return result;
    }

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

    Status Flush() override {
        if (db_) Execute("PRAGMA wal_checkpoint(PASSIVE)");
        return Status::Ok();
    }

    Status Compact() override {
        if (db_) Execute("VACUUM");
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
    Status CreateTables() {
        int rc;
        rc = Execute(
            "CREATE TABLE IF NOT EXISTS records ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  pipeline TEXT NOT NULL,"
            "  timestamp_ns INTEGER NOT NULL,"
            "  labels_json TEXT,"
            "  fields_json TEXT"
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
            "  stack_json TEXT,"
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
            "  data BLOB"
            ")");
        if (rc != SQLITE_OK) return Status::Error(StatusCode::kInternal, "Create profiles table failed");

        Execute("CREATE INDEX IF NOT EXISTS idx_records_ts ON records(pipeline, timestamp_ns)");
        Execute("CREATE INDEX IF NOT EXISTS idx_samples_ts ON stack_samples(pipeline, timestamp_ns)");
        Execute("CREATE INDEX IF NOT EXISTS idx_profiles_ts ON profiles(pipeline, start_ns)");

        return Status::Ok();
    }

    int Execute(const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK && err) {
            IL_ERROR("SQLite error: %s (SQL: %s)", err, sql);
            sqlite3_free(err);
        }
        return rc;
    }

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

    static std::string SerializeFields(
        const std::unordered_map<std::string_view, FieldValue>& fields) {
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        for (auto& [k, v] : fields) {
            if (!first) ss << ",";
            ss << "\"" << k << "\":";
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

    sqlite3* db_ = nullptr;
    std::string db_path_;
};

// Register SQLite backend
static bool _reg_sqlite = [] {
    StorageFactory::Instance().Register("sqlite", [] {
        return std::make_unique<SqliteBackend>();
    });
    return true;
}();

}  // namespace illuminator
