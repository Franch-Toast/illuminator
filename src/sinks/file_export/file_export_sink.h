#pragma once

#include <cstdio>
#include <string>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// Exports data batches to a JSON-lines file
class FileExportSink : public SinkPlugin {
public:
    const char* Name() const override { return "file_export"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        auto path = config["path"].AsString("/tmp/illuminator_output.jsonl");
        path_ = std::string(path);
        return Status::Ok();
    }

    Status Start() override {
        file_ = fopen(path_.c_str(), "a");
        if (!file_) {
            return Status::Error(StatusCode::kInternal,
                "Cannot open file: " + path_);
        }
        return Status::Ok();
    }

    Status Stop() override {
        if (file_) {
            fclose(file_);
            file_ = nullptr;
        }
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!file_ || !batch) return Status::Ok();

        for (auto& rec : batch->records()) {
            auto ns = TimestampToNanos(rec.timestamp);
            fprintf(file_, "{\"ts\":%lu,\"labels\":{", ns);

            bool first = true;
            for (auto& label : rec.labels) {
                if (!first) fprintf(file_, ",");
                fprintf(file_, "\"%.*s\":\"%.*s\"",
                        static_cast<int>(label.key.size()), label.key.data(),
                        static_cast<int>(label.value.size()), label.value.data());
                first = false;
            }

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

    Status Flush() override {
        if (file_) fflush(file_);
        return Status::Ok();
    }

private:
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

    std::string path_;
    FILE* file_ = nullptr;
};

IL_REGISTER_SINK("file_export", FileExportSink);

}  // namespace illuminator
