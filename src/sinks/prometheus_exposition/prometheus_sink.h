// =============================================================================
// 文件：prometheus_sink.h
// 模块：Illuminator 数据出口 - Prometheus 指标暴露
// 描述：
//   将 Record 数据转换为 Prometheus 指标格式，以 Gauge 类型暴露。
//   指标在内存中累积，可通过 HTTP /metrics 端点抓取。
//   支持自定义指标名前缀，自动对非法字符进行清理（sanitize）。
// =============================================================================

#pragma once

#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// PrometheusSink: Prometheus 指标暴露 Sink
// 将 Record 数据中的数值字段注册为 Prometheus Gauge 指标。
// 每条 Record 的标签转换为 Prometheus labels，字段值作为指标值。
// 配置参数:
//   prefix - 指标名统一前缀，默认 "illuminator"
// 输出格式: 标准 Prometheus Exposition Format
//   # HELP <metric_name> <description>
//   # TYPE <metric_name> gauge
//   <metric_name>{<labels>} <value>
class PrometheusSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "prometheus_exposition"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析指标名前缀
    // 参数:
    //   config - 配置项，包含 prefix 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        prefix_ = config["prefix"].AsString("illuminator");
        return Status::Ok();
    }

    // 将 DataBatch 中的 Record 转换为 Prometheus 指标并存储
    // 线程安全，使用互斥锁保护内部指标存储。
    // 参数:
    //   batch - 包含 Record 的数据批次
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& rec : batch->records()) {
            // 构造标签字符串（如 {pid="1234",cpu="0"}）
            std::string label_str = BuildLabelString(rec.labels);

            for (auto& [key, val] : rec.fields) {
                // 拼接前缀和字段名，并清理非法字符
                std::string metric_name = SanitizeName(prefix_ + "_" + std::string(key));
                double value = ExtractDouble(val);
                // 将指标名 + 标签作为唯一键存储
                std::string full_key = metric_name + label_str;
                gauges_[full_key] = value;
                metric_help_[metric_name] = metric_name;
            }
        }

        return Status::Ok();
    }

    // 生成标准 Prometheus Exposition 格式的文本
    // 线程安全。
    // 返回:
    //   格式化的 Prometheus 指标文本，可用于 /metrics HTTP 响应体
    std::string Expose() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;

        // 为每个指标名输出 HELP 和 TYPE 元数据头
        for (auto& [name, _] : metric_help_) {
            ss << "# HELP " << name << " Illuminator metric\n";
            ss << "# TYPE " << name << " gauge\n";
        }

        // 输出指标数据行
        for (auto& [key, value] : gauges_) {
            ss << key << " " << value << "\n";
        }

        return ss.str();
    }

private:
    // 清理指标名中的非法字符（非字母数字和下划线替换为下划线）
    // Prometheus 指标名只允许 [a-zA-Z0-9:_]
    // 参数:
    //   name - 原始名称
    // 返回:
    //   清理后的安全名称
    static std::string SanitizeName(const std::string& name) {
        std::string result = name;
        for (char& c : result) {
            if (!isalnum(c) && c != '_') c = '_';
        }
        return result;
    }

    // 构造 Prometheus 标签字符串
    // 格式: {key1="value1",key2="value2"} 或空字符串（无标签时）
    // 参数:
    //   labels - Record 的标签列表
    // 返回:
    //   格式化的标签字符串
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

    // 将 FieldValue 统一转换为 double 数值
    // 布尔值转为 1.0/0.0，字符串返回 0（Prometheus Gauge 仅支持数值）。
    // 参数:
    //   val - 字段值 (variant)
    // 返回:
    //   double 类型数值
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

    std::string prefix_ = "illuminator";                       // 指标名前缀
    mutable std::mutex mutex_;                                  // 保护指标存储的互斥锁（mutable 允许 const 方法加锁）
    std::unordered_map<std::string, double> gauges_;            // 指标全名 -> 数值
    std::unordered_map<std::string, std::string> metric_help_;  // 指标基础名 -> 帮助文本（去重用）
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("prometheus_exposition", PrometheusSink);

}  // namespace illuminator
