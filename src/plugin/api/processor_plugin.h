#pragma once

#include "plugin/api/plugin_api.h"

namespace illuminator {

// Processor plugins transform data in-flight.
// They receive a batch, may modify it, produce a new batch, or drop records.
class ProcessorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kProcessor; }

    // Process a batch. May modify in-place or produce a new output batch.
    // Return nullptr to drop the batch entirely.
    virtual StatusOr<DataBatchPtr> Process(DataBatchPtr input) = 0;
};

}  // namespace illuminator
