#pragma once

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/api/aggregator_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class CpuStatsAggregator : public AggregatorPlugin {
public:
    const char* Name() const override { return "cpu_stats_aggregator"; }
    const char* Version() const override { return "0.2.0"; }

    Status Init(const ConfigValue& config) override {
        window_ms_ = static_cast<uint32_t>(config["window_sec"].AsInt(10) * 1000);
        if (window_ms_ == 0) window_ms_ = 10000;
        return Status::Ok();
    }

    uint32_t FlushIntervalMs() const override { return window_ms_; }

    Status Add(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& rec : batch->records()) {
            std::string key = MakeRecordKey(rec);
            auto& series = record_buffer_[key];
            series.labels = CopyLabels(rec);
            for (auto& [fname, fval] : rec.fields) {
                if (auto* dv = std::get_if<double>(&fval)) {
                    series.field_values[std::string(fname)].push_back(*dv);
                } else if (auto* uv = std::get_if<uint64_t>(&fval)) {
                    series.field_values[std::string(fname)].push_back(
                        static_cast<double>(*uv));
                } else if (auto* iv = std::get_if<int64_t>(&fval)) {
                    series.field_values[std::string(fname)].push_back(
                        static_cast<double>(*iv));
                }
            }
        }

        for (auto& sample : batch->stack_samples()) {
            std::string key = MakeStackKey(sample);
            auto& agg = stack_buffer_[key];
            agg.pid = sample.pid;
            agg.tid = sample.tid;
            agg.comm = std::string(sample.comm);
            agg.count += sample.count;
            if (agg.user_stack.empty() && !sample.user_stack.empty()) {
                for (auto& f : sample.user_stack)
                    agg.user_stack.push_back({f.address, std::string(f.function_name),
                                              std::string(f.file_name), f.line_number,
                                              std::string(f.module_name)});
            }
            if (agg.kernel_stack.empty() && !sample.kernel_stack.empty()) {
                for (auto& f : sample.kernel_stack)
                    agg.kernel_stack.push_back({f.address, std::string(f.function_name),
                                                std::string(f.file_name), f.line_number,
                                                std::string(f.module_name)});
            }
        }

        return Status::Ok();
    }

    StatusOr<std::vector<DataBatchPtr>> Flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DataBatchPtr> result;

        if (!record_buffer_.empty()) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
            for (auto& [key, series] : record_buffer_) {
                auto& rec = batch->AddRecord();
                for (auto& [lk, lv] : series.labels) {
                    rec.labels.push_back({batch->InternString(lk),
                                          batch->InternString(lv)});
                }
                for (auto& [fname, values] : series.field_values) {
                    if (values.empty()) continue;
                    auto stats = ComputeStats(values);
                    rec.SetField(batch->InternString(fname + "_avg"), stats.avg);
                    rec.SetField(batch->InternString(fname + "_min"), stats.min);
                    rec.SetField(batch->InternString(fname + "_max"), stats.max);
                    if (values.size() >= 4) {
                        rec.SetField(batch->InternString(fname + "_p50"), stats.p50);
                        rec.SetField(batch->InternString(fname + "_p99"), stats.p99);
                    }
                    rec.SetField(batch->InternString(fname + "_count"),
                                 static_cast<uint64_t>(values.size()));
                }
            }
            result.push_back(std::move(batch));
            record_buffer_.clear();
        }

        if (!stack_buffer_.empty()) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
            for (auto& [key, agg] : stack_buffer_) {
                auto& sample = batch->AddStackSample();
                sample.pid = agg.pid;
                sample.tid = agg.tid;
                sample.comm = batch->InternString(agg.comm);
                sample.count = agg.count;
                for (auto& f : agg.user_stack) {
                    StackFrame sf;
                    sf.address = f.address;
                    sf.function_name = batch->InternString(f.function_name);
                    sf.file_name = batch->InternString(f.file_name);
                    sf.line_number = f.line_number;
                    sf.module_name = batch->InternString(f.module_name);
                    sample.user_stack.push_back(sf);
                }
                for (auto& f : agg.kernel_stack) {
                    StackFrame sf;
                    sf.address = f.address;
                    sf.function_name = batch->InternString(f.function_name);
                    sf.file_name = batch->InternString(f.file_name);
                    sf.line_number = f.line_number;
                    sf.module_name = batch->InternString(f.module_name);
                    sample.kernel_stack.push_back(sf);
                }
            }
            result.push_back(std::move(batch));
            stack_buffer_.clear();
        }

        return result;
    }

private:
    struct Stats {
        double avg = 0, min = 0, max = 0, p50 = 0, p99 = 0;
    };

    static Stats ComputeStats(std::vector<double>& vals) {
        Stats s;
        if (vals.empty()) return s;

        std::sort(vals.begin(), vals.end());
        s.min = vals.front();
        s.max = vals.back();

        double sum = 0;
        for (double v : vals) sum += v;
        s.avg = sum / static_cast<double>(vals.size());

        s.p50 = Percentile(vals, 0.50);
        s.p99 = Percentile(vals, 0.99);
        return s;
    }

    static double Percentile(const std::vector<double>& sorted, double pct) {
        if (sorted.empty()) return 0;
        double idx = pct * static_cast<double>(sorted.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(idx));
        size_t hi = static_cast<size_t>(std::ceil(idx));
        if (lo == hi || hi >= sorted.size()) return sorted[lo];
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    static std::string MakeRecordKey(const Record& rec) {
        std::string key;
        for (auto& l : rec.labels) {
            key += std::string(l.key) + "=" + std::string(l.value) + ";";
        }
        return key;
    }

    static std::string MakeStackKey(const StackSample& s) {
        std::string key = std::to_string(s.pid) + ":" + std::to_string(s.tid) + ":";
        for (auto& f : s.user_stack) {
            key += std::to_string(f.address) + ",";
        }
        key += "|";
        for (auto& f : s.kernel_stack) {
            key += std::to_string(f.address) + ",";
        }
        return key;
    }

    static std::vector<std::pair<std::string, std::string>> CopyLabels(const Record& rec) {
        std::vector<std::pair<std::string, std::string>> result;
        for (auto& l : rec.labels) {
            result.emplace_back(std::string(l.key), std::string(l.value));
        }
        return result;
    }

    struct RecordSeries {
        std::vector<std::pair<std::string, std::string>> labels;
        std::unordered_map<std::string, std::vector<double>> field_values;
    };

    struct OwnedStackFrame {
        uint64_t address = 0;
        std::string function_name;
        std::string file_name;
        uint32_t line_number = 0;
        std::string module_name;
    };

    struct AggStackSample {
        uint32_t pid = 0;
        uint32_t tid = 0;
        std::string comm;
        uint64_t count = 0;
        std::vector<OwnedStackFrame> user_stack;
        std::vector<OwnedStackFrame> kernel_stack;
    };

    uint32_t window_ms_ = 10000;
    std::mutex mutex_;
    std::unordered_map<std::string, RecordSeries> record_buffer_;
    std::unordered_map<std::string, AggStackSample> stack_buffer_;
};

IL_REGISTER_AGGREGATOR("cpu_stats_aggregator", CpuStatsAggregator);

}  // namespace illuminator
