// ============================================================================
// Illuminator 自观测系统 — 内部指标监控与资源限制
// ============================================================================
//
// 本文件提供了 Illuminator 的"自观测"（Self-Observability）能力，
// 即 Illuminator 监控自身的健康状况和资源消耗，类似于 Prometheus 的
// 内部指标（如进程 RSS、CPU 使用率、延迟分布等）。
//
// 三大组件：
// ==========
// 1. InternalMetrics 单例
//    - 线程安全的内部指标收集器
//    - 支持三种指标类型：
//      * Counter（计数器）：只增不减，如 batches_total、errors_total
//      * Gauge（仪表盘）：可增可减，如 rss_bytes、memory_usage
//      * Histogram（直方图/摘要）：记录延迟分布，含 count、sum、min、max、avg
//    - 支持 Prometheus exposition 格式和 JSON 格式导出
//    - 通过 HTTP /metrics 端点暴露，可被 Prometheus 抓取
//
// 2. ResourceLimiter 单例
//    - 监控 Illuminator 自身的资源消耗，防止过度占用系统资源
//    - 通过读取 /proc/self/statm 获取 RSS（常驻内存）大小
//    - 内存超限检测：默认上限 512MB
//    - CPU 和磁盘限制预留了扩展接口
//
// 3. ScopedTimer 工具类
//    - RAII 风格的延迟测量器
//    - 构造时记录开始时间，析构时自动上报延迟到 InternalMetrics
//    - 配合 IL_SCOPED_TIMER 宏使用，方便在关键路径插入性能埋点
//
// 设计理念：
//   - 所有指标收集器内部加锁，外部线程安全
//   - 单例模式确保全局唯一的数据汇聚点
//   - 不在快速路径进行重量级操作（锁粒度小，仅在写入时持锁）
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#include <sstream>

namespace illuminator {

// ============================================================================
// InternalMetrics — 线程安全的内部指标收集器
// ============================================================================
// 用途：监控 Illuminator 自身的运行状态，包括：
//   - pipeline 处理的数据批次数和记录数
//   - 各级插件的错误次数
//   - 关键操作的延迟分布
//   - 进程内存使用量
class InternalMetrics {
public:
    // 单例访问 — 线程安全的延迟初始化
    static InternalMetrics& Instance() {
        static InternalMetrics inst;
        return inst;
    }

    // ---- Counter 计数器操作 ----
    // 计数器只增不减，适合统计事件次数
    // name: 指标名称，delta: 增量（默认 1）
    void Inc(const std::string& name, uint64_t delta = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name] += delta;
    }

    // ---- Gauge 仪表盘操作 ----
    // 仪表盘可增可减，适合表示瞬时状态值（如内存使用量）
    void SetGauge(const std::string& name, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gauges_[name] = value;
    }

    // ---- 延迟记录 ----
    // 记录一次操作的延迟（微秒单位），用于生成分布统计
    void RecordLatency(const std::string& name, uint64_t us) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& hist = histograms_[name];
        hist.count++;          // 增加样本数
        hist.sum += us;        // 累加总延迟
        if (us < hist.min) hist.min = us;  // 更新最小值
        if (us > hist.max) hist.max = us;  // 更新最大值
    }

    // ---- 读取操作 ----
    // 获取指定 Counter 的当前值
    uint64_t GetCounter(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(name);
        return it != counters_.end() ? it->second : 0;
    }

    // 获取指定 Gauge 的当前值
    double GetGauge(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gauges_.find(name);
        return it != gauges_.end() ? it->second : 0;
    }

    // ---- 导出：Prometheus exposition 格式 ----
    // 生成兼容 Prometheus 的文本格式，可直接在 /metrics 端点暴露
    // 输出示例：
    //   # TYPE illuminator_batches_processed counter
    //   illuminator_batches_processed 42
    //   # TYPE illuminator_pipeline_latency summary
    //   illuminator_pipeline_latency_count 100
    //   illuminator_pipeline_latency_sum 5000000
    //   illuminator_pipeline_latency_avg 50000
    std::string ExportPrometheus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;

        // 导出所有 Counter
        for (auto& [name, val] : counters_) {
            ss << "# TYPE illuminator_" << name << " counter\n";
            ss << "illuminator_" << name << " " << val << "\n";
        }

        // 导出所有 Gauge
        for (auto& [name, val] : gauges_) {
            ss << "# TYPE illuminator_" << name << " gauge\n";
            ss << "illuminator_" << name << " " << val << "\n";
        }

        // 导出所有 Histogram（作为 summary 类型）
        for (auto& [name, hist] : histograms_) {
            ss << "# TYPE illuminator_" << name << " summary\n";
            ss << "illuminator_" << name << "_count " << hist.count << "\n";
            ss << "illuminator_" << name << "_sum " << hist.sum << "\n";
            if (hist.count > 0) {
                ss << "illuminator_" << name << "_avg "
                   << (hist.sum / hist.count) << "\n";
                ss << "illuminator_" << name << "_min " << hist.min << "\n";
                ss << "illuminator_" << name << "_max " << hist.max << "\n";
            }
        }

        return ss.str();
    }

    // ---- 导出：JSON 格式 ----
    // 用于前端仪表盘或其他非 Prometheus 消费者
    std::string ExportJson() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;
        ss << "{\"counters\":{";
        bool first = true;
        for (auto& [name, val] : counters_) {
            if (!first) ss << ",";
            ss << "\"" << name << "\":" << val;
            first = false;
        }
        ss << "},\"gauges\":{";
        first = true;
        for (auto& [name, val] : gauges_) {
            if (!first) ss << ",";
            ss << "\"" << name << "\":" << val;
            first = false;
        }
        ss << "},\"histograms\":{";
        first = true;
        for (auto& [name, hist] : histograms_) {
            if (!first) ss << ",";
            ss << "\"" << name << "\":{\"count\":" << hist.count
               << ",\"sum\":" << hist.sum
               << ",\"min\":" << hist.min
               << ",\"max\":" << hist.max << "}";
            first = false;
        }
        ss << "}}";
        return ss.str();
    }

private:
    InternalMetrics() = default;

    // 直方图/摘要数据结构
    struct Histogram {
        uint64_t count = 0;           // 样本总数
        uint64_t sum = 0;             // 延迟总和（微秒）
        uint64_t min = UINT64_MAX;    // 最小延迟（初始化为最大可能值）
        uint64_t max = 0;             // 最大延迟
    };

    mutable std::mutex mutex_;                             // 保护所有映射表的互斥锁
    std::unordered_map<std::string, uint64_t> counters_;   // 计数器映射
    std::unordered_map<std::string, double> gauges_;       // 仪表盘映射
    std::unordered_map<std::string, Histogram> histograms_; // 直方图映射
};

// ============================================================================
// ResourceLimiter — 资源限制器
// ============================================================================
// 监控 Illuminator 自身的资源消耗，防止过度占用系统资源。
// 当前主要关注内存使用（通过 /proc/self/statm 获取 RSS）。
class ResourceLimiter {
public:
    // 资源上限配置
    struct Limits {
        uint64_t max_memory_bytes = 512 * 1024 * 1024;  // 最大内存 512 MB
        double max_cpu_percent = 10.0;                    // 最大 CPU 占用 10%
        uint64_t max_disk_bytes = 1ULL * 1024 * 1024 * 1024; // 最大磁盘 1 GB
    };

    // 单例访问
    static ResourceLimiter& Instance() {
        static ResourceLimiter inst;
        return inst;
    }

    void SetLimits(const Limits& limits) { limits_ = limits; }
    const Limits& GetLimits() const { return limits_; }

    // 当前资源使用情况
    struct Usage {
        uint64_t rss_bytes = 0;       // 常驻内存大小（字节）
        double cpu_percent = 0;       // CPU 使用率（预留）
        bool memory_exceeded = false; // 内存是否超限
        bool cpu_exceeded = false;    // CPU 是否超限（预留）
    };

    // 检查当前资源消耗是否超出限制
    Usage Check() {
        Usage u;
        u.rss_bytes = GetRssBytes();                           // 读取进程 RSS
        u.memory_exceeded = u.rss_bytes > limits_.max_memory_bytes; // 判断是否超限

        // 将内存使用量同步到内部指标系统
        InternalMetrics::Instance().SetGauge("rss_bytes",
            static_cast<double>(u.rss_bytes));

        return u;
    }

private:
    ResourceLimiter() = default;

    // 从 /proc/self/statm 读取进程 RSS（常驻内存页数）
    // 输出格式示例：`45123 2345 1234 ...`
    //   第1列: 总虚拟内存页数
    //   第2列: RSS 页数（我们需要这个）
    // 字节数 = 页数 × 系统页大小（sysconf(_SC_PAGESIZE)）
    static uint64_t GetRssBytes() {
        FILE* f = fopen("/proc/self/statm", "r");
        if (!f) return 0;
        unsigned long pages = 0;
        // fscanf 格式：跳过第一列（%*u），读取第二列到 pages
        if (fscanf(f, "%*u %lu", &pages) != 1) pages = 0;
        fclose(f);
        long psz = ::sysconf(_SC_PAGESIZE);
        if (psz <= 0) psz = 4096;
        return pages * static_cast<uint64_t>(psz);
    }

    Limits limits_;
};

// ============================================================================
// ScopedTimer — RAII 延迟测量器
// ============================================================================
// 在构造时记录开始时间，析构时自动计算耗时并上报到 InternalMetrics。
// 用法：
//   void SomeCriticalPath() {
//       IL_SCOPED_TIMER("pipeline_process");  // 这一行等价于声明一个 ScopedTimer
//       // ... 关键路径代码 ...
//   }  // ScopedTimer 析构时自动上报延迟
//
class ScopedTimer {
public:
    // 构造时记录开始时间点和指标名称
    ScopedTimer(const std::string& name)
        : name_(name),
          start_(std::chrono::steady_clock::now()) {}

    // 析构时自动计算耗时（微秒）并上报
    ~ScopedTimer() {
        auto end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_).count();
        InternalMetrics::Instance().RecordLatency(name_, us);
    }

private:
    std::string name_;                              // 指标名称
    std::chrono::steady_clock::time_point start_;   // 开始时间
};

// 便捷宏：创建 ScopedTimer 并自动使用行号生成唯一变量名
// 使用方法：在函数开头写 `IL_SCOPED_TIMER("operation_name");`
#define IL_SCOPED_TIMER(name) \
    ::illuminator::ScopedTimer _il_timer_##__LINE__(name)

}  // namespace illuminator
