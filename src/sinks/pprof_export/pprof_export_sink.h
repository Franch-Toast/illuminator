#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// Exports profiling data in pprof-compatible folded format.
// Full protobuf pprof support requires protobuf dependency (Phase 5).
// This version outputs folded stacks compatible with FlameGraph tools.
class PprofExportSink : public SinkPlugin {
public:
    const char* Name() const override { return "pprof_export"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        path_ = config["path"].AsString("/tmp/illuminator.folded");
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        for (auto& sample : batch->stack_samples()) {
            std::string key = BuildFoldedStack(sample);
            if (!key.empty()) {
                folded_counts_[key] += sample.count;
            }
        }
        return Status::Ok();
    }

    Status Flush() override {
        if (folded_counts_.empty()) return Status::Ok();

        std::ofstream out(path_, std::ios::trunc);
        if (!out.is_open()) {
            return Status::Error(StatusCode::kInternal,
                "Cannot open pprof output: " + path_);
        }

        for (auto& [stack, count] : folded_counts_) {
            out << stack << " " << count << "\n";
        }

        out.close();
        IL_INFO("pprof export: %zu unique stacks written to %s",
                folded_counts_.size(), path_.c_str());
        folded_counts_.clear();
        return Status::Ok();
    }

    Status Stop() override {
        return Flush();
    }

private:
    static std::string BuildFoldedStack(const StackSample& sample) {
        std::string result;
        result.reserve(512);

        if (!sample.comm.empty()) {
            result.append(sample.comm.data(), sample.comm.size());
        }

        // Build folded format: comm;frame1;frame2;...;frameN
        for (auto it = sample.user_stack.rbegin();
             it != sample.user_stack.rend(); ++it) {
            result += ";";
            if (!it->function_name.empty()) {
                result.append(it->function_name.data(), it->function_name.size());
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%lx", it->address);
                result += buf;
            }
        }

        return result;
    }

    std::string path_;
    std::unordered_map<std::string, uint64_t> folded_counts_;
};

IL_REGISTER_SINK("pprof_export", PprofExportSink);

}  // namespace illuminator
