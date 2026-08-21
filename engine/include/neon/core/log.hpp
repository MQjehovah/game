#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neon::core {

enum class LogLevel { Debug = 0, Info, Warn, Error };

// Log categories tag each line with the subsystem that produced it so the
// runtime filter can be tuned per subsystem (e.g. --log-cat gfx:debug).
// Names are lowercase and mirrored by CategoryName()/CategoryFromName().
enum class LogCategory {
    Core = 0, // default category used by the plain NEON_LOG_* macros
    Gfx,
    Audio,
    Physics,
    Scene,
    Script,
    Bt,
    Net,
    Editor,
    Game,
};

// Lowercase display name for a category ("core", "gfx", ...). Falls back to
// "core" for out-of-range values.
const char* CategoryName(LogCategory category);

// Category for a name, case-insensitive. Unknown names map to Core.
LogCategory CategoryFromName(const std::string& name);

// Level for a lowercase level name ("debug"/"info"/"warn"/"error"),
// case-insensitive. Returns false when the name is not recognized.
bool LogLevelFromName(const std::string& name, LogLevel& out);

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string text;
    // Extended fields, all defaulted so existing uses of the first two members
    // keep compiling unchanged.
    std::string file;   // source file that logged ("" when unknown)
    int line = 0;       // source line (0 when unknown)
    LogCategory category = LogCategory::Core;
    uint64_t frame = 0; // frame counter at log time (0 = none)
};

// Classic 2-arg form: category=Core, no source location, current frame.
// Kept unchanged for backward compatibility.
void Log(LogLevel level, const char* fmt, ...);

// Full form used by the NEON_LOG_* / NEON_LOG_CAT macros.
void Log(LogLevel level, LogCategory category, const char* file, int line,
         const char* fmt, ...);

// Global level: the baseline used by categories without an override. A line
// passes when level >= the effective level for its category (the per-category
// override when one is set, otherwise this global). Thread-safe.
void SetLogLevel(LogLevel level);

// Per-category override; replaces the global level for that category until it
// is overwritten. Thread-safe.
void SetCategoryLogLevel(LogCategory category, LogLevel level);

// Frame counter stamped on every entry and rendered as a [fNNNN] prefix.
// 0 disables the prefix. Thread-safe.
void SetLogFrame(uint64_t frame);
uint64_t GetLogFrame();

// File sink: appends the same formatted lines that go to stderr. The file is
// opened lazily on the first write and flushed per line. DisableFileLog()
// closes it. Thread-safe.
void EnableFileLog(const std::string& path);
void DisableFileLog();

// Editor/tool integration: a fixed-size ring buffer of recent log lines plus
// an optional subscriber callback. The game loop never needs this; it exists
// so tool UIs (e.g. the editor log panel) can render engine logs.
void AddLogSink(void (*sink)(const LogEntry&, void* userData), void* userData);
void RemoveLogSink(void (*sink)(const LogEntry&, void* userData), void* userData);

// Returns up to maxCount most recent entries (newest last). Thread-safe.
std::vector<LogEntry> GetRecentLogs(size_t maxCount);
void ClearLogs();

} // namespace neon::core

#define NEON_LOG_DEBUG(...)                                                    \
    ::neon::core::Log(::neon::core::LogLevel::Debug,                            \
                      ::neon::core::LogCategory::Core, __FILE__, __LINE__,      \
                      __VA_ARGS__)
#define NEON_LOG_INFO(...)                                                     \
    ::neon::core::Log(::neon::core::LogLevel::Info,                             \
                      ::neon::core::LogCategory::Core, __FILE__, __LINE__,      \
                      __VA_ARGS__)
#define NEON_LOG_WARN(...)                                                     \
    ::neon::core::Log(::neon::core::LogLevel::Warn,                             \
                      ::neon::core::LogCategory::Core, __FILE__, __LINE__,      \
                      __VA_ARGS__)
#define NEON_LOG_ERROR(...)                                                    \
    ::neon::core::Log(::neon::core::LogLevel::Error,                            \
                      ::neon::core::LogCategory::Core, __FILE__, __LINE__,      \
                      __VA_ARGS__)
#define NEON_LOG_CAT(category, level, ...)                                     \
    ::neon::core::Log((level), (category), __FILE__, __LINE__, __VA_ARGS__)
