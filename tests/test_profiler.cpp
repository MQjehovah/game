#include <cstdio>
#include <string>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

struct FakeClock {
    double now = 0.0;
    double operator()() const { return now; }
    void Advance(double ms) { now += ms; }
};

const core::Profiler::Sample* FindSample(const core::Profiler& p, uint64_t frame) {
    for (const auto& s : p.Ring())
        if (s.frame == frame) return &s;
    return nullptr;
}

} // namespace

// A scope timer records its elapsed wall time into the current frame slot.
TEST(ProfilerScopedTiming) {
    core::Profiler& p = core::Profiler::Get();
    p.Reset();
    FakeClock clock;
    p.SetClock([&]() { return clock(); });

    p.BeginFrame(1);
    clock.Advance(0.5);
    {
        core::ScopedTimer t("physics");
        clock.Advance(1.5);
    }
    clock.Advance(0.25);
    {
        core::ScopedTimer t("scripts");
        clock.Advance(2.0);
    }
    p.EndFrame();

    const core::Profiler::Sample* s = FindSample(p, 1);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK_NEAR(s->totalMs, 0.5 + 1.5 + 0.25 + 2.0, 1e-6);
    CHECK_EQ(s->slotCount, 2);
    if (s->slotCount != 2) return;
    CHECK_EQ(std::string(s->slots[0].name), std::string("physics"));
    CHECK_NEAR(s->slots[0].ms, 1.5, 1e-6);
    CHECK_EQ(std::string(s->slots[1].name), std::string("scripts"));
    CHECK_NEAR(s->slots[1].ms, 2.0, 1e-6);

    // ScopedTimer outside a frame is a no-op (must not crash or record).
    {
        core::ScopedTimer t("outside");
        clock.Advance(1.0);
    }
    p.EndFrame(); // ignored: no active frame
    CHECK_EQ(p.Ring().size(), 1u);
}

// Repeated timings with the same name accumulate into one slot.
TEST(ProfilerSlotAccumulates) {
    core::Profiler& p = core::Profiler::Get();
    p.Reset();
    FakeClock clock;
    p.SetClock([&]() { return clock(); });

    p.BeginFrame(2);
    {
        core::ScopedTimer t("draw");
        clock.Advance(1.0);
    }
    {
        core::ScopedTimer t("draw");
        clock.Advance(0.5);
    }
    p.AddTiming("manual", 0.25);
    p.EndFrame();

    const core::Profiler::Sample* s = FindSample(p, 2);
    CHECK(s != nullptr);
    if (!s) return;
    CHECK_EQ(s->slotCount, 2);
    if (s->slotCount != 2) return;
    CHECK_EQ(std::string(s->slots[0].name), std::string("draw"));
    CHECK_NEAR(s->slots[0].ms, 1.5, 1e-6);
    CHECK_EQ(std::string(s->slots[1].name), std::string("manual"));
    CHECK_NEAR(s->slots[1].ms, 0.25, 1e-6);
}

// The ring is fixed-capacity and wraps, keeping the newest samples.
TEST(ProfilerRingWraps) {
    core::Profiler& p = core::Profiler::Get();
    p.Reset();
    FakeClock clock;
    p.SetClock([&]() { return clock(); });

    for (uint64_t i = 1; i <= core::Profiler::kRingCapacity + 88; ++i) {
        p.BeginFrame(i);
        clock.Advance(1.0);
        p.EndFrame();
    }

    CHECK_EQ(p.Ring().size(), core::Profiler::kRingCapacity);
    CHECK_EQ(p.LastFrame(), core::Profiler::kRingCapacity + 88);
    CHECK_EQ(p.Ring().front().frame, 89u); // 1 + 88 dropped
    CHECK_EQ(p.Ring().back().frame, core::Profiler::kRingCapacity + 88);

    // Report is newest-first and contains the newest frame.
    const std::string report = p.Report(2);
    CHECK(report.find("frame " + std::to_string(core::Profiler::kRingCapacity + 88)) !=
          std::string::npos);
    const size_t firstLine = report.find('\n');
    CHECK(firstLine != std::string::npos);
    CHECK(report.substr(0, firstLine).find("total=") != std::string::npos);

    // Report(n) limits to n frames.
    const std::string limited = p.Report(1);
    CHECK(limited.find("frame " + std::to_string(core::Profiler::kRingCapacity + 87)) ==
          std::string::npos);
}

// WriteCrashReport emits the log ring + profiler ring into a file (G8-1).
TEST(ProfilerCrashReportWritesFile) {
    core::Profiler& p = core::Profiler::Get();
    p.Reset();
    FakeClock clock;
    p.SetClock([&]() { return clock(); });
    p.BeginFrame(42);
    p.AddTiming("crash_slot", 3.5);
    p.EndFrame();

    NEON_LOG_INFO("profiler crash-report marker");
    test::TempDir tmp;
    const std::string path = tmp.Str() + "/crash_report.txt";
    CHECK(core::WriteCrashReport(path));

    std::string text;
    CHECK(test::ReadFileAll(path, text));
    CHECK(text.find("NeonEngine crash report") != std::string::npos);
    CHECK(text.find("profiler crash-report marker") != std::string::npos);
    CHECK(text.find("Profiler ring") != std::string::npos);
    CHECK(text.find("crash_slot=3.500ms") != std::string::npos);
    CHECK(text.find("frame 42") != std::string::npos);
}
