// =============================================================================
// 文件：console_sink.h
// 模块：Illuminator 数据出口 - 控制台输出
// 描述：
//   将数据批次（DataBatch）输出到标准输出流 (stdout)。
//   支持两种输出格式：
//     1. text - 人类可读的键值对格式
//     2. json - JSON 格式（便于机器解析和日志收集）
//   堆栈采样以缩进树形格式输出。
// =============================================================================

#pragma once

#include <cstdio>
#include <string>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"
#include "cli/json_serializer.h"

namespace illuminator {

// ConsoleSink: 控制台输出 Sink
// 将 Record 和 StackSample 数据输出到 stdout。
// 配置参数:
//   format - 输出格式："text"（默认）或 "json"
class ConsoleSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "console_output"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中读取输出格式设置
    // 参数:
    //   config - 配置项，包含 format 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        auto fmt = config["format"].AsString("text");
        json_mode_ = (fmt == "json");
        return Status::Ok();
    }

    // 将 DataBatch 中的数据写入 stdout
    // 参数:
    //   batch - 要输出的数据批次
    // 返回:
    //   Status::Ok() 表示操作成功
    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        // 输出 Record 数据
        for (auto& rec : batch->records()) {
            if (json_mode_) {
                WriteRecordJson(rec);
            } else {
                WriteRecordText(rec);
            }
        }

        // 输出 StackSample 数据
        for (auto& sample : batch->stack_samples()) {
            WriteStackSample(sample);
        }

        return Status::Ok();
    }

private:
    // FieldPrinter: text 模式下单个字段的打印辅助器
    // 使用 std::visit 根据字段值的实际类型选择格式化方式
    struct FieldPrinter {
        FILE* out;
        std::string_view key;

        // 空值不打印
        void operator()(std::monostate) const {}

        // 布尔值：输出 true/false
        void operator()(bool v) const {
            fprintf(out, " %.*s=%s", static_cast<int>(key.size()), key.data(),
                    v ? "true" : "false");
        }

        // 有符号整数
        void operator()(int64_t v) const {
            fprintf(out, " %.*s=%ld", static_cast<int>(key.size()), key.data(), v);
        }

        // 无符号整数
        void operator()(uint64_t v) const {
            fprintf(out, " %.*s=%lu", static_cast<int>(key.size()), key.data(), v);
        }

        // 浮点数：保留 3 位小数
        void operator()(double v) const {
            fprintf(out, " %.*s=%.3f", static_cast<int>(key.size()), key.data(), v);
        }

        // 字符串
        void operator()(std::string_view v) const {
            fprintf(out, " %.*s=%.*s", static_cast<int>(key.size()), key.data(),
                    static_cast<int>(v.size()), v.data());
        }
    };

    // 以 text 格式输出一条 Record
    // 格式: [<timestamp_ns>] {<label_key>=<label_value>} <field_key>=<field_value> ...
    // 参数:
    //   rec - 要输出的 Record
    void WriteRecordText(const Record& rec) {
        auto ns = TimestampToNanos(rec.timestamp);
        fprintf(stdout, "[%lu]", ns);

        // 输出标签（花括号包围）
        for (auto& label : rec.labels) {
            fprintf(stdout, " {%.*s=%.*s}",
                    static_cast<int>(label.key.size()), label.key.data(),
                    static_cast<int>(label.value.size()), label.value.data());
        }

        // 输出字段
        for (auto& [key, val] : rec.fields) {
            std::visit(FieldPrinter{stdout, key}, val);
        }

        fprintf(stdout, "\n");
    }

    void WriteRecordJson(const Record& rec) {
        auto s = RecordToJson(rec).dump();
        fprintf(stdout, "%s\n", s.c_str());
    }

    // 输出堆栈采样信息
    // 格式:
    //   [stack] pid=<pid> tid=<tid> comm=<comm> count=<count>
    //     <function_name>+0x<addr> (<file_name>:<line>)
    //   ...
    // 参数:
    //   sample - 要输出的 StackSample
    void WriteStackSample(const StackSample& sample) {
        fprintf(stdout, "[stack] pid=%u tid=%u comm=%.*s count=%lu\n",
                sample.pid, sample.tid,
                static_cast<int>(sample.comm.size()), sample.comm.data(),
                sample.count);
        // 逐帧输出用户态调用栈（自底向上）
        for (auto& frame : sample.user_stack) {
            fprintf(stdout, "  %.*s+0x%lx",
                    static_cast<int>(frame.function_name.size()),
                    frame.function_name.data(), frame.address);
            // 若有源文件信息则附加显示
            if (!frame.file_name.empty()) {
                fprintf(stdout, " (%.*s:%u)",
                        static_cast<int>(frame.file_name.size()),
                        frame.file_name.data(), frame.line_number);
            }
            fprintf(stdout, "\n");
        }
    }

    // 当前输出模式是否为 JSON
    bool json_mode_ = false;
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("console_output", ConsoleSink);

}  // namespace illuminator
