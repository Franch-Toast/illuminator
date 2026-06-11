// ============================================================================
// Illuminator 配置系统
// ============================================================================
//
// 本文件定义了三层配置结构：
// 1. ConfigValue — 扁平的键值对配置容器，支持嵌套 key 的点号表示法
//    设计目标：避免递归类型问题，简化跨语言边界的配置传递
// 2. PipelineConfig — 单条数据处理管道的配置结构
//    （Source → Processors → Aggregator → Sinks）
// 3. GlobalConfig — 全局配置，包含日志、服务器和多条管道配置
//
// 设计理念：
// - ConfigValue 使用扁平化存储（string->string map），嵌套通过 "parent.child" 键名实现
// - 不引入复杂类型层级，方便序列化和反序列化（无论来源是 YAML/JSON/命令行）
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdlib>

#include "core/common/status.h"

namespace illuminator {

// ---- ConfigValue: 扁平化键值配置容器 ----
//
// 核心设计：所有配置最终展开为 string → string 的映射表。
// 嵌套配置通过点号（.）连接键名：
//   server.http.listen → {"server.http.listen": "0.0.0.0:9527"}
//
// 支持方便的读写操作：
// - config.Set("key", value);               写入标量值
// - auto sub = config["parent"];             读取子值（带点号前缀解析）
// - config["key"].AsInt(default);           读取并转换类型
class ConfigValue {
public:
    ConfigValue() = default;

    ConfigValue(int64_t v) { values_[""] = std::to_string(v); }
    ConfigValue(const char* v) { values_[""] = v; }
    ConfigValue(std::string v) { values_[""] = std::move(v); }

    void Set(const std::string& key, const std::string& value) {
        values_[key] = value;
    }

    void Set(const std::string& key, const char* value) {
        values_[key] = value;
    }

    void Set(const std::string& key, int64_t value) {
        values_[key] = std::to_string(value);
    }

    // ---- 下标运算符：只读取子配置 ----
    [[nodiscard]] ConfigValue operator[](const std::string& key) const {
        ConfigValue child;
        auto it = values_.find(key);
        if (it != values_.end()) {
            child.values_[""] = it->second;
        }
        std::string prefix = key + ".";
        for (auto& [k, v] : values_) {
            if (k.substr(0, prefix.size()) == prefix) {
                child.values_[k.substr(prefix.size())] = v;
            }
        }
        return child;
    }

    // ---- 值查询方法 ----

    // 判断配置是否为空（无任何键值对）
    [[nodiscard]] bool IsNull() const { return values_.empty(); }

    // 读取为 64 位整数（失败时返回默认值）
    [[nodiscard]] int64_t AsInt(int64_t def = 0) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        try { return std::stoll(it->second); }
        catch (...) { return def; }
    }

    // 读取为双精度浮点数
    [[nodiscard]] double AsDouble(double def = 0.0) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }

    // 读取为字符串
    [[nodiscard]] std::string AsString(const std::string& def = "") const {
        auto it = values_.find("");
        return it != values_.end() ? it->second : def;
    }

    // 读取为布尔值（"true" 或 "1" 视作 true）
    [[nodiscard]] bool AsBool(bool def = false) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        return it->second == "true" || it->second == "1";
    }

    // 读取为字符串列表（从索引化存储 "0","1",... 中恢复）
    [[nodiscard]] std::vector<std::string> AsList() const {
        auto size_it = values_.find("_size");
        if (size_it == values_.end()) {
            auto scalar = values_.find("");
            if (scalar != values_.end() && !scalar->second.empty()) {
                return {scalar->second};
            }
            return {};
        }
        size_t n = 0;
        try { n = std::stoull(size_it->second); } catch (...) { return {}; }
        std::vector<std::string> result;
        result.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            auto it = values_.find(std::to_string(i));
            if (it != values_.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    // 获取原始映射表（用于遍历）
    [[nodiscard]] const std::unordered_map<std::string, std::string>& Raw() const {
        return values_;
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

// ---- PipelineConfig: 单条管道配置 ----
//
// 每条管道 = 一个 Source + 可选的多个 Processor + 可选的 Aggregator + 多个 Sink
// 数据流向：Source → Processor1 → Processor2 → ... → Aggregator → Sink1, Sink2, ...
struct PipelineConfig {
    std::string name;  // 管道唯一名称

    // 阶段配置：类型名 + 配置参数
    struct StageConfig {
        std::string type;      // 插件类型名（如 "cpu_utilization"、"filter"）
        ConfigValue config;    // 该阶段的配置参数
    };

    StageConfig source;                         // 数据源（必需）
    std::vector<StageConfig> processors;        // 处理器链（可选，按顺序执行）
    std::optional<StageConfig> aggregator;      // 聚合器（可选，用于时间窗口聚合）
    std::vector<StageConfig> sinks;             // 数据出口（至少一个）
};

// ---- EngineConfig: 管道引擎全局配置 ----
struct EngineConfig {
    uint32_t collect_pool_threads = 0;  // CollectPool 线程数（0 = auto: 2）
    uint32_t sink_pool_threads = 0;     // SinkPool 线程数（0 = auto: CPU核数/2）

    struct ChannelConfig {
        std::string size = "medium";            // small(1024) | medium(4096) | large(16384)
        std::string drop_policy = "drop_newest"; // drop_newest | drop_oldest
        double backpressure_high = 0.8;
        double backpressure_low = 0.2;
    } channel;
};

// ---- GlobalConfig: 全局配置结构 ----
//
// 根级配置，包含整个 Illuminator 实例的配置信息
struct GlobalConfig {
    // 全局设置
    std::string log_level = "info";
    std::string log_file;                          // 日志文件路径（空 = 仅控制台）
    size_t log_max_size = 10 * 1024 * 1024;        // 单文件最大 10MB
    size_t log_max_files = 3;                      // 轮转文件数
    std::string data_dir = "/var/lib/illuminator";
    std::vector<std::string> plugin_dirs;
    bool auto_start = false;                       // true=启动时全量运行pipeline; false=按需启动

    // 服务器配置
    struct ServerConfig {
        bool http_enabled = true;
        std::string http_listen = "127.0.0.1:9527";
        bool ws_enabled = true;
        std::string auth_token;
    } server;

    // 管道引擎配置
    EngineConfig engine;

    // 关联的管道配置列表
    std::vector<PipelineConfig> pipelines;
};

}  // namespace illuminator
