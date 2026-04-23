#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#include <memory>
#include <chrono>

#include "core/memory/arena.h"

namespace illuminator {

using Timestamp = std::chrono::time_point<std::chrono::system_clock,
                                          std::chrono::nanoseconds>;

inline Timestamp NowTimestamp() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
}

inline uint64_t TimestampToNanos(Timestamp ts) {
    return static_cast<uint64_t>(ts.time_since_epoch().count());
}

// A single label (key-value pair), using string_view into arena memory
struct Label {
    std::string_view key;
    std::string_view value;
};

// Flexible value type for records
using FieldValue = std::variant<std::monostate, bool, int64_t, uint64_t,
                                double, std::string_view>;

// A single data record in the pipeline
struct Record {
    Timestamp timestamp;
    std::vector<Label> labels;
    std::unordered_map<std::string_view, FieldValue> fields;

    void SetField(std::string_view key, FieldValue val) {
        fields[key] = std::move(val);
    }

    const FieldValue* GetField(std::string_view key) const {
        auto it = fields.find(key);
        return it != fields.end() ? &it->second : nullptr;
    }
};

// Stack frame for profiling data
struct StackFrame {
    uint64_t address = 0;
    std::string_view function_name;
    std::string_view file_name;
    uint32_t line_number = 0;
    std::string_view module_name;
};

// Stack sample for profiling
struct StackSample {
    Timestamp timestamp;
    uint32_t pid = 0;
    uint32_t tid = 0;
    std::string_view comm;  // Process name
    std::vector<StackFrame> kernel_stack;
    std::vector<StackFrame> user_stack;
    uint64_t count = 1;
};

// Batch of data records flowing through the pipeline.
// All string data is owned by the arena for zero-copy semantics.
class DataBatch {
public:
    enum class Type {
        kMetrics,
        kProfile,
        kTrace,
        kLog,
        kGeneric,
    };

    explicit DataBatch(Type type = Type::kGeneric)
        : type_(type), arena_(std::make_shared<Arena>()) {}

    DataBatch(Type type, std::shared_ptr<Arena> arena)
        : type_(type), arena_(std::move(arena)) {}

    Type type() const { return type_; }
    Arena& arena() { return *arena_; }
    const Arena& arena() const { return *arena_; }

    // Record-based access
    std::vector<Record>& records() { return records_; }
    const std::vector<Record>& records() const { return records_; }

    Record& AddRecord() {
        records_.emplace_back();
        records_.back().timestamp = NowTimestamp();
        return records_.back();
    }

    // Stack sample access (for profiling pipelines)
    std::vector<StackSample>& stack_samples() { return stack_samples_; }
    const std::vector<StackSample>& stack_samples() const { return stack_samples_; }

    StackSample& AddStackSample() {
        stack_samples_.emplace_back();
        stack_samples_.back().timestamp = NowTimestamp();
        return stack_samples_.back();
    }

    // Copy a string into the arena
    std::string_view InternString(std::string_view s) {
        return arena_->CopyString(s);
    }

    size_t Size() const { return records_.size() + stack_samples_.size(); }
    bool Empty() const { return records_.empty() && stack_samples_.empty(); }

    void Clear() {
        records_.clear();
        stack_samples_.clear();
        arena_->Reset();
    }

    // Metadata for the batch
    void SetMeta(const std::string& key, const std::string& value) {
        metadata_[key] = value;
    }

    std::string GetMeta(const std::string& key, const std::string& def = "") const {
        auto it = metadata_.find(key);
        return it != metadata_.end() ? it->second : def;
    }

private:
    Type type_;
    std::shared_ptr<Arena> arena_;
    std::vector<Record> records_;
    std::vector<StackSample> stack_samples_;
    std::unordered_map<std::string, std::string> metadata_;
};

using DataBatchPtr = std::shared_ptr<DataBatch>;

}  // namespace illuminator
