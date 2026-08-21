#include <string>
#include <vector>

#if defined(__STDCPP_THREADS__)
#include <thread>
#endif

#include "neon/core/log.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Captures every entry a sink receives so tests can assert on exactly what
// passed the runtime gate (sinks only see accepted lines).
struct CaptureSink {
    std::vector<core::LogEntry> entries;
    static void OnEntry(const core::LogEntry& entry, void* userData) {
        static_cast<CaptureSink*>(userData)->entries.push_back(entry);
    }
};

} // namespace

TEST(LogCategoryFiltering) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();
    core::SetCategoryLogLevel(core::LogCategory::Gfx, core::LogLevel::Warn);

    CaptureSink sink;
    core::AddLogSink(&CaptureSink::OnEntry, &sink);

    NEON_LOG_CAT(core::LogCategory::Gfx, core::LogLevel::Debug, "gfx debug line");
    NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info, "core info line");

    core::RemoveLogSink(&CaptureSink::OnEntry, &sink);
    core::SetCategoryLogLevel(core::LogCategory::Gfx, core::LogLevel::Debug);

    CHECK_EQ(sink.entries.size(), 1u);
    if (sink.entries.size() == 1) {
        CHECK(sink.entries[0].category == core::LogCategory::Core);
        CHECK(sink.entries[0].level == core::LogLevel::Info);
        CHECK(sink.entries[0].text.find("core info line") != std::string::npos);
    }
}

TEST(LogSourceLocation) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();

    CaptureSink sink;
    core::AddLogSink(&CaptureSink::OnEntry, &sink);

    NEON_LOG_INFO("loc test %d", 7);

    core::RemoveLogSink(&CaptureSink::OnEntry, &sink);

    CHECK_EQ(sink.entries.size(), 1u);
    if (!sink.entries.empty()) {
        CHECK(!sink.entries[0].file.empty());
        CHECK(sink.entries[0].line > 0);
        CHECK(sink.entries[0].text.find("loc test 7") != std::string::npos);
    }
}

TEST(LogFrame) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();
    core::SetLogFrame(42);
    NEON_LOG_INFO("frame probe");
    core::SetLogFrame(0);

    const std::vector<core::LogEntry> entries = core::GetRecentLogs(100);
    CHECK_EQ(entries.size(), 1u);
    if (!entries.empty()) CHECK_EQ(entries[0].frame, 42u);
}

TEST(LogGlobalLevelGate) {
    core::SetLogLevel(core::LogLevel::Warn);

    CaptureSink sink;
    core::AddLogSink(&CaptureSink::OnEntry, &sink);

    NEON_LOG_INFO("should be suppressed");
    NEON_LOG_WARN("warn passes");
    NEON_LOG_ERROR("error passes");

    core::RemoveLogSink(&CaptureSink::OnEntry, &sink);
    core::SetLogLevel(core::LogLevel::Debug);

    CHECK_EQ(sink.entries.size(), 2u);
    if (sink.entries.size() == 2) {
        CHECK(sink.entries[0].level == core::LogLevel::Warn);
        CHECK(sink.entries[1].level == core::LogLevel::Error);
    }
}

TEST(LogPermissiveCategoryOverride) {
    // Headline behavior: a per-category override that is MORE permissive than
    // the global gate lets that category through while others stay suppressed.
    core::SetLogLevel(core::LogLevel::Warn);
    core::SetCategoryLogLevel(core::LogCategory::Gfx, core::LogLevel::Debug);

    CaptureSink sink;
    core::AddLogSink(&CaptureSink::OnEntry, &sink);

    NEON_LOG_CAT(core::LogCategory::Gfx, core::LogLevel::Debug, "gfx debug passes");
    NEON_LOG_DEBUG("core debug suppressed");

    core::RemoveLogSink(&CaptureSink::OnEntry, &sink);
    core::SetLogLevel(core::LogLevel::Debug);
    core::SetCategoryLogLevel(core::LogCategory::Gfx, core::LogLevel::Debug);

    CHECK_EQ(sink.entries.size(), 1u);
    if (sink.entries.size() == 1) {
        CHECK(sink.entries[0].category == core::LogCategory::Gfx);
        CHECK(sink.entries[0].level == core::LogLevel::Debug);
        CHECK(sink.entries[0].text.find("gfx debug passes") != std::string::npos);
    }
}

TEST(LogBackwardCompat) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();

    core::Log(core::LogLevel::Info, "hello %d", 5);

    const std::vector<core::LogEntry> entries = core::GetRecentLogs(100);
    CHECK_EQ(entries.size(), 1u);
    if (!entries.empty()) {
        CHECK_EQ(entries[0].text, std::string("hello 5"));
        CHECK(entries[0].category == core::LogCategory::Core);
        CHECK(entries[0].file.empty());
        CHECK_EQ(entries[0].line, 0);
    }
}

TEST(LogCategoryNames) {
    CHECK(std::string(core::CategoryName(core::LogCategory::Gfx)) == "gfx");
    CHECK(std::string(core::CategoryName(core::LogCategory::Net)) == "net");
    CHECK(std::string(core::CategoryName(core::LogCategory::Core)) == "core");
    CHECK(core::CategoryFromName("gfx") == core::LogCategory::Gfx);
    CHECK(core::CategoryFromName("GFX") == core::LogCategory::Gfx);
    CHECK(core::CategoryFromName("audio") == core::LogCategory::Audio);
    CHECK(core::CategoryFromName("bt") == core::LogCategory::Bt);
    // Unknown names fall back to Core (documented behavior).
    CHECK(core::CategoryFromName("bogus") == core::LogCategory::Core);
}

TEST(LogFileSink) {
    core::SetLogLevel(core::LogLevel::Debug);
    test::TempDir tmp;
    const std::string path = tmp.Str() + "/log.txt";
    core::EnableFileLog(path);
    NEON_LOG_INFO("file sink line %d", 3);
    core::DisableFileLog();

    std::string text;
    CHECK(test::ReadFileAll(path, text));
    CHECK(text.find("file sink line 3") != std::string::npos);
}

#if defined(__STDCPP_THREADS__)

// Real thread-safety smoke: 4 threads x 100 lines through the spinlock. Only
// compiled when the toolchain actually provides std::thread (not the MinGW 8.1
// win32 threading model used here, which lacks it).
TEST(LogConcurrency) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i) {
                NEON_LOG_INFO("concurrent %d %d", t, i);
            }
        });
    }
    for (std::thread& th : threads) th.join();

    const std::vector<core::LogEntry> entries = core::GetRecentLogs(1000);
    CHECK_EQ(entries.size(), 400u);
    for (const core::LogEntry& e : entries) {
        CHECK(e.text.find("concurrent") != std::string::npos);
    }
}

#else

// This toolchain (MinGW 8.1 x86_64-win32-seh) ships no std::thread, and the
// CMake build links Threads only on non-Windows. Exercise the same lock path
// sequentially with the same volume of entries instead: it verifies the buffer
// bookkeeping and the spinlock's non-corruption under repeated acquisition.
TEST(LogConcurrency) {
    core::SetLogLevel(core::LogLevel::Debug);
    core::ClearLogs();

    for (int i = 0; i < 400; ++i) {
        NEON_LOG_INFO("concurrent 0 %d", i);
    }

    const std::vector<core::LogEntry> entries = core::GetRecentLogs(1000);
    CHECK_EQ(entries.size(), 400u);
    for (const core::LogEntry& e : entries) {
        CHECK(e.text.find("concurrent") != std::string::npos);
    }
}

#endif
