#pragma once

#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// A no-op processor that passes data through unchanged.
// Useful for testing pipelines.
class PassthroughProcessor : public ProcessorPlugin {
public:
    const char* Name() const override { return "passthrough"; }
    const char* Version() const override { return "0.1.0"; }

    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        return input;
    }
};

IL_REGISTER_PROCESSOR("passthrough", PassthroughProcessor);

}  // namespace illuminator
