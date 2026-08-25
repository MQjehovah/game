#pragma once

#include <cmath>

namespace neon::gfx {

// Volumetric distance fog (exponential), applied at composite time against the
// scene depth. Unlike the lit shader's linear start/end fog, this densifies
// with distance and reads the post-render depth, so it also affects objects the
// lit shader cannot see (sprites, decals, instanced props). Pure math here so
// the curve is unit-testable.

inline float FogFactor(float distance, float density) {
    // Exponential-squared fog: classic, cheap, smooth. density > 0.
    return 1.0f - std::exp(-density * density * distance * distance);
}

} // namespace neon::gfx
