#pragma once
#include <cstdint>

namespace neon::core {

struct Time {
    float delta = 0.016f;    // seconds since last rendered frame
    float elapsed = 0.0f;    // total simulated time (fixed steps)
    uint64_t frameIndex = 0; // rendered frame counter

    float Fps() const { return delta > 0.0f ? 1.0f / delta : 0.0f; }
};

} // namespace neon::core
