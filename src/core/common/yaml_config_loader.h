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
//   global.auto_start   -> GlobalConfig.auto_start
//   server.http.enabled -> GlobalConfig.server.http_enabled
//   server.http.listen  -> GlobalConfig.server.http_listen
//   engine.*            -> GlobalConfig.engine
// ============================================================================

#pragma once

#include <fstream>
#include <set>
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
            YAML::Node root = YAML::LoadFile(path);
            auto config = ParseConfig(root);
            auto valid = ValidateConfig(config);
            if (!valid.ok()) return valid;
            return config;
        } catch (const YAML::Exception& e) {
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("YAML parse error: ") + e.what());
        }
    }

    static StatusOr<GlobalConfig> LoadFromString(const std::string& yaml_str) {
        try {
            YAML::Node root = YAML::Load(yaml_str);
            auto config = ParseConfig(root);
            auto valid = ValidateConfig(config);
            if (!valid.ok()) return valid;
            return config;
        } catch (const YAML::Exception& e) {
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("YAML parse error: ") + e.what());
        }
    }

private:
    // ---- 语义校验：检查配置的合法性 ----
    static Status ValidateConfig(const GlobalConfig& cfg) {
        static const std::set<std::string> kValidLogLevels =
            {"trace", "debug", "info", "warn", "error"};
        if (!kValidLogLevels.count(cfg.log_level)) {
            return Status::Error(StatusCode::kInvalidArgument,
                "Invalid log_level '" + cfg.log_level +
                "'; valid: trace, debug, info, warn, error");
        }

        static const std::set<std::string> kValidChannelSizes =
            {"small", "medium", "large"};
        if (!kValidChannelSizes.count(cfg.engine.channel.size)) {
            return Status::Error(StatusCode::kInvalidArgument,
                "Invalid channel.size '" + cfg.engine.channel.size +
                "'; valid: small, medium, large");
        }

        static const std::set<std::string> kValidDropPolicies =
            {"drop_newest", "drop_oldest"};
        if (!kValidDropPolicies.count(cfg.engine.channel.drop_policy)) {
            return Status::Error(StatusCode::kInvalidArgument,
                "Invalid channel.drop_policy '" + cfg.engine.channel.drop_policy +
                "'; valid: drop_newest, drop_oldest");
        }

        if (cfg.engine.channel.backpressure_high <=
            cfg.engine.channel.backpressure_low) {
            return Status::Error(StatusCode::kInvalidArgument,
                "backpressure_high must be > backpressure_low");
        }

        if (cfg.engine.channel.backpressure_high > 1.0 ||
            cfg.engine.channel.backpressure_low < 0.0) {
            return Status::Error(StatusCode::kInvalidArgument,
                "backpressure thresholds must be in [0.0, 1.0]");
        }

        return Status::Ok();
    }

    // ---- 解析根节点为 GlobalConfig ----
    static GlobalConfig ParseConfig(const YAML::Node& root) {
        GlobalConfig config;

        // 解析 [global] 节
        if (auto global = root["global"]) {
            if (global["log_level"])
                config.log_level = global["log_level"].as<std::string>();
            if (global["log_file"])
                config.log_file = global["log_file"].as<std::string>();
            if (global["log_max_size"])
                config.log_max_size = global["log_max_size"].as<size_t>(10485760);
            if (global["log_max_files"])
                config.log_max_files = global["log_max_files"].as<size_t>(3);
            if (global["data_dir"])
                config.data_dir = global["data_dir"].as<std::string>();
            if (global["plugin_dirs"]) {
                for (const auto& dir : global["plugin_dirs"])
                    config.plugin_dirs.push_back(dir.as<std::string>());
            }
            if (global["auto_start"])
                config.auto_start = global["auto_start"].as<bool>();
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

        return config;
    }
};

}  // namespace illuminator
