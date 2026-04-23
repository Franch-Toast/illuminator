#pragma once

#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <chrono>

namespace illuminator {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLevel(LogLevel level) { level_ = level; }
    LogLevel GetLevel() const { return level_; }

    void Log(LogLevel level, const char* file, int line, const char* fmt, ...) {
        if (level < level_) return;

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&time_t));

        char msg_buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        va_end(args);

        const char* level_str = LevelToString(level);

        std::lock_guard<std::mutex> lock(mutex_);
        fprintf(stderr, "%s.%03d [%s] %s:%d - %s\n",
                time_buf, static_cast<int>(ms.count()),
                level_str, file, line, msg_buf);

        if (level == LogLevel::kFatal) {
            std::abort();
        }
    }

private:
    Logger() = default;

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

    LogLevel level_ = LogLevel::kInfo;
    std::mutex mutex_;
};

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
