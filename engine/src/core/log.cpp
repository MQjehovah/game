#include "neon/core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <ctime>

namespace neon::core {

static LogLevel g_logLevel = LogLevel::Debug;
static constexpr size_t kLogBufferSize = 2048;

struct LogSinkEntry {
    void (*fn)(const LogEntry&, void*) = nullptr;
    void* userData = nullptr;
};

// Minimal spinlock: std::mutex needs OS thread support which some toolchains
// (e.g. MinGW 8.1 win32 model) lack; atomic_flag is pure header-only.
static std::atomic_flag g_logLock = ATOMIC_FLAG_INIT;
struct LogLockGuard {
    std::atomic_flag& flag;
    explicit LogLockGuard(std::atomic_flag& f) : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~LogLockGuard() { flag.clear(std::memory_order_release); }
};
static std::vector<LogEntry> g_logBuffer;
static std::vector<LogSinkEntry> g_logSinks;

void SetLogLevel(LogLevel level) { g_logLevel = level; }

void AddLogSink(void (*sink)(const LogEntry&, void*), void* userData) {
    LogLockGuard lock(g_logLock);
    if (!sink) return;
    for (const LogSinkEntry& e : g_logSinks) {
        if (e.fn == sink && e.userData == userData) return;
    }
    g_logSinks.push_back({sink, userData});
}

void RemoveLogSink(void (*sink)(const LogEntry&, void*), void* userData) {
    LogLockGuard lock(g_logLock);
    for (auto it = g_logSinks.begin(); it != g_logSinks.end(); ++it) {
        if (it->fn == sink && it->userData == userData) {
            g_logSinks.erase(it);
            return;
        }
    }
}

std::vector<LogEntry> GetRecentLogs(size_t maxCount) {
    LogLockGuard lock(g_logLock);
    if (g_logBuffer.empty() || maxCount == 0) return {};
    size_t start = g_logBuffer.size() > maxCount ? g_logBuffer.size() - maxCount : 0;
    return std::vector<LogEntry>(g_logBuffer.begin() + static_cast<ptrdiff_t>(start),
                                 g_logBuffer.end());
}

void ClearLogs() {
    LogLockGuard lock(g_logLock);
    g_logBuffer.clear();
}

void Log(LogLevel level, const char* fmt, ...) {
    if (level < g_logLevel) return;

    const char* tag = "DEBUG";
    switch (level) {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info: tag = "INFO"; break;
        case LogLevel::Warn: tag = "WARN"; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::fprintf(stderr, "[%02d:%02d:%02d] [%s] %s\n",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, tag, buffer);
    std::fflush(stderr);

    LogEntry entry;
    entry.level = level;
    entry.text = buffer;
    LogEntry entryCopy = entry; // delivered to sinks after releasing the lock
    std::vector<LogSinkEntry> sinks;
    {
        LogLockGuard lock(g_logLock);
        g_logBuffer.push_back(entry);
        if (g_logBuffer.size() > kLogBufferSize) {
            g_logBuffer.erase(g_logBuffer.begin());
        }
        sinks = g_logSinks;
    }
    for (const LogSinkEntry& s : sinks) {
        if (s.fn) s.fn(entryCopy, s.userData);
    }
}

} // namespace neon::core
