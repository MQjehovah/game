#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace neon::core {

enum class LogLevel { Debug = 0, Info, Warn, Error };

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string text;
};

void Log(LogLevel level, const char* fmt, ...);
void SetLogLevel(LogLevel level);

// Editor/tool integration: a fixed-size ring buffer of recent log lines plus
// an optional subscriber callback. The game loop never needs this; it exists
// so tool UIs (e.g. the editor log panel) can render engine logs.
void AddLogSink(void (*sink)(const LogEntry&, void* userData), void* userData);
void RemoveLogSink(void (*sink)(const LogEntry&, void* userData), void* userData);

// Returns up to maxCount most recent entries (newest last). Thread-safe.
std::vector<LogEntry> GetRecentLogs(size_t maxCount);
void ClearLogs();

} // namespace neon::core

#define NEON_LOG_DEBUG(...) ::neon::core::Log(::neon::core::LogLevel::Debug, __VA_ARGS__)
#define NEON_LOG_INFO(...) ::neon::core::Log(::neon::core::LogLevel::Info, __VA_ARGS__)
#define NEON_LOG_WARN(...) ::neon::core::Log(::neon::core::LogLevel::Warn, __VA_ARGS__)
#define NEON_LOG_ERROR(...) ::neon::core::Log(::neon::core::LogLevel::Error, __VA_ARGS__)
