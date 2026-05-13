// ============================================================================
// Illuminator 日志系统 — spdlog 原生接口
// ============================================================================
// IL_TRACE/DEBUG/INFO/WARN/ERROR/FATAL 宏 — spdlog fmt 风格（"{}" 占位符）
// ============================================================================

#pragma once

#include <cstdlib>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace illuminator {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

inline spdlog::logger* GetDefaultLogger() {
    static auto logger = [] {
        auto console = spdlog::stderr_color_mt("illuminator");
        spdlog::set_default_logger(console);
        spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%s:%#] %v");
        spdlog::set_level(spdlog::level::info);
        return console;
    }();
    return logger.get();
}

inline void SetLogLevel(LogLevel level) {
    GetDefaultLogger();
    switch (level) {
        case LogLevel::kTrace: spdlog::set_level(spdlog::level::trace); break;
        case LogLevel::kDebug: spdlog::set_level(spdlog::level::debug); break;
        case LogLevel::kInfo:  spdlog::set_level(spdlog::level::info);  break;
        case LogLevel::kWarn:  spdlog::set_level(spdlog::level::warn);  break;
        case LogLevel::kError: spdlog::set_level(spdlog::level::err);   break;
        case LogLevel::kFatal: spdlog::set_level(spdlog::level::critical); break;
    }
}

}  // namespace illuminator

#define IL_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define IL_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define IL_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define IL_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define IL_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define IL_FATAL(...) \
    do { SPDLOG_CRITICAL(__VA_ARGS__); spdlog::default_logger_raw()->flush(); std::abort(); } while (0)
