#include "neon/world/world_features.hpp"

#include <cmath>

namespace neon::world {
namespace {

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float DistanceToPolyline(const std::vector<math::Vec2>& points, math::Vec2 p,
                         math::Vec2* closest) {
    if (points.empty()) {
        if (closest) *closest = p;
        return 1e30f;
    }
    if (points.size() == 1) {
        if (closest) *closest = points[0];
        const float dx = p.x - points[0].x;
        const float dz = p.y - points[0].y;
        return std::sqrt(dx * dx + dz * dz);
    }

    float best = 1e30f;
    math::Vec2 bestPoint = points[0];
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const math::Vec2 a = points[i];
        const math::Vec2 b = points[i + 1];
        const float abx = b.x - a.x;
        const float abz = b.y - a.y;
        const float len2 = abx * abx + abz * abz;
        float t = 0.0f;
        if (len2 > 1e-12f) {
            t = ((p.x - a.x) * abx + (p.y - a.y) * abz) / len2;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }
        const math::Vec2 q{a.x + abx * t, a.y + abz * t};
        const float dx = p.x - q.x;
        const float dz = p.y - q.y;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < best) {
            best = d;
            bestPoint = q;
        }
    }
    if (closest) *closest = bestPoint;
    return best;
}

FlattenSample SampleFlattening(const WorldFeatures& features, float x, float z,
                               const HeightFn& baseHeight) {
    FlattenSample out;
    const math::Vec2 p{x, z};

    for (const RoadSpec& road : features.roads) {
        math::Vec2 q;
        const float d = DistanceToPolyline(road.points, p, &q);
        float weight = 0.0f;
        if (d <= road.halfWidth) {
            weight = 1.0f;
        } else if (road.shoulder > 0.0f && d < road.halfWidth + road.shoulder) {
            weight = 1.0f - SmoothStep(road.halfWidth, road.halfWidth + road.shoulder, d);
        }
        if (weight > out.weight) {
            out.weight = weight;
            out.height = baseHeight ? baseHeight(q.x, q.y) : 0.0f;
        }
    }

    for (const PadSpec& pad : features.pads) {
        const float dx = p.x - pad.center.x;
        const float dz = p.y - pad.center.y;
        const float d = std::sqrt(dx * dx + dz * dz);
        float weight = 0.0f;
        if (d <= pad.radius) {
            weight = 1.0f;
        } else if (pad.falloff > 0.0f && d < pad.radius + pad.falloff) {
            weight = 1.0f - SmoothStep(pad.radius, pad.radius + pad.falloff, d);
        }
        if (weight > out.weight) {
            out.weight = weight;
            out.height = baseHeight ? baseHeight(pad.center.x, pad.center.y) : 0.0f;
        }
    }

    return out;
}

void AddPad(WorldFeatures& features, math::Vec2 center, float radius,
            float falloff) {
    features.pads.push_back({center, radius, falloff});
}

} // namespace neon::world
