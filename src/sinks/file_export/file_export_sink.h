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
#include "serialization/json_serializer.h"

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
            auto s = RecordToJson(rec).dump();
            fprintf(file_, "%s\n", s.c_str());
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
    std::string path_;
    FILE* file_ = nullptr;
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("file_export", FileExportSink);

}  // namespace illuminator
