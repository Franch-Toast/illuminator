#pragma once

#include <memory>
#include <string>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"
#include "storage/storage_backend.h"

namespace illuminator {

// Sink that writes pipeline data to a local storage backend
class LocalStorageSink : public SinkPlugin {
public:
    const char* Name() const override { return "local_storage"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        backend_name_ = config["backend"].AsString("sqlite");
        data_dir_ = config["path"].AsString("/var/lib/illuminator");
        pipeline_name_ = config["pipeline"].AsString("default");
        return Status::Ok();
    }

    Status Start() override {
        backend_ = StorageFactory::Instance().Create(backend_name_);
        if (!backend_) {
            return Status::Error(StatusCode::kNotFound,
                "Storage backend not found: " + backend_name_);
        }
        return backend_->Init(data_dir_, ConfigValue());
    }

    Status Write(DataBatchPtr batch) override {
        if (!backend_ || !batch) return Status::Ok();

        if (!batch->records().empty()) {
            auto status = backend_->WriteRecords(pipeline_name_, batch->records());
            if (!status.ok()) return status;
        }

        if (!batch->stack_samples().empty()) {
            auto status = backend_->WriteStackSamples(pipeline_name_, batch->stack_samples());
            if (!status.ok()) return status;
        }

        return Status::Ok();
    }

    Status Flush() override {
        return backend_ ? backend_->Flush() : Status::Ok();
    }

    Status Stop() override {
        if (backend_) {
            backend_->Flush();
            backend_->Close();
        }
        return Status::Ok();
    }

private:
    std::string backend_name_;
    std::string data_dir_;
    std::string pipeline_name_;
    std::unique_ptr<StorageBackend> backend_;
};

IL_REGISTER_SINK("local_storage", LocalStorageSink);

}  // namespace illuminator
