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
    Ecs,
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

// Rate limiter for log lines that a per-frame loop can emit thousands of times
// (a failing script callback, a broken asset polled every tick). The first
// `burst` occurrences are logged verbatim; after that only one summary line is
// emitted every `reportEvery` occurrences, carrying the suppressed count. This
// keeps the log readable and stops a single bad line from filling the log file
// (and the editor log ring) for the rest of the session.
class LogThrottle {
public:
    LogThrottle() = default;
    LogThrottle(uint32_t burst, uint32_t reportEvery)
        : burst_(burst ? burst : 1), reportEvery_(reportEvery ? reportEvery : 1) {}

    // True when the caller should emit this occurrence (either one of the first
    // `burst` lines or a periodic summary).
    bool Allow() {
        ++calls_;
        if (calls_ <= burst_) return true;
        ++suppressed_;
        if (++sinceReport_ >= reportEvery_) {
            sinceReport_ = 0;
            return true;
        }
        return false;
    }
    // Occurrences suppressed since this was last read (call it when Allow()
    // returned true to include the count in the summary line).
    uint32_t TakeSuppressed() {
        const uint32_t n = suppressed_;
        suppressed_ = 0;
        return n;
    }
    uint64_t Calls() const { return calls_; }

private:
    uint32_t burst_ = 4;
    uint32_t reportEvery_ = 600;
    uint64_t calls_ = 0;
    uint32_t suppressed_ = 0;
    uint32_t sinceReport_ = 0;
};

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
