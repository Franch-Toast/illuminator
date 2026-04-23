#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdlib>

#include "core/common/status.h"

namespace illuminator {

// Flat key-value configuration. Nested config is flattened with dot-notation.
// This avoids recursive type issues with older compilers.
class ConfigValue {
public:
    ConfigValue() = default;
    ConfigValue(int64_t v) { values_[""] = std::to_string(v); }
    ConfigValue(const char* v) { values_[""] = v; }
    ConfigValue(std::string v) { values_[""] = std::move(v); }

    void Set(const std::string& key, const std::string& value) {
        values_[key] = value;
    }

    ConfigValue operator[](const std::string& key) const {
        ConfigValue child;
        auto it = values_.find(key);
        if (it != values_.end()) {
            child.values_[""] = it->second;
        }
        // Also propagate nested keys
        std::string prefix = key + ".";
        for (auto& [k, v] : values_) {
            if (k.substr(0, prefix.size()) == prefix) {
                child.values_[k.substr(prefix.size())] = v;
            }
        }
        return child;
    }

    ConfigValue& operator[](const std::string& key) {
        // Return self for mutation; key stored as ""
        current_key_ = key;
        return *this;
    }

    ConfigValue& operator=(int64_t v) {
        values_[current_key_] = std::to_string(v);
        return *this;
    }

    ConfigValue& operator=(const std::string& v) {
        values_[current_key_] = v;
        return *this;
    }

    ConfigValue& operator=(const char* v) {
        values_[current_key_] = v;
        return *this;
    }

    bool IsNull() const { return values_.empty(); }

    int64_t AsInt(int64_t def = 0) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        try { return std::stoll(it->second); }
        catch (...) { return def; }
    }

    double AsDouble(double def = 0.0) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }

    std::string AsString(const std::string& def = "") const {
        auto it = values_.find("");
        return it != values_.end() ? it->second : def;
    }

    bool AsBool(bool def = false) const {
        auto it = values_.find("");
        if (it == values_.end()) return def;
        return it->second == "true" || it->second == "1";
    }

    const std::unordered_map<std::string, std::string>& Raw() const {
        return values_;
    }

private:
    std::unordered_map<std::string, std::string> values_;
    std::string current_key_;
};

struct PipelineConfig {
    std::string name;
    struct StageConfig {
        std::string type;
        ConfigValue config;
    };
    StageConfig source;
    std::vector<StageConfig> processors;
    std::optional<StageConfig> aggregator;
    std::vector<StageConfig> sinks;
};

struct GlobalConfig {
    std::string log_level = "info";
    std::string data_dir = "/var/lib/illuminator";
    std::vector<std::string> plugin_dirs;
    struct ServerConfig {
        bool http_enabled = true;
        std::string http_listen = "0.0.0.0:9527";
        bool ws_enabled = true;
    } server;
    std::vector<PipelineConfig> pipelines;
};

}  // namespace illuminator
