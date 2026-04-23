#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace illuminator {

// Internal metrics for self-observability.
// Thread-safe counters and gauges for monitoring Illuminator's own health.
class InternalMetrics {
public:
    static InternalMetrics& Instance() {
        static InternalMetrics inst;
        return inst;
    }

    // Increment a counter
    void Inc(const std::string& name, uint64_t delta = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name] += delta;
    }

    // Set a gauge value
    void SetGauge(const std::string& name, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        gauges_[name] = value;
    }

    // Record a latency observation (microseconds)
    void RecordLatency(const std::string& name, uint64_t us) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& hist = histograms_[name];
        hist.count++;
        hist.sum += us;
        if (us < hist.min) hist.min = us;
        if (us > hist.max) hist.max = us;
    }

    uint64_t GetCounter(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(name);
        return it != counters_.end() ? it->second : 0;
    }

    double GetGauge(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gauges_.find(name);
        return it != gauges_.end() ? it->second : 0;
    }

    // Export all metrics as Prometheus exposition format
    std::string ExportPrometheus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;

        for (auto& [name, val] : counters_) {
            ss << "# TYPE illuminator_" << name << " counter\n";
            ss << "illuminator_" << name << " " << val << "\n";
        }

        for (auto& [name, val] : gauges_) {
            ss << "# TYPE illuminator_" << name << " gauge\n";
            ss << "illuminator_" << name << " " << val << "\n";
        }

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

    // Export as JSON
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

    struct Histogram {
        uint64_t count = 0;
        uint64_t sum = 0;
        uint64_t min = UINT64_MAX;
        uint64_t max = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, uint64_t> counters_;
    std::unordered_map<std::string, double> gauges_;
    std::unordered_map<std::string, Histogram> histograms_;
};

// Resource limiter: monitors Illuminator's own resource consumption
class ResourceLimiter {
public:
    struct Limits {
        uint64_t max_memory_bytes = 512 * 1024 * 1024;  // 512 MB
        double max_cpu_percent = 10.0;
        uint64_t max_disk_bytes = 1ULL * 1024 * 1024 * 1024;  // 1 GB
    };

    static ResourceLimiter& Instance() {
        static ResourceLimiter inst;
        return inst;
    }

    void SetLimits(const Limits& limits) { limits_ = limits; }
    const Limits& GetLimits() const { return limits_; }

    // Check current resource usage against limits
    struct Usage {
        uint64_t rss_bytes = 0;
        double cpu_percent = 0;
        bool memory_exceeded = false;
        bool cpu_exceeded = false;
    };

    Usage Check() {
        Usage u;
        u.rss_bytes = GetRssBytes();
        u.memory_exceeded = u.rss_bytes > limits_.max_memory_bytes;

        InternalMetrics::Instance().SetGauge("rss_bytes",
            static_cast<double>(u.rss_bytes));

        return u;
    }

private:
    ResourceLimiter() = default;

    static uint64_t GetRssBytes() {
        FILE* f = fopen("/proc/self/statm", "r");
        if (!f) return 0;
        unsigned long pages = 0;
        if (fscanf(f, "%*u %lu", &pages) != 1) pages = 0;
        fclose(f);
        return pages * 4096;
    }

    Limits limits_;
};

// Scoped timer for latency measurement
class ScopedTimer {
public:
    ScopedTimer(const std::string& name)
        : name_(name),
          start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_).count();
        InternalMetrics::Instance().RecordLatency(name_, us);
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

#define IL_SCOPED_TIMER(name) \
    ::illuminator::ScopedTimer _il_timer_##__LINE__(name)

}  // namespace illuminator
