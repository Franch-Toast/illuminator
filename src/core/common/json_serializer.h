// ============================================================================
// Illuminator JSON 序列化 — 基于 nlohmann/json
// ============================================================================
//
// 提供 DataBatch、StackSample、SchedAnalyzer 数据类型到 JSON 的序列化。
// 替代原 main.cc 中手写 ostringstream 拼接方式。
// ============================================================================

#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/common/data_batch.h"

namespace illuminator {

using json = nlohmann::json;

inline json FieldValueToJson(const FieldValue& fv) {
    if (auto* b = std::get_if<bool>(&fv)) return *b;
    if (auto* i = std::get_if<int64_t>(&fv)) return *i;
    if (auto* u = std::get_if<uint64_t>(&fv)) return *u;
    if (auto* d = std::get_if<double>(&fv)) return *d;
    if (auto* sv = std::get_if<std::string_view>(&fv))
        return std::string(sv->data(), sv->size());
    return nullptr;
}

inline json StackFrameToJson(const StackFrame& f) {
    json j;
    j["address"] = f.address;
    if (!f.function_name.empty())
        j["function_name"] = std::string(f.function_name);
    if (!f.module_name.empty())
        j["module_name"] = std::string(f.module_name);
    return j;
}

inline json StackFramesToJson(const std::vector<StackFrame>& frames) {
    json arr = json::array();
    for (auto& f : frames)
        arr.push_back(StackFrameToJson(f));
    return arr;
}

inline json StackSampleToJson(const StackSample& s) {
    json j;
    j["timestamp"] = TimestampToNanos(s.timestamp) / 1000000;
    j["pid"] = s.pid;
    j["tid"] = s.tid;
    j["cpu"] = s.cpu;
    j["count"] = s.count;
    j["comm"] = std::string(s.comm);
    j["type"] = static_cast<int>(s.sample_type);
    if (s.duration_ns > 0)
        j["duration_ns"] = s.duration_ns;
    j["kernel_stack"] = StackFramesToJson(s.kernel_stack);
    j["user_stack"] = StackFramesToJson(s.user_stack);
    return j;
}

inline json RecordToJson(const Record& rec) {
    json j;
    j["timestamp"] = TimestampToNanos(rec.timestamp) / 1000000;

    json labels = json::object();
    for (auto& l : rec.labels)
        labels[std::string(l.key)] = std::string(l.value);
    j["labels"] = std::move(labels);

    json fields = json::object();
    for (auto& [k, v] : rec.fields)
        fields[std::string(k)] = FieldValueToJson(v);
    j["fields"] = std::move(fields);
    return j;
}

inline json RecordsToJsonArray(const DataBatch& batch) {
    json arr = json::array();
    for (auto& rec : batch.records())
        arr.push_back(RecordToJson(rec));
    return arr;
}

inline json StackSamplesToJsonArray(const DataBatch& batch) {
    json arr = json::array();
    for (auto& s : batch.stack_samples())
        arr.push_back(StackSampleToJson(s));
    return arr;
}

inline std::string BatchToJson(const DataBatch& batch, const std::string& pipeline) {
    json j;
    j["pipeline"] = pipeline;
    j["records"] = RecordsToJsonArray(batch);

    if (!batch.stack_samples().empty())
        j["stack_samples"] = StackSamplesToJsonArray(batch);

    return j.dump();
}

}  // namespace illuminator
