#include "neon/core/crash.hpp"

#include <cstdio>

#include "neon/core/log.hpp"
#include "neon/core/profiler.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace neon::core {

namespace {

constexpr const char* kCrashReportPath = "crash_report.txt";

std::string BuildReport() {
    std::string out = "NeonEngine crash report\n"
                      "======================\n"
                      "Recent log ring:\n";
    const std::vector<LogEntry> logs = GetRecentLogs(512);
    for (const LogEntry& e : logs) {
        out += "  [";
        out += e.level == LogLevel::Debug   ? "debug"
               : e.level == LogLevel::Info  ? "info"
               : e.level == LogLevel::Warn  ? "warn"
                                            : "error";
        out += "] [";
        out += CategoryName(e.category);
        out += "] ";
        if (!e.file.empty()) {
            out += e.file;
            if (e.line > 0) {
                char line[16];
                std::snprintf(line, sizeof(line), ":%d", e.line);
                out += line;
            }
            out += " ";
        }
        out += e.text;
        out += "\n";
    }
    out += "\nProfiler ring (newest first, ~8.5s at 60 Hz):\n";
    out += Profiler::Get().Report(Profiler::kRingCapacity);
    return out;
}

#if defined(_WIN32)

LONG WINAPI CrashFilter(_EXCEPTION_POINTERS*) {
    WriteCrashReport(kCrashReportPath);
    return EXCEPTION_CONTINUE_SEARCH; // let the OS terminate the process
}

#else

void FatalSignalHandler(int) {
    // Async-signal-safe: only raw syscalls. The full report is rendered on
    // Windows (SEH filter runs in normal context); POSIX dumps a marker plus
    // whatever the app already wrote via EnableFileLog.
    const char msg[] = "NeonEngine: fatal signal (see crash_report.txt if a "
                       "full report was written before this handler)\n";
    int fd = ::open(kCrashReportPath, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        ::write(fd, msg, sizeof(msg) - 1);
        ::close(fd);
    }
    _exit(128);
}

#endif

bool gInstalled = false;

} // namespace

bool WriteCrashReport(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const std::string report = BuildReport();
    const bool ok = std::fwrite(report.data(), 1, report.size(), f) == report.size();
    std::fclose(f);
    return ok;
}

void InstallCrashHandler() {
    if (gInstalled) return;
    gInstalled = true;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(&CrashFilter);
#else
    struct sigaction sa {};
    sa.sa_handler = &FatalSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
    ::sigaction(SIGILL, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
#endif
}

} // namespace neon::core
