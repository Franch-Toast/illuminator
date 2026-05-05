// ============================================================================
// Illuminator 数据模型 — 数据批次、记录、堆栈采样
// ============================================================================
//
// 本文件定义了 Illuminator 流水线中流动的核心数据结构。
//
// 数据流模型概览：
// =================
// Illuminator 使用"批次"（DataBatch）作为流水线各阶段之间传递数据的基本单位。
// 一个 DataBatch 可以包含：
//   - 多个 Record（适用于指标 Metric 场景，如 CPU 利用率、内存使用）
//   - 多个 StackSample（适用于性能剖析 Profile 场景，如火焰图数据）
//
// 数据批次类型（DataBatch::Type）：
//   kMetrics  — 时间序列指标（CPU%、内存 KB、负载等）
//   kProfile  — 性能剖析样本（堆栈 + 计数）
//   kTrace    — 分布式追踪/事件
//   kLog      — 日志
//   kGeneric  — 通用/未分类
//
// 零拷贝（Zero-Copy）设计：
// ==========================
// 所有字符串数据（标签键值、字段名、函数名等）都存储在 Arena 内存分配器中。
// 通过 std::string_view 引用 Arena 中的字符串内存，避免了在整个流水线中
// 反复拷贝字符串。DataBatch 持有 Arena 的 shared_ptr，确保数据生命周期。
//
// 核心数据结构：
// ==============
// - Label:      键值对标签，用于标识数据来源和分组维度
// - FieldValue: 灵活的字段值类型（bool/int64/uint64/double/string_view）
// - Record:     单条指标记录，包含时间戳、标签集和字段值映射
// - StackFrame: 单层堆栈帧（函数地址、函数名、文件名、行号、模块名）
// - StackSample: 单条堆栈采样记录（进程/线程信息 + 内核栈 + 用户栈 + 计数）
// - DataBatch:  批次容器，携带 Arena 内存池 + 记录集合/堆栈样本集合
// ============================================================================

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

// ---- 时间戳类型 ----
// 使用纳秒精度的系统时钟时间点
using Timestamp = std::chrono::time_point<std::chrono::system_clock,
                                          std::chrono::nanoseconds>;

// 获取当前时间的纳秒时间戳
inline Timestamp NowTimestamp() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
}

// 将时间戳转为纳秒整数（从 Unix Epoch 起的纳秒数）
inline uint64_t TimestampToNanos(Timestamp ts) {
    return static_cast<uint64_t>(ts.time_since_epoch().count());
}

// ---- Label: 标签键值对 ----
// 使用 string_view 引用 Arena 中的字符串内存，无需额外拷贝
struct Label {
    std::string_view key;    // 标签键名（如 "cpu"、"pid"、"source"）
    std::string_view value;  // 标签值（如 "cpu0"、"1234"、"cpu_utilization"）
};

// ---- FieldValue: 灵活字段值 ----
// 使用 variant 支持多种值类型，通过 std::visit 统一处理
// std::monostate 表示"空值"或"未设置"
using FieldValue = std::variant<std::monostate, bool, int64_t, uint64_t,
                                double, std::string_view>;

// ---- Record: 单条数据记录 ----
// 流水线中的基本数据单元，适用于指标监控场景
struct Record {
    Timestamp timestamp;                                // 记录产生时间
    std::vector<Label> labels;                          // 标识标签集
    std::unordered_map<std::string_view, FieldValue> fields;  // 指标字段

    // 设置字段值
    void SetField(std::string_view key, FieldValue val) {
        fields[key] = std::move(val);
    }

    // 获取字段值（返回 nullptr 表示不存在）
    const FieldValue* GetField(std::string_view key) const {
        auto it = fields.find(key);
        return it != fields.end() ? &it->second : nullptr;
    }
};

// ---- StackFrame: 堆栈帧 ----
// 表示调用栈中的一个函数调用帧
struct StackFrame {
    uint64_t address = 0;              // 指令地址（用于符号化解析）
    std::string_view function_name;    // 符号化后的函数名
    std::string_view file_name;        // 源文件名
    uint32_t line_number = 0;          // 源代码行号
    std::string_view module_name;      // 模块名称（如 libc.so、myapp）
};

// ---- 采样类型枚举 ----
enum class SampleType : uint8_t {
    kOnCpu = 0,   // On-CPU 采样（进程正在 CPU 上执行）
    kOffCpu = 1,  // Off-CPU 采样（进程等待 I/O、锁、调度等）
};

// ---- StackSample: 堆栈采样记录 ----
// 用于性能剖析（Profiling）场景，记录一次采样时的完整调用栈
struct StackSample {
    Timestamp timestamp;                     // 采样时间
    uint32_t pid = 0;                        // 进程 ID
    uint32_t tid = 0;                        // 线程 ID
    std::string_view comm;                   // 进程名（command name）
    std::vector<StackFrame> kernel_stack;    // 内核态调用栈（从下到上）
    std::vector<StackFrame> user_stack;      // 用户态调用栈（从下到上）
    uint64_t count = 1;                      // 该采样发生的次数（合并后）

    uint32_t cpu = 0;                        // 在哪个 CPU 核上采样
    SampleType sample_type = SampleType::kOnCpu;  // On-CPU 或 Off-CPU
    uint64_t duration_ns = 0;                // 持续时间（仅 Off-CPU 采样有意义）
    int32_t kernel_stack_id = -1;            // eBPF 侧内核栈 ID
    int32_t user_stack_id = -1;              // eBPF 侧用户栈 ID
};

// ============================================================================
// DataBatch — 数据批次容器
// ============================================================================
// 流水线中各阶段传递数据的基本单位。
// 所有字符串数据存储在 Arena 内存池中，通过 shared_ptr 共享所有权。
// 支持两种数据承载方式：
//   - records():      指标记录列表
//   - stack_samples(): 堆栈采样列表
class DataBatch {
public:
    // 数据批次类型枚举
    enum class Type {
        kMetrics,   // 时间序列指标
        kProfile,   // 性能剖析样本
        kTrace,     // 追踪/事件
        kLog,       // 日志
        kGeneric,   // 通用/未分类
    };

    // 构造时创建一个新的 Arena 内存池
    explicit DataBatch(Type type = Type::kGeneric)
        : type_(type), arena_(std::make_shared<Arena>()) {}

    // 使用外部共享 Arena 构造（用于跨批次复用内存）
    DataBatch(Type type, std::shared_ptr<Arena> arena)
        : type_(type), arena_(std::move(arena)) {}

    // ---- 基本属性 ----
    Type type() const { return type_; }
    Arena& arena() { return *arena_; }
    const Arena& arena() const { return *arena_; }

    // ---- Record 记录访问 ----
    std::vector<Record>& records() { return records_; }
    const std::vector<Record>& records() const { return records_; }

    // 添加一条新记录，自动设置时间戳
    Record& AddRecord() {
        records_.emplace_back();
        records_.back().timestamp = NowTimestamp();
        return records_.back();
    }

    // ---- StackSample 堆栈采样访问 ----
    std::vector<StackSample>& stack_samples() { return stack_samples_; }
    const std::vector<StackSample>& stack_samples() const { return stack_samples_; }

    // 添加一条新堆栈采样，自动设置时间戳
    StackSample& AddStackSample() {
        stack_samples_.emplace_back();
        stack_samples_.back().timestamp = NowTimestamp();
        return stack_samples_.back();
    }

    // ---- 字符串内部化 ----
    // 将字符串拷贝到 Arena 中，返回 Arena 内的 string_view 引用
    // 这是实现零拷贝数据流的关键方法
    // 用法：record.labels.push_back({batch->InternString("key"), batch->InternString("value")});
    std::string_view InternString(std::string_view s) {
        return arena_->CopyString(s);
    }

    // ---- 容量信息 ----
    size_t Size() const { return records_.size() + stack_samples_.size(); }
    bool Empty() const { return records_.empty() && stack_samples_.empty(); }

    // 清空批次内容，重置 Arena
    void Clear() {
        records_.clear();
        stack_samples_.clear();
        arena_->Reset();
    }

    // ---- 批次元数据 ----
    // 用于存储批次级别的附加信息（如来源管道名、采集间隔等）
    void SetMeta(const std::string& key, const std::string& value) {
        metadata_[key] = value;
    }

    std::string GetMeta(const std::string& key, const std::string& def = "") const {
        auto it = metadata_.find(key);
        return it != metadata_.end() ? it->second : def;
    }

private:
    Type type_;                                             // 批次类型
    std::shared_ptr<Arena> arena_;                          // 共享 Arena 内存池
    std::vector<Record> records_;                           // 指标记录集合
    std::vector<StackSample> stack_samples_;                // 堆栈采样集合
    std::unordered_map<std::string, std::string> metadata_; // 批次元数据
};

// DataBatch 的 shared_ptr 别名，方便在代码中传递
using DataBatchPtr = std::shared_ptr<DataBatch>;

}  // namespace illuminator
