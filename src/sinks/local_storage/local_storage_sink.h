// =============================================================================
// 文件：local_storage_sink.h
// 模块：Illuminator 数据出口 - 本地存储
// 描述：
//   将数据批次持久化到本地存储后端（默认 SQLite）。
//   支持通用的 StorageBackend 接口，可替换为其他存储实现。
//   同时写入 Record 和 StackSample 数据。
// =============================================================================

#pragma once

#include <memory>
#include <string>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"
#include "storage/storage_backend.h"

namespace illuminator {

// LocalStorageSink: 本地存储 Sink
// 使用可插拔的存储后端将数据持久化到本地文件系统。
// 配置参数:
//   backend  - 存储后端类型名称（如 "sqlite"），默认 sqlite
//   path     - 数据存储目录，默认 /var/lib/illuminator
//   pipeline - 流水线名称，用于数据隔离，默认 "default"
class LocalStorageSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "local_storage"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析存储参数
    // 参数:
    //   config - 配置项，包含 backend、path、pipeline 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        backend_name_ = config["backend"].AsString("sqlite");
        data_dir_ = config["path"].AsString("/var/lib/illuminator");
        pipeline_name_ = config["pipeline"].AsString("default");
        return Status::Ok();
    }

    // 启动 Sink：通过 StorageFactory 创建存储后端实例并初始化
    // 返回:
    //   Status::Ok() 表示成功；kNotFound 表示不支持的存储后端
    Status Start() override {
        backend_ = StorageFactory::Instance().Create(backend_name_);
        if (!backend_) {
            return Status::Error(StatusCode::kNotFound,
                "Storage backend not found: " + backend_name_);
        }
        return backend_->Init(data_dir_, ConfigValue());
    }

    // 将 DataBatch 写入存储后端
    // 分别写入 Record 和 StackSample 数据。
    // 参数:
    //   batch - 待写入的数据批次
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!backend_ || !batch) return Status::Ok();

        // 写入 Record 数据
        if (!batch->records().empty()) {
            auto status = backend_->WriteRecords(pipeline_name_, batch->records());
            if (!status.ok()) return status;
        }

        // 写入 StackSample 数据
        if (!batch->stack_samples().empty()) {
            auto status = backend_->WriteStackSamples(pipeline_name_, batch->stack_samples());
            if (!status.ok()) return status;
        }

        return Status::Ok();
    }

    // 刷新存储后端的缓冲区，强制持久化
    // 返回:
    //   Status::Ok() 表示成功
    Status Flush() override {
        return backend_ ? backend_->Flush() : Status::Ok();
    }

    // 停止 Sink：先 Flush 再关闭存储后端
    // 返回:
    //   Status::Ok() 表示成功
    Status Stop() override {
        if (backend_) {
            backend_->Flush();
            backend_->Close();
        }
        return Status::Ok();
    }

private:
    std::string backend_name_;                           // 存储后端类型名
    std::string data_dir_;                               // 数据目录
    std::string pipeline_name_;                          // 流水线名称（用于命名空间隔离）
    std::unique_ptr<StorageBackend> backend_;            // 存储后端实例
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("local_storage", LocalStorageSink);

}  // namespace illuminator
