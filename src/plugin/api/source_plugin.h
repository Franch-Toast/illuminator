#pragma once

#include <functional>
#include "plugin/api/plugin_api.h"

namespace illuminator {

// Callback to deliver data downstream
using SourceCallback = std::function<void(DataBatchPtr)>;

// Source plugins produce data by either:
// 1. Polling (Pull mode): Collect() is called periodically
// 2. Streaming (Push mode): Start() begins async delivery via callback
class SourcePlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSource; }

    // Set the callback for delivering collected data
    void SetCallback(SourceCallback cb) { callback_ = std::move(cb); }

    // Pull mode: called by the pipeline to collect a batch
    virtual StatusOr<DataBatchPtr> Collect() {
        return Status::Error(StatusCode::kUnimplemented, "Pull mode not implemented");
    }

    // Returns true if this source operates in push (streaming) mode.
    // If true, the pipeline will call Start() and data is delivered via callback.
    // If false, the pipeline will call Collect() periodically.
    virtual bool IsPushMode() const { return false; }

    // Collection interval for pull mode (milliseconds)
    virtual uint32_t IntervalMs() const { return 1000; }

protected:
    SourceCallback callback_;
};

}  // namespace illuminator
