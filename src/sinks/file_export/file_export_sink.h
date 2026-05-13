// =============================================================================
// 文件：file_export_sink.h
// 模块：Illuminator 数据出口 - 文件导出
// 描述：
//   将数据批次（DataBatch）中的 Record 以 JSON Lines (JSONL) 格式
//   追加写入到指定文件。每条 Record 一行 JSON，便于后续离线分析。
//   注意：当前版本仅导出 Record，不导出 StackSample。
// =============================================================================

#pragma once

#include <cstdio>
#include <string>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// FileExportSink: 文件导出 Sink
// 将 Record 数据以 JSON Lines 格式追加写入到指定文件。
// 配置参数:
//   path - 输出文件路径，默认 /tmp/illuminator_output.jsonl
class FileExportSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "file_export"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析输出文件路径
    // 参数:
    //   config - 配置项，包含 path 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        auto path = config["path"].AsString("/tmp/illuminator_output.jsonl");
        path_ = std::string(path);
        return Status::Ok();
    }

    // 启动 Sink：以追加模式打开输出文件
    // 返回:
    //   Status::Ok() 表示成功；kInternal 表示文件无法打开
    Status Start() override {
        file_ = fopen(path_.c_str(), "a");
        if (!file_) {
            return Status::Error(StatusCode::kInternal,
                "Cannot open file: " + path_);
        }
        return Status::Ok();
    }

    // 停止 Sink：关闭文件句柄
    // 返回:
    //   Status::Ok() 表示成功
    Status Stop() override {
        if (file_) {
            fclose(file_);
            file_ = nullptr;
        }
        return Status::Ok();
    }

    // 将 DataBatch 中的 Record 写入文件
    // 每条 Record 输出为一行 JSON。
    // 参数:
    //   batch - 待导出的数据批次
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!file_ || !batch) return Status::Ok();

        for (auto& rec : batch->records()) {
            auto ns = TimestampToNanos(rec.timestamp);
            fprintf(file_, "{\"ts\":%lu,\"labels\":{", ns);

            // 输出标签字典
            bool first = true;
            for (auto& label : rec.labels) {
                if (!first) fprintf(file_, ",");
                fprintf(file_, "\"%.*s\":\"%.*s\"",
                        static_cast<int>(label.key.size()), label.key.data(),
                        static_cast<int>(label.value.size()), label.value.data());
                first = false;
            }

            // 输出字段字典
            fprintf(file_, "},\"fields\":{");
            first = true;
            for (auto& [key, val] : rec.fields) {
                if (!first) fprintf(file_, ",");
                fprintf(file_, "\"%.*s\":", static_cast<int>(key.size()), key.data());
                PrintValue(val);
                first = false;
            }
            fprintf(file_, "}}\n");
        }

        return Status::Ok();
    }

    // 刷新文件缓冲区，确保数据落盘
    // 返回:
    //   Status::Ok() 表示成功
    Status Flush() override {
        if (file_) fflush(file_);
        return Status::Ok();
    }

private:
    // 将 FieldValue 以 JSON 类型格式输出到文件
    // 参数:
    //   val - 要输出的字段值
    void PrintValue(const FieldValue& val) {
        struct Visitor {
            FILE* f;
            void operator()(std::monostate) const { fprintf(f, "null"); }
            void operator()(bool v) const { fprintf(f, "%s", v ? "true" : "false"); }
            void operator()(int64_t v) const { fprintf(f, "%ld", v); }
            void operator()(uint64_t v) const { fprintf(f, "%lu", v); }
            void operator()(double v) const { fprintf(f, "%.6f", v); }
            void operator()(std::string_view v) const {
                fprintf(f, "\"%.*s\"", static_cast<int>(v.size()), v.data());
            }
        };
        std::visit(Visitor{file_}, val);
    }

    std::string path_;       // 输出文件路径
    FILE* file_ = nullptr;   // 文件句柄，未打开时为 nullptr
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("file_export", FileExportSink);

}  // namespace illuminator
