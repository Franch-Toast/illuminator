// ============================================================================
// Illuminator 存储后端抽象接口 — 可插拔持久化层
// ============================================================================
//
// 本文件定义了 Illuminator 的存储抽象层，使得可以灵活替换持久化实现。
// 当前内置 SQLite 实现，未来可扩展 RocksDB、influxdb 等后端。
//
// 核心数据结构：
// ==============
// - ProfileMeta:    存储的性能剖析元数据
// - TimeRange:      时间范围查询条件
// - QueryRequest:   存储查询请求
// - QueryResult:    查询结果（Record 列表 + StackSample 列表）
//
// StorageBackend 抽象接口（所有持久化必须实现的 9 个方法）：
// ==========================================================
// Init(dir, config)        — 初始化后端，指定数据目录和配置
// WriteRecords(pipe, recs)  — 写入时间序列记录
// WriteStackSamples(pipe, samples) — 写入堆栈采样
// WriteProfile(meta, data)  — 写入原始 profile 数据（如 pprof 字节）
// Query(req)                — 查询历史数据
// ListProfiles(pipe, range) — 列出 profile 列表
// Flush() / Compact() / Close() — 生命周期管理
//
// StorageFactory — 存储后端工厂
// =================================
// 类似 PluginRegistry，提供"按名称创建存储后端实例"的能力
// - Register(name, creator): 注册工厂函数
// - Create(name): 创建实例
// - Available(): 列出可用后端
// ============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/status.h"
#include "core/engine/data_batch.h"

namespace illuminator {

// 存储的 profile 元数据
struct ProfileMeta {
    std::string pipeline_name;                              // 来源管道名
    std::string profile_type;   // 类型："cpu","memory","io","sched","net"
    uint64_t start_time_ns = 0;                             // 开始时间
    uint64_t end_time_ns = 0;                               // 结束时间
    uint64_t sample_count = 0;                              // 采样数量
    std::unordered_map<std::string, std::string> labels;    // 附加标签
};

// 时间范围查询条件（纳秒时间戳）
struct TimeRange {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
};

// 历史数据查询请求
struct QueryRequest {
    std::string pipeline_name;   // 管道名称（过滤条件）
    std::string metric_name;     // 指标名称（预留）
    TimeRange time_range;        // 时间范围
    uint32_t limit = 1000;       // 最大返回条数
    std::string order_by;        // 排序字段
    bool descending = true;      // 是否降序
};

// 查询结果
struct QueryResult {
    std::vector<Record> records;          // 指标记录
    std::vector<StackSample> stack_samples; // 堆栈采样
    uint64_t total_count = 0;             // 总命中数
    bool has_more = false;                // 是否有更多数据

    // 反序列化时的字符串存储池，Record 中 string_view 指向这里
    std::vector<std::unique_ptr<std::string>> string_pool;

    std::string_view Intern(const std::string& s) {
        string_pool.push_back(std::make_unique<std::string>(s));
        return *string_pool.back();
    }
};

// ---- StorageBackend 抽象接口 ----
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual const char* Name() const = 0;

    // 初始化：创建目录、打开数据库连接、建表
    virtual Status Init(const std::string& data_dir,
                        const ConfigValue& config) = 0;

    // 写入时间序列指标记录
    virtual Status WriteRecords(const std::string& pipeline_name,
                                const std::vector<Record>& records) = 0;

    // 写入性能剖析的堆栈采样
    virtual Status WriteStackSamples(const std::string& pipeline_name,
                                     const std::vector<StackSample>& samples) = 0;

    // 写入原始 profile 数据（如 pprof protobuf 二进制）
    virtual Status WriteProfile(const ProfileMeta& meta,
                                const std::vector<uint8_t>& data) = 0;

    // 查询历史数据
    virtual StatusOr<QueryResult> Query(const QueryRequest& req) = 0;

    // 列出已存储的 profile 列表
    virtual StatusOr<std::vector<ProfileMeta>> ListProfiles(
        const std::string& pipeline_name,
        const TimeRange& range) = 0;

    // 生命周期
    virtual Status Flush() = 0;       // 刷出缓冲数据
    virtual Status Compact() = 0;     // 压缩/优化存储
    virtual Status Close() = 0;       // 关闭连接

    // 磁盘占用（字节），默认返回 0
    virtual uint64_t DiskUsageBytes() const { return 0; }

    // 执行原始只读 SQL 查询，返回 JSON 字符串
    virtual StatusOr<std::string> ExecuteRawQuery(const std::string& sql) {
        return Status::Error(StatusCode::kUnimplemented,
                             "ExecuteRawQuery not supported by this backend");
    }
};

// ============================================================================
// StorageFactory — 存储后端工厂
// ============================================================================
// 类似 PluginRegistry，管理存储后端的注册和创建。
// 内置的 SQLite 后端通过静态初始化器自动注册。
class StorageFactory {
public:
    static StorageFactory& Instance() {
        static StorageFactory inst;
        return inst;
    }

    // 工厂函数类型
    using Creator = std::function<std::unique_ptr<StorageBackend>()>;

    // 注册一个存储后端工厂
    void Register(const std::string& name, Creator creator) {
        creators_[name] = std::move(creator);
    }

    // 按名称创建存储后端实例（未注册时返回 nullptr）
    std::unique_ptr<StorageBackend> Create(const std::string& name) {
        auto it = creators_.find(name);
        if (it == creators_.end()) return nullptr;
        return it->second();
    }

    // 列出所有已注册后端名称
    std::vector<std::string> Available() const {
        std::vector<std::string> names;
        for (auto& [k, _] : creators_) names.push_back(k);
        return names;
    }

private:
    std::unordered_map<std::string, Creator> creators_;
};

}  // namespace illuminator
