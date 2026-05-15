// ============================================================================
// Illuminator YAML 配置加载器 — 从 YAML 文件/字符串解析全局配置
// ============================================================================
//
// 本文件实现了将 YAML 配置文件解析为 GlobalConfig 结构体的功能。
// 使用 yaml-cpp 库进行 YAML 节点树解析，再递归转换为内部配置格式。
//
// 核心功能：
// ==========
// 1. LoadFromFile(path)  — 从 YAML 文件加载配置
// 2. LoadFromString(str)  — 从 YAML 字符串加载配置（用于测试和内联配置）
//
// 配置结构映射：
// ==============
// YAML 文件中的结构对应关系：
//   global.log_level    -> GlobalConfig.log_level
//   global.data_dir     -> GlobalConfig.data_dir
//   global.plugin_dirs  -> GlobalConfig.plugin_dirs
//   server.http.enabled -> GlobalConfig.server.http_enabled
//   server.http.listen  -> GlobalConfig.server.http_listen
//   pipelines.<name>.source.type   -> PipelineConfig.source.type
//   pipelines.<name>.processors[]  -> PipelineConfig.processors[]
//   pipelines.<name>.aggregator    -> PipelineConfig.aggregator
//   pipelines.<name>.sinks[]       -> PipelineConfig.sinks[]
//
// ParseConfigValue 递归策略：
// =============================
// - 标量值: 直接存为 string
// - 序列值: 用逗号连接后存为 string（如 [a, b, c] -> "a,b,c"）
// - 嵌套映射: 递归展开为点号分隔的扁平 key（如 {a: {b: 1}} -> "a.b"="1"）
// ============================================================================

#pragma once

#include <fstream>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"

namespace illuminator {

class YamlConfigLoader {
public:
    // ---- 从 YAML 文件加载配置 ----
    // 参数: path - YAML 配置文件的路径
    // 返回: StatusOr<GlobalConfig>，失败时返回 YAML 解析错误
    static StatusOr<GlobalConfig> LoadFromFile(const std::string& path) {
        try {
            YAML::Node root = YAML::LoadFile(path);  // yaml-cpp 自动处理文件读取
            return ParseConfig(root);                 // 递归解析为 GlobalConfig
        } catch (const YAML::Exception& e) {
            // 捕获 YAML 语法错误，返回明确的错误消息
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("YAML parse error: ") + e.what());
        }
    }

    // ---- 从 YAML 字符串加载配置 ----
    // 用于测试、内联配置或 API 动态传入配置的场景
    static StatusOr<GlobalConfig> LoadFromString(const std::string& yaml_str) {
        try {
            YAML::Node root = YAML::Load(yaml_str);
            return ParseConfig(root);
        } catch (const YAML::Exception& e) {
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("YAML parse error: ") + e.what());
        }
    }

private:
    // ---- 解析根节点为 GlobalConfig ----
    // 遍历 YAML 树的各个顶层节点，填充 GlobalConfig 的各字段
    static GlobalConfig ParseConfig(const YAML::Node& root) {
        GlobalConfig config;

        // 解析 [global] 节
        if (auto global = root["global"]) {
            if (global["log_level"])
                config.log_level = global["log_level"].as<std::string>();
            if (global["data_dir"])
                config.data_dir = global["data_dir"].as<std::string>();
            // plugin_dirs 是一个数组，遍历解析
            if (global["plugin_dirs"]) {
                for (const auto& dir : global["plugin_dirs"])
                    config.plugin_dirs.push_back(dir.as<std::string>());
            }
        }

        // 解析 [server] 节
        if (auto server = root["server"]) {
            // HTTP 子配置
            if (auto http = server["http"]) {
                if (http["enabled"])
                    config.server.http_enabled = http["enabled"].as<bool>();
                if (http["listen"])
                    config.server.http_listen = http["listen"].as<std::string>();
            }
            // WebSocket 子配置
            if (auto ws = server["websocket"]) {
                if (ws["enabled"])
                    config.server.ws_enabled = ws["enabled"].as<bool>();
            }
        }

        // 解析 [engine] 节
        if (auto engine = root["engine"]) {
            if (engine["collect_pool_threads"])
                config.engine.collect_pool_threads =
                    engine["collect_pool_threads"].as<unsigned>(0);
            if (engine["sink_pool_threads"])
                config.engine.sink_pool_threads =
                    engine["sink_pool_threads"].as<unsigned>(0);
            if (auto ch = engine["channel"]) {
                if (ch["size"])
                    config.engine.channel.size = ch["size"].as<std::string>("medium");
                if (ch["drop_policy"])
                    config.engine.channel.drop_policy =
                        ch["drop_policy"].as<std::string>("drop_newest");
                if (ch["backpressure_high"])
                    config.engine.channel.backpressure_high =
                        ch["backpressure_high"].as<double>(0.8);
                if (ch["backpressure_low"])
                    config.engine.channel.backpressure_low =
                        ch["backpressure_low"].as<double>(0.2);
            }
        }

        // 解析 [pipelines] 节
        // YAML 格式示例：
        //   pipelines:
        //     cpu_utilization:       ← 键名就是管道名称
        //       source:
        //         type: cpu_utilization
        //         config: { ... }
        //       sinks:
        //         - type: local_storage
        if (auto pipelines = root["pipelines"]) {
            // pipelines 是一个 Map，key 是管道名，value 是管道配置
            for (auto it = pipelines.begin(); it != pipelines.end(); ++it) {
                PipelineConfig pc;
                pc.name = it->first.as<std::string>();  // 管道名称
                auto pipe_node = it->second;

                // ---- 解析 Source 阶段 ----
                if (auto source = pipe_node["source"]) {
                    pc.source.type = source["type"].as<std::string>("");
                    if (auto cfg = source["config"]) {
                        pc.source.config = ParseConfigValue(cfg);  // 递归解析配置子节点
                    }
                }

                // ---- 解析 Processor 链 ----
                // Processors 是一个数组，按顺序执行
                if (auto processors = pipe_node["processors"]) {
                    for (const auto& proc : processors) {
                        PipelineConfig::StageConfig sc;
                        sc.type = proc["type"].as<std::string>("");
                        if (auto cfg = proc["config"]) {
                            sc.config = ParseConfigValue(cfg);
                        }
                        pc.processors.push_back(std::move(sc));
                    }
                }

                // ---- 解析 Aggregator（可选） ----
                if (auto agg = pipe_node["aggregator"]) {
                    PipelineConfig::StageConfig sc;
                    sc.type = agg["type"].as<std::string>("");
                    if (auto cfg = agg["config"]) {
                        sc.config = ParseConfigValue(cfg);
                    }
                    pc.aggregator = std::move(sc);
                }

                // ---- 解析 Sink 列表 ----
                if (auto sinks = pipe_node["sinks"]) {
                    for (const auto& sink : sinks) {
                        PipelineConfig::StageConfig sc;
                        sc.type = sink["type"].as<std::string>("");
                        if (auto cfg = sink["config"]) {
                            sc.config = ParseConfigValue(cfg);
                        }
                        pc.sinks.push_back({sc.type, sc.config});
                    }
                }

                config.pipelines.push_back(std::move(pc));
            }
        }

        return config;
    }

    // ---- 递归解析 YAML 节点为 ConfigValue ----
    // 这是 YAML 树结构到扁平 KV 映射的核心转换函数
    // 处理三种节点类型：
    //   - Map:   递归展平，子键用点号连接
    //   - Scalar: 直接存为键值对
    //   - Sequence: 用逗号连接后存储
    static ConfigValue ParseConfigValue(const YAML::Node& node) {
        ConfigValue cv;
        if (!node || node.IsNull()) return cv;  // 空节点返回空配置

        if (node.IsMap()) {
            // 处理嵌套映射：递归转换为扁平结构
            for (auto it = node.begin(); it != node.end(); ++it) {
                auto key = it->first.as<std::string>();
                if (it->second.IsScalar()) {
                    cv.Set(key, it->second.as<std::string>());
                } else if (it->second.IsSequence()) {
                    std::string joined;
                    for (size_t i = 0; i < it->second.size(); ++i) {
                        if (i > 0) joined += ",";
                        joined += it->second[i].as<std::string>();
                    }
                    cv.Set(key, joined);
                } else if (it->second.IsMap()) {
                    auto nested = ParseConfigValue(it->second);
                    for (auto& [nk, nv] : nested.Raw()) {
                        std::string full_key = nk.empty() ? key : key + "." + nk;
                        cv.Set(full_key, nv);
                    }
                }
            }
        } else if (node.IsScalar()) {
            cv.Set("", node.as<std::string>());
        }

        return cv;
    }
};

}  // namespace illuminator
