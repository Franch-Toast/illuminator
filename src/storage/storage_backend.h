#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/common/status.h"
#include "core/engine/data_batch.h"

namespace illuminator {

// Metadata for a stored profile
struct ProfileMeta {
    std::string pipeline_name;
    std::string profile_type;   // "cpu", "memory", "io", "sched", "net"
    uint64_t start_time_ns = 0;
    uint64_t end_time_ns = 0;
    uint64_t sample_count = 0;
    std::unordered_map<std::string, std::string> labels;
};

// Time range query
struct TimeRange {
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
};

// Query request for stored data
struct QueryRequest {
    std::string pipeline_name;
    std::string metric_name;
    TimeRange time_range;
    uint32_t limit = 1000;
    std::string order_by;
    bool descending = true;
};

struct QueryResult {
    std::vector<Record> records;
    std::vector<StackSample> stack_samples;
    uint64_t total_count = 0;
    bool has_more = false;
};

// Abstract storage backend interface.
// Implementations provide the actual persistence mechanism.
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual const char* Name() const = 0;

    // Initialize the backend with data directory
    virtual Status Init(const std::string& data_dir,
                        const ConfigValue& config) = 0;

    // Write time-series records
    virtual Status WriteRecords(const std::string& pipeline_name,
                                const std::vector<Record>& records) = 0;

    // Write profiling stack samples
    virtual Status WriteStackSamples(const std::string& pipeline_name,
                                     const std::vector<StackSample>& samples) = 0;

    // Write raw profile data (e.g., pprof bytes)
    virtual Status WriteProfile(const ProfileMeta& meta,
                                const std::vector<uint8_t>& data) = 0;

    // Query stored data
    virtual StatusOr<QueryResult> Query(const QueryRequest& req) = 0;

    // List available profiles
    virtual StatusOr<std::vector<ProfileMeta>> ListProfiles(
        const std::string& pipeline_name,
        const TimeRange& range) = 0;

    // Lifecycle
    virtual Status Flush() = 0;
    virtual Status Compact() = 0;
    virtual Status Close() = 0;

    // Storage stats
    virtual uint64_t DiskUsageBytes() const { return 0; }
};

// Factory for creating storage backends by name
class StorageFactory {
public:
    static StorageFactory& Instance() {
        static StorageFactory inst;
        return inst;
    }

    using Creator = std::function<std::unique_ptr<StorageBackend>()>;

    void Register(const std::string& name, Creator creator) {
        creators_[name] = std::move(creator);
    }

    std::unique_ptr<StorageBackend> Create(const std::string& name) {
        auto it = creators_.find(name);
        if (it == creators_.end()) return nullptr;
        return it->second();
    }

    std::vector<std::string> Available() const {
        std::vector<std::string> names;
        for (auto& [k, _] : creators_) names.push_back(k);
        return names;
    }

private:
    std::unordered_map<std::string, Creator> creators_;
};

}  // namespace illuminator
