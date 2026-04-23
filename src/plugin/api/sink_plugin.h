#pragma once

#include "plugin/api/plugin_api.h"

namespace illuminator {

// Sink plugins consume processed data.
// Examples: storage backends, file exporters, console output, remote endpoints.
class SinkPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSink; }

    // Write a batch to the sink
    virtual Status Write(DataBatchPtr batch) = 0;

    // Flush buffered data
    virtual Status Flush() { return Status::Ok(); }
};

}  // namespace illuminator
