#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>
#include <deque>

namespace thunderlink {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

/* One buffered log line, kept in memory for the in-app Logs panel. */
struct LogEntry {
    LogLevel    level;
    std::string text;
};

class Logger {
public:
    static void Init(const char* logFile = nullptr, bool allocConsole = false);
    static void Shutdown();
    static void SetLevel(LogLevel level);
    static void Log(LogLevel level, const char* file, int line, const char* fmt, ...);

    /* Snapshot (newest last) of the most recent lines, for the Logs overlay. */
    static std::vector<LogEntry> GetRecentLogs();

private:
    static LogLevel       s_level;
    static FILE*          s_file;
    static std::mutex     s_mutex;
    static bool           s_initialized;

    /* Ring buffer of recent lines shown in the Logs panel. */
    static std::deque<LogEntry> s_recent;
    static constexpr size_t     kMaxRecent = 500;

    static const char* LevelToString(LogLevel level);
    static void SetConsoleColor(LogLevel level);
    static void ResetConsoleColor();
};

} // namespace thunderlink

#define LOG_TRACE(fmt, ...) ::thunderlink::Logger::Log(::thunderlink::LogLevel::Trace, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) ::thunderlink::Logger::Log(::thunderlink::LogLevel::Debug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ::thunderlink::Logger::Log(::thunderlink::LogLevel::Info,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::thunderlink::Logger::Log(::thunderlink::LogLevel::Warn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::thunderlink::Logger::Log(::thunderlink::LogLevel::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) ::thunderlink::Logger::Log(::thunderlink::LogLevel::Fatal, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
