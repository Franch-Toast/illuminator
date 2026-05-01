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
    static StatusOr<GlobalConfig> LoadFromFile(const std::string& path) {
        try {
            YAML::Node root = YAML::LoadFile(path);
            return ParseConfig(root);
        } catch (const YAML::Exception& e) {
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("YAML parse error: ") + e.what());
        }
    }

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
    static GlobalConfig ParseConfig(const YAML::Node& root) {
        GlobalConfig config;

        if (auto global = root["global"]) {
            if (global["log_level"])
                config.log_level = global["log_level"].as<std::string>();
            if (global["data_dir"])
                config.data_dir = global["data_dir"].as<std::string>();
            if (global["plugin_dirs"]) {
                for (const auto& dir : global["plugin_dirs"])
                    config.plugin_dirs.push_back(dir.as<std::string>());
            }
        }

        if (auto server = root["server"]) {
            if (auto http = server["http"]) {
                if (http["enabled"])
                    config.server.http_enabled = http["enabled"].as<bool>();
                if (http["listen"])
                    config.server.http_listen = http["listen"].as<std::string>();
            }
            if (auto ws = server["websocket"]) {
                if (ws["enabled"])
                    config.server.ws_enabled = ws["enabled"].as<bool>();
            }
        }

        if (auto pipelines = root["pipelines"]) {
            for (auto it = pipelines.begin(); it != pipelines.end(); ++it) {
                PipelineConfig pc;
                pc.name = it->first.as<std::string>();
                auto pipe_node = it->second;

                if (auto source = pipe_node["source"]) {
                    pc.source.type = source["type"].as<std::string>("");
                    if (auto cfg = source["config"]) {
                        pc.source.config = ParseConfigValue(cfg);
                    }
                }

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

                if (auto agg = pipe_node["aggregator"]) {
                    PipelineConfig::StageConfig sc;
                    sc.type = agg["type"].as<std::string>("");
                    if (auto cfg = agg["config"]) {
                        sc.config = ParseConfigValue(cfg);
                    }
                    pc.aggregator = std::move(sc);
                }

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

    static ConfigValue ParseConfigValue(const YAML::Node& node) {
        ConfigValue cv;
        if (!node || node.IsNull()) return cv;

        if (node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                auto key = it->first.as<std::string>();
                if (it->second.IsScalar()) {
                    cv[key] = it->second.as<std::string>();
                } else if (it->second.IsSequence()) {
                    std::string joined;
                    for (size_t i = 0; i < it->second.size(); ++i) {
                        if (i > 0) joined += ",";
                        joined += it->second[i].as<std::string>();
                    }
                    cv[key] = joined;
                } else if (it->second.IsMap()) {
                    auto nested = ParseConfigValue(it->second);
                    for (auto& [nk, nv] : nested.Raw()) {
                        std::string full_key = nk.empty() ? key : key + "." + nk;
                        cv[full_key] = nv;
                    }
                }
            }
        } else if (node.IsScalar()) {
            cv[""] = node.as<std::string>();
        }

        return cv;
    }
};

}  // namespace illuminator
