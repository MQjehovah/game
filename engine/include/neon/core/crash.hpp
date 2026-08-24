#pragma once

#include <string>

namespace neon::core {

// Installs process-wide crash handlers (Windows SEH unhandled-exception
// filter, POSIX fatal signals) that write <cwd>/crash_report.txt with the
// recent log ring and the last ~8.5s of profiler samples (G8-1), then let the
// process terminate. Idempotent; call once at application startup.
void InstallCrashHandler();

// Writes the same report to `path` directly (used by the handlers and by
// tests). Returns true on success.
bool WriteCrashReport(const std::string& path);

} // namespace neon::core
