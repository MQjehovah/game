#pragma once
#include <cmath>
#include "neon/math/vec2.hpp"

namespace neon::math {

// Pure X/Z-layout helpers for scattering scenery and reading character facing
// on a top-down minimap. All functions are deterministic and side-effect free,
// so scene composition and minimap arrow math can be unit-tested directly.
// The convention is a top-down minimap where world +X maps to screen +x and
// world +Z maps to screen +y (y grows downward on screen).

// Circular exclusion zone in world X/Z. Used to keep scenery out of play
// spaces (the village plaza, the spawn point).
struct ExclusionZone {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
};

// True when `p` (X/Z) lies inside any of `zones` (strict interior).
inline bool InExclusionZones(const Vec2& p, const ExclusionZone* zones, int zoneCount) {
    for (int i = 0; i < zoneCount; ++i) {
        float dx = p.x - zones[i].x;
        float dz = p.y - zones[i].z;
        float r = zones[i].radius;
        if (dx * dx + dz * dz < r * r) return true;
    }
    return false;
}

// True when `p` (X/Z) is closer than `minDist` to any placed point. O(n) per
// probe; n <= ~200 so the full scatter pass is O(n^2) and deliberately simple
// (a spatial hash grid would add code for no measurable gain at this scale).
inline bool TooCloseToAny(const Vec2& p, const Vec2* placed, int placedCount, float minDist) {
    float m2 = minDist * minDist;
    for (int i = 0; i < placedCount; ++i) {
        float dx = p.x - placed[i].x;
        float dz = p.y - placed[i].y;
        if (dx * dx + dz * dz < m2) return true;
    }
    return false;
}

// Triangle points (in minimap/design space) for a facing arrow at `center`.
// The character's world facing is the camera yaw `yaw` (camera-relative
// movement: pressing W moves along (-sin yaw, -cos yaw)), and world +Z maps to
// screen +y on a top-down minimap, so the arrow points along
// (-sin yaw, -cos yaw). `length` is the arrow tip distance from center,
// `halfWidth` the half-width of the base, which sits behind the tip.
inline void FacingArrowPoints(const Vec2& center, float yaw, float length, float halfWidth,
                              Vec2& tip, Vec2& left, Vec2& right) {
    Vec2 fwd{-std::sin(yaw), -std::cos(yaw)};
    Vec2 perp{-fwd.y, fwd.x};
    tip = center + fwd * length;
    Vec2 back = center - fwd * (length * 0.55f);
    left = back + perp * halfWidth;
    right = back - perp * halfWidth;
}

} // namespace neon::math
