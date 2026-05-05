// ============================================================================
// Illuminator 日志系统
// ============================================================================
//
// 本文件实现了一个线程安全的轻量级日志系统：
// 1. Logger 单例 — 负责日志级别过滤、格式化输出到 stderr
// 2. 便捷宏 — IL_TRACE/DEBUG/INFO/WARN/ERROR/FATAL 简化调用
//
// 设计理念：
// - 单例模式确保全局唯一日志输出点
// - 锁保护保证多线程日志输出不被交错
// - FATAL 级别在输出日志后调用 std::abort() 终止程序
// - 通过宏自动注入文件名和行号，无需手动传递
// ============================================================================

#pragma once

#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <chrono>

namespace illuminator {

// 日志级别定义（由低到高）
enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

// ---- Logger: 线程安全的全局日志器 ----
class Logger {
public:
    // 单例访问：线程安全的延迟初始化（C++11 static 保证）
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    // 设置最小输出日志级别（低于此级别的日志将被忽略）
    void SetLevel(LogLevel level) { level_ = level; }
    LogLevel GetLevel() const { return level_; }

    // 核心日志输出方法
    // 参数：级别、源文件名、行号、格式化字符串、可变参数列表
    void Log(LogLevel level, const char* file, int line, const char* fmt, ...) {
        // 级别过滤：低于当前设定级别的日志直接跳过
        if (level < level_) return;

        // 生成带毫秒精度的时间戳
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&time_t));

        // 格式化用户消息
        char msg_buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        va_end(args);

        const char* level_str = LevelToString(level);

        // 加锁输出：防止多线程日志交错
        std::lock_guard<std::mutex> lock(mutex_);
        fprintf(stderr, "%s.%03d [%s] %s:%d - %s\n",
                time_buf, static_cast<int>(ms.count()),
                level_str, file, line, msg_buf);

        // FATAL 级别立即终止程序
        if (level == LogLevel::kFatal) {
            std::abort();
        }
    }

private:
    Logger() = default;

    // 日志级别转字符串（对齐输出）
    static const char* LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::kTrace: return "TRACE";
            case LogLevel::kDebug: return "DEBUG";
            case LogLevel::kInfo:  return "INFO ";
            case LogLevel::kWarn:  return "WARN ";
            case LogLevel::kError: return "ERROR";
            case LogLevel::kFatal: return "FATAL";
        }
        return "?????";
    }

    LogLevel level_ = LogLevel::kInfo;  // 默认 INFO 级别
    std::mutex mutex_;                  // 保护日志输出的互斥锁
};

// ---- 便捷日志宏 ----
// 自动注入 __FILE__ 和 __LINE__，简化调用
// 用法：IL_INFO("Pipeline %s started", name);

#define IL_LOG(level, fmt, ...) \
    ::illuminator::Logger::Instance().Log( \
        level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define IL_TRACE(fmt, ...) IL_LOG(::illuminator::LogLevel::kTrace, fmt, ##__VA_ARGS__)
#define IL_DEBUG(fmt, ...) IL_LOG(::illuminator::LogLevel::kDebug, fmt, ##__VA_ARGS__)
#define IL_INFO(fmt, ...)  IL_LOG(::illuminator::LogLevel::kInfo,  fmt, ##__VA_ARGS__)
#define IL_WARN(fmt, ...)  IL_LOG(::illuminator::LogLevel::kWarn,  fmt, ##__VA_ARGS__)
#define IL_ERROR(fmt, ...) IL_LOG(::illuminator::LogLevel::kError, fmt, ##__VA_ARGS__)
#define IL_FATAL(fmt, ...) IL_LOG(::illuminator::LogLevel::kFatal, fmt, ##__VA_ARGS__)

}  // namespace illuminator
