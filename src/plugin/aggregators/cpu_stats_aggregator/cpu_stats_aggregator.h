// Available via PluginRegistry but not yet wired into any FeatureDriver.
// =============================================================================
// 文件：cpu_stats_aggregator.h
// 模块：Illuminator 聚合器 - CPU 统计聚合器
// 描述：
//   在时间窗口内累积指标记录（Record）和堆栈采样（StackSample），
//   定期 Flush 输出聚合后的统计结果：
//     - 对 Record: 计算每个字段的 avg/min/max/p50/p99 和采样数
//     - 对 StackSample: 按地址累加 count 并去重
//   线程安全，使用互斥锁保护内部缓冲区。
// =============================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/api/aggregator_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

// CpuStatsAggregator: CPU 统计聚合器
// 实现 AggregatorPlugin 接口，在内存缓冲区中累积数据，
// 按配置的时间窗口周期性地输出聚合结果。
// 配置参数:
//   window_sec - 聚合窗口大小（秒），默认 10
// 聚合输出:
//   对 Record 字段值的数组计算 avg/min/max 统计量；
//   当样本数 >= 4 时额外输出 p50/p99 百分位值。
class CpuStatsAggregator : public AggregatorPlugin {
public:
    // 返回聚合器名称
    const char* Name() const override { return "cpu_stats_aggregator"; }

    // 返回聚合器版本号
    const char* Version() const override { return "0.2.0"; }

    // 从配置中解析聚合窗口大小
    // 参数:
    //   config - 配置项，包含 window_sec
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        window_ms_ = static_cast<uint32_t>(config["window_sec"].AsInt(10) * 1000);
        if (window_ms_ == 0) window_ms_ = 10000;
        return Status::Ok();
    }

    // 返回 Flush 间隔，单位毫秒
    // Pipeline 框架据此周期性地调用 Flush() 输出聚合结果
    uint32_t FlushIntervalMs() const override { return window_ms_; }

    // 将 DataBatch 中的数据累积到内部缓冲区
    // 线程安全，内部加锁。
    // 参数:
    //   batch - 待聚合的 DataBatch
    // 返回:
    //   Status::Ok() 表示操作成功
    Status Add(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lock(mutex_);

        // 处理 Record 类型数据
        for (auto& rec : batch->records()) {
            std::string key = MakeRecordKey(rec);
            auto& series = record_buffer_[key];
            series.labels = CopyLabels(rec);
            // 将字段值统一转为 double 存入时间序列
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

        // 处理 StackSample 类型数据
        for (auto& sample : batch->stack_samples()) {
            std::string key = MakeStackKey(sample);
            auto& agg = stack_buffer_[key];
            agg.pid = sample.pid;
            agg.tid = sample.tid;
            agg.comm = std::string(sample.comm);
            agg.count += sample.count;
            // 仅首次出现时复制栈帧内容，后续仅累加 count
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

    // 输出当前聚合窗口内的统计结果并清空缓冲区
    // 线程安全，内部加锁。
    // 产生两个独立的 DataBatch:
    //   1. Type::kMetrics - Record 聚合统计
    //   2. Type::kProfile - StackSample 聚合结果
    // 返回:
    //   StatusOr<vector<DataBatchPtr>> - 聚合结果列表
    StatusOr<std::vector<DataBatchPtr>> Flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DataBatchPtr> result;

        // 输出 Record 聚合统计
        if (!record_buffer_.empty()) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
            for (auto& [key, series] : record_buffer_) {
                auto& rec = batch->AddRecord();
                // 复制标签
                for (auto& [lk, lv] : series.labels) {
                    rec.labels.push_back({batch->InternString(lk),
                                          batch->InternString(lv)});
                }
                // 对每个字段的值序列计算统计量
                for (auto& [fname, values] : series.field_values) {
                    if (values.empty()) continue;
                    auto stats = ComputeStats(values);
                    rec.SetField(batch->InternString(fname + "_avg"), stats.avg);
                    rec.SetField(batch->InternString(fname + "_min"), stats.min);
                    rec.SetField(batch->InternString(fname + "_max"), stats.max);
                    // 样本量足够时才计算百分位（小样本百分位无统计意义）
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

        // 输出 StackSample 聚合结果
        if (!stack_buffer_.empty()) {
            auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
            for (auto& [key, agg] : stack_buffer_) {
                auto& sample = batch->AddStackSample();
                sample.pid = agg.pid;
                sample.tid = agg.tid;
                sample.comm = batch->InternString(agg.comm);
                sample.count = agg.count;
                // 将 owned 格式栈帧写回 StackFrame
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
    // Stats: 单字段的统计指标
    struct Stats {
        double avg = 0, min = 0, max = 0, p50 = 0, p99 = 0;
    };

    // 对数值数组计算统计指标
    // 会将输入数组排序以计算 min/max 和百分位
    // 参数:
    //   vals - 输入数值数组，会被就地排序
    // 返回:
    //   包含 avg/min/max/p50/p99 的 Stats 结构
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

    // 计算数组的百分位值（线性插值）
    // 参数:
    //   sorted - 已排序的数值数组
    //   pct    - 百分位 (0.0 ~ 1.0)
    // 返回:
    //   对应百分位的值
    static double Percentile(const std::vector<double>& sorted, double pct) {
        if (sorted.empty()) return 0;
        double idx = pct * static_cast<double>(sorted.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(idx));
        size_t hi = static_cast<size_t>(std::ceil(idx));
        if (lo == hi || hi >= sorted.size()) return sorted[lo];
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    // 根据标签构造 Record 的唯一键
    // 格式: "key1=value1;key2=value2;..."
    // 参数:
    //   rec - 数据记录
    // 返回:
    //   标签拼接的唯一字符串键
    static std::string MakeRecordKey(const Record& rec) {
        std::string key;
        for (auto& l : rec.labels) {
            key += std::string(l.key) + "=" + std::string(l.value) + ";";
        }
        return key;
    }

    // 根据 PID/TID 和栈帧地址构造 StackSample 的唯一键
    // 格式: "pid:tid:addr1,addr2,...|addr1,addr2,..."
    // 参数:
    //   s - 堆栈采样
    // 返回:
    //   唯一的字符串键
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

    // 复制 Record 的标签到独立拥有的副本中（用于跨 batch 缓存）
    // 参数:
    //   rec - 源数据记录
    // 返回:
    //   (key, value) 对构成的 vector
    static std::vector<std::pair<std::string, std::string>> CopyLabels(const Record& rec) {
        std::vector<std::pair<std::string, std::string>> result;
        for (auto& l : rec.labels) {
            result.emplace_back(std::string(l.key), std::string(l.value));
        }
        return result;
    }

    // RecordSeries: 标签相同的记录字段值时间序列
    struct RecordSeries {
        std::vector<std::pair<std::string, std::string>> labels;          // 标签副本
        std::unordered_map<std::string, std::vector<double>> field_values; // 字段名 -> 值序列
    };

    // OwnedStackFrame: 独立拥有内存的栈帧表示
    // 与 StackFrame 使用 intern 字符串不同，此处使用 std::string 持有完整副本
    struct OwnedStackFrame {
        uint64_t address = 0;
        std::string function_name;
        std::string file_name;
        uint32_t line_number = 0;
        std::string module_name;
    };

    // AggStackSample: 聚合后的堆栈采样（独立拥有所有字符串内存）
    struct AggStackSample {
        uint32_t pid = 0;
        uint32_t tid = 0;
        std::string comm;
        uint64_t count = 0;
        std::vector<OwnedStackFrame> user_stack;
        std::vector<OwnedStackFrame> kernel_stack;
    };

    uint32_t window_ms_ = 10000;                                      // 聚合窗口（毫秒），默认 10 秒
    std::mutex mutex_;                                                // 保护内部缓冲区的互斥锁
    std::unordered_map<std::string, RecordSeries> record_buffer_;     // Record 累积缓冲区（按标签键索引）
    std::unordered_map<std::string, AggStackSample> stack_buffer_;    // StackSample 累积缓冲区（按 PID/TID/地址索引）
};

// 在插件注册表中注册该聚合器
IL_REGISTER_AGGREGATOR("cpu_stats_aggregator", CpuStatsAggregator);

}  // namespace illuminator
