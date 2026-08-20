#pragma once
#include <cstdint>
#include "neon/math/vec3.hpp"

namespace neon::core {

// xorshift64* - small, fast, deterministic.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : state_(seed ? seed : 1u) {}

    uint64_t Next() {
        uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x * 0x2545F4914F6CDD1Dull;
    }

    float Float() { return (Next() >> 40) * (1.0f / 16777216.0f); }
    float Range(float lo, float hi) { return lo + (hi - lo) * Float(); }
    int Int(int lo, int hiExclusive) { return lo + static_cast<int>(Next() % static_cast<uint64_t>(hiExclusive - lo)); }
    bool Bool(float probability = 0.5f) { return Float() < probability; }

    math::Vec3 OnUnitSphere() {
        float z = Range(-1.0f, 1.0f);
        float a = Range(0.0f, 6.283185307f);
        float r = std::sqrt(1.0f - z * z);
        return {r * std::cos(a), z, r * std::sin(a)};
    }

private:
    uint64_t state_;
};

} // namespace neon::core
