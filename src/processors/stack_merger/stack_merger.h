#pragma once

#include <sstream>
#include <string>
#include <unordered_map>

#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

enum class StackMergeGroupBy {
    kComm,
    kPid,
    kTid,
    kGlobal,
};

class StackMergerProcessor : public ProcessorPlugin {
public:
    const char* Name() const override { return "stack_merger"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        auto gb = config["group_by"].AsString("comm");
        if (gb == "pid")
            group_by_ = StackMergeGroupBy::kPid;
        else if (gb == "tid")
            group_by_ = StackMergeGroupBy::kTid;
        else if (gb == "global")
            group_by_ = StackMergeGroupBy::kGlobal;
        else
            group_by_ = StackMergeGroupBy::kComm;

        include_kernel_ = config["include_kernel"].AsBool(true);
        return Status::Ok();
    }

    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        if (!input || input->stack_samples().empty())
            return input;

        std::unordered_map<std::string, StackSample> merged;
        merged.reserve(input->stack_samples().size());

        for (const auto& src : input->stack_samples()) {
            std::string key = StackKey(src);
            auto it = merged.find(key);
            if (it == merged.end())
                merged.emplace(std::move(key), src);
            else
                it->second.count += src.count;
        }

        auto out = std::make_shared<DataBatch>(input->type());
        for (auto& [_, sample] : merged) {
            auto& dst = out->AddStackSample();
            dst.timestamp = sample.timestamp;
            dst.pid = sample.pid;
            dst.tid = sample.tid;
            dst.comm = out->InternString(sample.comm);
            dst.count = sample.count;
            dst.cpu = sample.cpu;
            dst.sample_type = sample.sample_type;
            dst.duration_ns = sample.duration_ns;
            dst.kernel_stack_id = sample.kernel_stack_id;
            dst.user_stack_id = sample.user_stack_id;

            dst.kernel_stack.reserve(sample.kernel_stack.size());
            for (const auto& fr : sample.kernel_stack) {
                StackFrame nf;
                nf.address = fr.address;
                nf.function_name = out->InternString(fr.function_name);
                nf.file_name = out->InternString(fr.file_name);
                nf.line_number = fr.line_number;
                nf.module_name = out->InternString(fr.module_name);
                dst.kernel_stack.push_back(nf);
            }

            dst.user_stack.reserve(sample.user_stack.size());
            for (const auto& fr : sample.user_stack) {
                StackFrame nf;
                nf.address = fr.address;
                nf.function_name = out->InternString(fr.function_name);
                nf.file_name = out->InternString(fr.file_name);
                nf.line_number = fr.line_number;
                nf.module_name = out->InternString(fr.module_name);
                dst.user_stack.push_back(nf);
            }
        }

        for (auto& rec : input->records())
            out->records().push_back(rec);

        return out;
    }

private:
    std::string StackKey(const StackSample& s) const {
        std::ostringstream os;
        switch (group_by_) {
            case StackMergeGroupBy::kComm:
                os << std::string(s.comm) << "|";
                break;
            case StackMergeGroupBy::kPid:
                os << s.pid << "|";
                break;
            case StackMergeGroupBy::kTid:
                os << s.pid << ":" << s.tid << "|";
                break;
            case StackMergeGroupBy::kGlobal:
                break;
        }

        if (include_kernel_) {
            for (const auto& fr : s.kernel_stack) {
                if (!fr.function_name.empty())
                    os << std::string(fr.function_name) << ";";
                else
                    os << "k:" << fr.address << ";";
            }
            os << "|";
        }
        for (const auto& fr : s.user_stack) {
            if (!fr.function_name.empty())
                os << std::string(fr.function_name) << ";";
            else
                os << "u:" << fr.address << ";";
        }
        return os.str();
    }

    StackMergeGroupBy group_by_ = StackMergeGroupBy::kComm;
    bool include_kernel_ = true;
};

IL_REGISTER_PROCESSOR("stack_merger", StackMergerProcessor);

}  // namespace illuminator
