#pragma once

#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

namespace thunderlink {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class Logger {
public:
    static void Init(const char* logFile = nullptr);
    static void Shutdown();
    static void SetLevel(LogLevel level);
    static void Log(LogLevel level, const char* file, int line, const char* fmt, ...);

private:
    static LogLevel       s_level;
    static FILE*          s_file;
    static std::mutex     s_mutex;
    static bool           s_initialized;

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
