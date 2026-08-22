#include "neon/core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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
static std::map<LogCategory, LogLevel> g_catLevels;
static uint64_t g_frame = 0;
static std::string g_filePath;
static std::ofstream g_fileStream;
static bool g_fileOpenFailed = false;

const char* CategoryName(LogCategory category) {
    switch (category) {
        case LogCategory::Core: return "core";
        case LogCategory::Gfx: return "gfx";
        case LogCategory::Audio: return "audio";
        case LogCategory::Physics: return "physics";
        case LogCategory::Scene: return "scene";
        case LogCategory::Ecs: return "ecs";
        case LogCategory::Script: return "script";
        case LogCategory::Bt: return "bt";
        case LogCategory::Net: return "net";
        case LogCategory::Editor: return "editor";
        case LogCategory::Game: return "game";
    }
    return "core";
}

namespace {

std::string Lowercase(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

const char* TagFor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "DEBUG";
}

} // namespace

LogCategory CategoryFromName(const std::string& name) {
    const std::string n = Lowercase(name);
    if (n == "gfx") return LogCategory::Gfx;
    if (n == "audio") return LogCategory::Audio;
    if (n == "physics") return LogCategory::Physics;
    if (n == "scene") return LogCategory::Scene;
    if (n == "ecs") return LogCategory::Ecs;
    if (n == "script") return LogCategory::Script;
    if (n == "bt") return LogCategory::Bt;
    if (n == "net") return LogCategory::Net;
    if (n == "editor") return LogCategory::Editor;
    if (n == "game") return LogCategory::Game;
    return LogCategory::Core; // "core" and unknown names
}

bool LogLevelFromName(const std::string& name, LogLevel& out) {
    const std::string n = Lowercase(name);
    if (n == "debug") {
        out = LogLevel::Debug;
        return true;
    }
    if (n == "info") {
        out = LogLevel::Info;
        return true;
    }
    if (n == "warn") {
        out = LogLevel::Warn;
        return true;
    }
    if (n == "error") {
        out = LogLevel::Error;
        return true;
    }
    return false;
}

void SetLogLevel(LogLevel level) {
    LogLockGuard lock(g_logLock);
    g_logLevel = level;
}

void SetCategoryLogLevel(LogCategory category, LogLevel level) {
    LogLockGuard lock(g_logLock);
    g_catLevels[category] = level;
}

void SetLogFrame(uint64_t frame) {
    LogLockGuard lock(g_logLock);
    g_frame = frame;
}

uint64_t GetLogFrame() {
    LogLockGuard lock(g_logLock);
    return g_frame;
}

void EnableFileLog(const std::string& path) {
    LogLockGuard lock(g_logLock);
    if (g_fileStream.is_open()) g_fileStream.close();
    g_filePath = path;
    g_fileOpenFailed = false;
}

void DisableFileLog() {
    LogLockGuard lock(g_logLock);
    if (g_fileStream.is_open()) g_fileStream.close();
    g_filePath.clear();
    g_fileOpenFailed = false;
}

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

namespace {

// Shared body for both Log() overloads. Formatting, the runtime gate, the ring
// buffer, the file sink and the current-frame stamp all happen under the lock;
// stderr write and sink delivery run after it is released (matching the old
// "sink delivery after lock release" contract and avoiding lock re-entrancy in
// user sinks).
void LogImpl(LogLevel level, LogCategory category, const char* file, int line,
             const char* fmt, va_list args) {
    LogEntry entry;
    entry.level = level;
    entry.category = category;
    entry.file = file ? file : "";
    entry.line = line;

    char buffer[kLogBufferSize] = {};
    char formatted[4096] = {};
    std::string framePrefix;
    std::string loc;
    std::string fileOpenWarn;
    std::vector<LogSinkEntry> sinks;
    LogEntry entryCopy;

    {
        LogLockGuard lock(g_logLock);

        LogLevel effective = g_logLevel;
        const auto overrideIt = g_catLevels.find(category);
        if (overrideIt != g_catLevels.end()) effective = overrideIt->second;
        if (level < effective) return;

        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        entry.text = buffer;
        entry.frame = g_frame;
        entryCopy = entry;

        if (g_frame > 0) {
            char frameBuf[32];
            std::snprintf(frameBuf, sizeof(frameBuf), "[f%04llu] ",
                          static_cast<unsigned long long>(g_frame));
            framePrefix = frameBuf;
        }
        if (!entry.file.empty()) {
            loc = entry.file;
            if (entry.line > 0) loc += ":" + std::to_string(entry.line);
            loc += " ";
        }

        // [HH:MM:SS.mmm] from the wall clock (ms precision) so the prefix reads
        // naturally and still carries the millisecond fraction for sortability.
        const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch - secs);
        std::time_t nowSecs = static_cast<std::time_t>(secs.count());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &nowSecs);
#else
        localtime_r(&nowSecs, &tm);
#endif
        const int written = std::snprintf(
            formatted, sizeof(formatted), "[%02d:%02d:%02d.%03d] %s[%s] [%s] %s%s",
            tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(millis.count()),
            framePrefix.c_str(), TagFor(level), CategoryName(category), loc.c_str(),
            buffer);
        (void)written;

        g_logBuffer.push_back(entry);
        if (g_logBuffer.size() > kLogBufferSize) {
            g_logBuffer.erase(g_logBuffer.begin());
        }
        sinks = g_logSinks;

        if (!g_filePath.empty()) {
            if (!g_fileStream.is_open() && !g_fileOpenFailed) {
                g_fileStream.open(g_filePath.c_str(), std::ios::out | std::ios::app);
                if (!g_fileStream.is_open()) {
                    g_fileOpenFailed = true;
                    fileOpenWarn =
                        "WARN [core] EnableFileLog: cannot open '" + g_filePath + "'";
                }
            }
            if (g_fileStream.is_open()) {
                g_fileStream << formatted << '\n';
                g_fileStream.flush();
            }
        }
    }

    if (!fileOpenWarn.empty()) {
        std::fprintf(stderr, "%s\n", fileOpenWarn.c_str());
        std::fflush(stderr);
    }

    std::fprintf(stderr, "%s\n", formatted);
    std::fflush(stderr);

    for (const LogSinkEntry& s : sinks) {
        if (s.fn) s.fn(entryCopy, s.userData);
    }
}

} // namespace

void Log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogImpl(level, LogCategory::Core, nullptr, 0, fmt, args);
    va_end(args);
}

void Log(LogLevel level, LogCategory category, const char* file, int line,
         const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogImpl(level, category, file, line, fmt, args);
    va_end(args);
}

} // namespace neon::core
