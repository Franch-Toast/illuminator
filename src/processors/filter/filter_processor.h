#pragma once

#include <string>
#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// Filter processor: drops records based on label matching rules.
class FilterProcessor : public ProcessorPlugin {
public:
    const char* Name() const override { return "filter"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        auto key_sv = config["exclude_label_key"].AsString();
        auto val_sv = config["exclude_label_value"].AsString();
        exclude_key_ = std::string(key_sv);
        exclude_value_ = std::string(val_sv);
        return Status::Ok();
    }

    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        if (exclude_key_.empty()) return input;

        auto output = std::make_shared<DataBatch>(input->type());

        for (auto& rec : input->records()) {
            bool excluded = false;
            for (auto& label : rec.labels) {
                if (label.key == exclude_key_ && label.value == exclude_value_) {
                    excluded = true;
                    break;
                }
            }
            if (!excluded) {
                output->records().push_back(rec);
            }
        }

        for (auto& sample : input->stack_samples()) {
            output->stack_samples().push_back(sample);
        }

        return output;
    }

private:
    std::string exclude_key_;
    std::string exclude_value_;
};

IL_REGISTER_PROCESSOR("filter", FilterProcessor);

}  // namespace illuminator
