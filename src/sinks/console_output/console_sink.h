#pragma once

#include <cstdio>
#include <string>
#include <variant>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class ConsoleSink : public SinkPlugin {
public:
    const char* Name() const override { return "console_output"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        auto fmt = config["format"].AsString("text");
        json_mode_ = (fmt == "json");
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        for (auto& rec : batch->records()) {
            if (json_mode_) {
                WriteRecordJson(rec);
            } else {
                WriteRecordText(rec);
            }
        }

        for (auto& sample : batch->stack_samples()) {
            WriteStackSample(sample);
        }

        return Status::Ok();
    }

private:
    struct FieldPrinter {
        FILE* out;
        std::string_view key;

        void operator()(std::monostate) const {}
        void operator()(bool v) const {
            fprintf(out, " %.*s=%s", static_cast<int>(key.size()), key.data(),
                    v ? "true" : "false");
        }
        void operator()(int64_t v) const {
            fprintf(out, " %.*s=%ld", static_cast<int>(key.size()), key.data(), v);
        }
        void operator()(uint64_t v) const {
            fprintf(out, " %.*s=%lu", static_cast<int>(key.size()), key.data(), v);
        }
        void operator()(double v) const {
            fprintf(out, " %.*s=%.3f", static_cast<int>(key.size()), key.data(), v);
        }
        void operator()(std::string_view v) const {
            fprintf(out, " %.*s=%.*s", static_cast<int>(key.size()), key.data(),
                    static_cast<int>(v.size()), v.data());
        }
    };

    void WriteRecordText(const Record& rec) {
        auto ns = TimestampToNanos(rec.timestamp);
        fprintf(stdout, "[%lu]", ns);

        for (auto& label : rec.labels) {
            fprintf(stdout, " {%.*s=%.*s}",
                    static_cast<int>(label.key.size()), label.key.data(),
                    static_cast<int>(label.value.size()), label.value.data());
        }

        for (auto& [key, val] : rec.fields) {
            std::visit(FieldPrinter{stdout, key}, val);
        }

        fprintf(stdout, "\n");
    }

    void WriteRecordJson(const Record& rec) {
        auto ns = TimestampToNanos(rec.timestamp);
        fprintf(stdout, "{\"ts\":%lu,\"labels\":{", ns);

        bool first = true;
        for (auto& label : rec.labels) {
            if (!first) fprintf(stdout, ",");
            fprintf(stdout, "\"%.*s\":\"%.*s\"",
                    static_cast<int>(label.key.size()), label.key.data(),
                    static_cast<int>(label.value.size()), label.value.data());
            first = false;
        }

        fprintf(stdout, "},\"fields\":{");
        first = true;
        for (auto& [key, val] : rec.fields) {
            if (!first) fprintf(stdout, ",");
            fprintf(stdout, "\"%.*s\":", static_cast<int>(key.size()), key.data());
            PrintJsonValue(val);
            first = false;
        }

        fprintf(stdout, "}}\n");
    }

    void PrintJsonValue(const FieldValue& val) {
        struct Visitor {
            void operator()(std::monostate) const { fprintf(stdout, "null"); }
            void operator()(bool v) const { fprintf(stdout, "%s", v ? "true" : "false"); }
            void operator()(int64_t v) const { fprintf(stdout, "%ld", v); }
            void operator()(uint64_t v) const { fprintf(stdout, "%lu", v); }
            void operator()(double v) const { fprintf(stdout, "%.6f", v); }
            void operator()(std::string_view v) const {
                fprintf(stdout, "\"%.*s\"", static_cast<int>(v.size()), v.data());
            }
        };
        std::visit(Visitor{}, val);
    }

    void WriteStackSample(const StackSample& sample) {
        fprintf(stdout, "[stack] pid=%u tid=%u comm=%.*s count=%lu\n",
                sample.pid, sample.tid,
                static_cast<int>(sample.comm.size()), sample.comm.data(),
                sample.count);
        for (auto& frame : sample.user_stack) {
            fprintf(stdout, "  %.*s+0x%lx",
                    static_cast<int>(frame.function_name.size()),
                    frame.function_name.data(), frame.address);
            if (!frame.file_name.empty()) {
                fprintf(stdout, " (%.*s:%u)",
                        static_cast<int>(frame.file_name.size()),
                        frame.file_name.data(), frame.line_number);
            }
            fprintf(stdout, "\n");
        }
    }

    bool json_mode_ = false;
};

IL_REGISTER_SINK("console_output", ConsoleSink);

}  // namespace illuminator
