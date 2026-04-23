#pragma once

#include <vector>
#include "plugin/api/plugin_api.h"

namespace illuminator {

// Aggregator plugins buffer and aggregate data before flushing downstream.
// Examples: stack merging, histogram bucketing, windowed counters.
class AggregatorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kAggregator; }

    // Add a batch to the aggregation buffer
    virtual Status Add(DataBatchPtr batch) = 0;

    // Flush aggregated data. Called periodically or on shutdown.
    // Returns zero or more output batches.
    virtual StatusOr<std::vector<DataBatchPtr>> Flush() = 0;

    // Flush interval in milliseconds. 0 means flush every batch.
    virtual uint32_t FlushIntervalMs() const { return 10000; }
};

}  // namespace illuminator
