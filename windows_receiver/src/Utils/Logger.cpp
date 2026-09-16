#include "Logger.h"

#include <chrono>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace thunderlink {

LogLevel   Logger::s_level       = LogLevel::Info;
FILE*      Logger::s_file        = nullptr;
std::mutex Logger::s_mutex;
bool       Logger::s_initialized = false;
std::deque<LogEntry> Logger::s_recent;

void Logger::Init(const char* logFile, bool allocConsole) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) return;

#ifdef _WIN32
    /* Optional debug console, opt-in via --console. Off by default so a GUI
     * launch no longer pops up a terminal window — logs are available in-app
     * via the Logs panel and in the log file. */
    if (allocConsole && AllocConsole()) {
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);

        /* Enable ANSI colors on Windows 10+ */
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#else
    (void)allocConsole;
#endif

    if (logFile) {
        /* Append so the cross-device log accumulates across runs. */
#ifdef _WIN32
        fopen_s(&s_file, logFile, "a");
#else
        s_file = fopen(logFile, "a");
#endif
        if (s_file) {
            fprintf(s_file, "\n===== ThunderLink Windows session start =====\n");
            fflush(s_file);
        }
    }

    s_initialized = true;
}

#ifdef _WIN32
void Logger::InitWide(const wchar_t* logFile, bool allocConsole) {
    Init(nullptr, allocConsole);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (logFile && !s_file) {
        _wfopen_s(&s_file, logFile, L"a");
        if (s_file) { fprintf(s_file, "\n===== ThunderLink Windows session start =====\n"); fflush(s_file); }
    }
}
#endif

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_file) {
        fclose(s_file);
        s_file = nullptr;
    }
    s_initialized = false;
}

void Logger::SetLevel(LogLevel level) {
    s_level = level;
}

void Logger::Log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < s_level) return;

    /* Timestamp */
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d.%03d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());

    /* Extract just the filename from the path */
    const char* filename = file;
    const char* p = file;
    while (*p) {
        if (*p == '/' || *p == '\\') filename = p + 1;
        p++;
    }

    /* Format the user message */
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    /* Compose the full line once, then fan it out to console/file/panel. */
    char lineBuf[2200];
    snprintf(lineBuf, sizeof(lineBuf), "[%s] [%-5s] %s:%d - %s",
             timeStr, LevelToString(level), filename, line, msgBuf);

    std::lock_guard<std::mutex> lock(s_mutex);

    /* Console output with color */
    SetConsoleColor(level);
    fprintf(stderr, "%s\n", lineBuf);
    ResetConsoleColor();

    /* File output (no color) */
    if (s_file) {
        fprintf(s_file, "%s\n", lineBuf);
        fflush(s_file);
    }

    /* In-memory ring buffer for the Logs overlay */
    s_recent.push_back(LogEntry{ level, lineBuf });
    if (s_recent.size() > kMaxRecent) s_recent.pop_front();
}

std::vector<LogEntry> Logger::GetRecentLogs() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return std::vector<LogEntry>(s_recent.begin(), s_recent.end());
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

void Logger::SetConsoleColor(LogLevel level) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
    switch (level) {
        case LogLevel::Trace: SetConsoleTextAttribute(hOut, 8);  break; // Gray
        case LogLevel::Debug: SetConsoleTextAttribute(hOut, 7);  break; // White
        case LogLevel::Info:  SetConsoleTextAttribute(hOut, 10); break; // Green
        case LogLevel::Warn:  SetConsoleTextAttribute(hOut, 14); break; // Yellow
        case LogLevel::Error: SetConsoleTextAttribute(hOut, 12); break; // Red
        case LogLevel::Fatal: SetConsoleTextAttribute(hOut, 79); break; // White on Red
    }
#else
    (void)level;
#endif
}

void Logger::ResetConsoleColor() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
    SetConsoleTextAttribute(hOut, 7); // Default white
#endif
}

} // namespace thunderlink
