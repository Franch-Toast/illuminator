#pragma once

#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// Exposes metrics in Prometheus exposition format.
// Metrics are accumulated and can be scraped via HTTP /metrics endpoint.
class PrometheusSink : public SinkPlugin {
public:
    const char* Name() const override { return "prometheus_exposition"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        prefix_ = config["prefix"].AsString("illuminator");
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& rec : batch->records()) {
            std::string label_str = BuildLabelString(rec.labels);

            for (auto& [key, val] : rec.fields) {
                std::string metric_name = SanitizeName(prefix_ + "_" + std::string(key));
                double value = ExtractDouble(val);
                std::string full_key = metric_name + label_str;
                gauges_[full_key] = value;
                metric_help_[metric_name] = metric_name;
            }
        }

        return Status::Ok();
    }

    // Generate Prometheus exposition format text
    std::string Expose() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;

        for (auto& [name, _] : metric_help_) {
            ss << "# HELP " << name << " Illuminator metric\n";
            ss << "# TYPE " << name << " gauge\n";
        }

        for (auto& [key, value] : gauges_) {
            ss << key << " " << value << "\n";
        }

        return ss.str();
    }

private:
    static std::string SanitizeName(const std::string& name) {
        std::string result = name;
        for (char& c : result) {
            if (!isalnum(c) && c != '_') c = '_';
        }
        return result;
    }

    static std::string BuildLabelString(const std::vector<Label>& labels) {
        if (labels.empty()) return "";
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        for (auto& l : labels) {
            if (!first) ss << ",";
            ss << l.key << "=\"" << l.value << "\"";
            first = false;
        }
        ss << "}";
        return ss.str();
    }

    static double ExtractDouble(const FieldValue& val) {
        struct Vis {
            double operator()(std::monostate) const { return 0; }
            double operator()(bool v) const { return v ? 1.0 : 0.0; }
            double operator()(int64_t v) const { return static_cast<double>(v); }
            double operator()(uint64_t v) const { return static_cast<double>(v); }
            double operator()(double v) const { return v; }
            double operator()(std::string_view) const { return 0; }
        };
        return std::visit(Vis{}, val);
    }

    std::string prefix_ = "illuminator";
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> gauges_;
    std::unordered_map<std::string, std::string> metric_help_;
};

IL_REGISTER_SINK("prometheus_exposition", PrometheusSink);

}  // namespace illuminator
