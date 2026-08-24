#include "neon/core/profiler.hpp"

#include <chrono>
#include <cstring>

namespace neon::core {

namespace {

double SteadyNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

Profiler& Profiler::Get() {
    static Profiler instance;
    return instance;
}

void Profiler::BeginFrame(uint64_t frame) {
    current_ = Sample{};
    current_.frame = frame;
    frameStartMs_ = NowMs();
    inFrame_ = true;
}

void Profiler::EndFrame() {
    if (!inFrame_) return;
    current_.totalMs = NowMs() - frameStartMs_;
    PushSample(current_);
    inFrame_ = false;
}

void Profiler::AddTiming(const char* name, double ms) {
    if (!inFrame_ || name == nullptr) return;
    for (int i = 0; i < current_.slotCount; ++i) {
        // Match by content, not pointer: identical string literals are not
        // guaranteed to share an address (MSVC Debug builds give each a
        // distinct copy), so pointer comparison would split one slot.
        if (current_.slots[i].name != nullptr &&
            std::strcmp(current_.slots[i].name, name) == 0) {
            current_.slots[i].ms += ms;
            return;
        }
    }
    if (current_.slotCount < kMaxSlots) {
        current_.slots[current_.slotCount].name = name;
        current_.slots[current_.slotCount].ms = ms;
        ++current_.slotCount;
    }
}

double Profiler::NowMs() const {
    return clock_ ? clock_() : SteadyNowMs();
}

uint64_t Profiler::LastFrame() const {
    return ring_.empty() ? 0 : ring_.back().frame;
}

void Profiler::Reset() {
    ring_.clear();
    current_ = Sample{};
    inFrame_ = false;
    frameStartMs_ = 0.0;
}

void Profiler::PushSample(const Sample& s) {
    if (ring_.size() < kRingCapacity) {
        ring_.push_back(s);
    } else {
        ring_.erase(ring_.begin()); // O(capacity) per frame; 512 entries, negligible
        ring_.push_back(s);
    }
}

std::string Profiler::Report(size_t frames) const {
    std::string out;
    const size_t start = frames == 0 || frames >= ring_.size() ? 0 : ring_.size() - frames;
    for (size_t i = ring_.size(); i > start; --i) {
        const Sample& s = ring_[i - 1];
        char buf[64];
        std::snprintf(buf, sizeof(buf), "frame %llu total=%.3fms", 
                      static_cast<unsigned long long>(s.frame), s.totalMs);
        out += buf;
        for (int k = 0; k < s.slotCount; ++k) {
            std::snprintf(buf, sizeof(buf), " %s=%.3fms",
                          s.slots[k].name ? s.slots[k].name : "?", s.slots[k].ms);
            out += buf;
        }
        out += "\n";
    }
    return out;
}

ScopedTimer::ScopedTimer(const char* name) : name_(name), startMs_(Profiler::Get().NowMs()) {}

ScopedTimer::~ScopedTimer() {
    Profiler::Get().AddTiming(name_, Profiler::Get().NowMs() - startMs_);
}

} // namespace neon::core
