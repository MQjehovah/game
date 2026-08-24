#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neon::core {

// Fixed-capacity ring of per-frame performance samples (G8-1). Main-thread
// only: no locks, no allocations after the first EndFrame. Each sample holds
// the frame's total time plus up to kMaxSlots named scope timings recorded by
// ScopedTimer (or AddTiming). The ring keeps ~8.5s of history at 60 Hz, which
// is what crash reports dump ("the last 5 seconds").
class Profiler {
public:
    static constexpr int kMaxSlots = 16;
    static constexpr size_t kRingCapacity = 512;

    struct Slot {
        const char* name = nullptr;
        double ms = 0.0;
    };
    struct Sample {
        uint64_t frame = 0;
        double totalMs = 0.0;
        int slotCount = 0;
        Slot slots[kMaxSlots];
    };

    // Millisecond clock; tests inject a fake one for determinism.
    using ClockFn = std::function<double()>;
    static Profiler& Get();
    void SetClock(ClockFn fn) { clock_ = std::move(fn); }

    // Begins a frame sample; resets the current slot set. EndFrame measures
    // the wall time since BeginFrame (via the clock) and pushes the sample
    // into the ring. Nested Begin/End without a matching End is ignored.
    void BeginFrame(uint64_t frame);
    void EndFrame();

    // Accumulates elapsed milliseconds into the current frame's named slot
    // (up to kMaxSlots unique names per frame). No-op outside a frame.
    void AddTiming(const char* name, double ms);

    // Current wall-clock milliseconds (uses the injected clock when set).
    double NowMs() const;

    // Chronological ring (oldest first, up to kRingCapacity samples).
    const std::vector<Sample>& Ring() const { return ring_; }
    size_t Capacity() const { return kRingCapacity; }
    uint64_t LastFrame() const;
    void Reset();

    // "frame N total=..ms name=..ms ..." for the most recent `frames`
    // samples, newest first. Used by --bench output and crash reports.
    std::string Report(size_t frames) const;

private:
    Profiler() { ring_.reserve(kRingCapacity); }
    void PushSample(const Sample& s);

    std::vector<Sample> ring_;
    Sample current_;
    bool inFrame_ = false;
    double frameStartMs_ = 0.0;
    ClockFn clock_;
};

// RAII scope timer: records elapsed time into the named slot of the current
// Profiler frame when it goes out of scope (no-op outside a frame).
class ScopedTimer {
public:
    explicit ScopedTimer(const char* name);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    const char* name_;
    double startMs_;
};

} // namespace neon::core
