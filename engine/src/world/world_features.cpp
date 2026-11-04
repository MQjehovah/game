#include "neon/world/world_features.hpp"

#include <cmath>

#include "neon/core/json.hpp"

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
    float weightSum = 0.0f;
    float heightSum = 0.0f;

    auto add = [&](float weight, float height) {
        if (weight <= 0.0f) return;
        weightSum += weight;
        heightSum += weight * height;
    };

    for (const RoadSpec& road : features.roads) {
        math::Vec2 q;
        const float d = DistanceToPolyline(road.points, p, &q);
        float weight = 0.0f;
        if (d <= road.halfWidth) {
            weight = 1.0f;
        } else if (road.shoulder > 0.0f && d < road.halfWidth + road.shoulder) {
            weight = 1.0f - SmoothStep(road.halfWidth, road.halfWidth + road.shoulder, d);
        }
        add(weight, baseHeight ? baseHeight(q.x, q.y) : 0.0f);
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
        const float target = std::isfinite(pad.height)
                                 ? pad.height
                                 : (baseHeight ? baseHeight(pad.center.x, pad.center.y) : 0.0f);
        add(weight, target);
    }

    if (weightSum > 0.0f) {
        // Blend overlapping features instead of switching to the strongest
        // one: an argmax swap makes the surface jump by w * (h1 - h2) at the
        // crossover. Clamp the blend weight so an overlap never over-flattens
        // beyond a single feature.
        out.weight = weightSum > 1.0f ? 1.0f : weightSum;
        out.height = heightSum / weightSum;
    }
    return out;
}

void AddPad(WorldFeatures& features, math::Vec2 center, float radius,
            float falloff) {
    features.pads.push_back({center, radius, falloff});
}

WorldFeatures LoadWorldFeatures(const std::string& jsonText) {
    WorldFeatures out;
    std::string err;
    const core::Json root = core::Json::Parse(jsonText, &err);
    if (const core::Json* roads = root.Get("roads")) {
        if (roads->IsArray()) {
            for (const core::Json& r : roads->Items()) {
                RoadSpec road;
                if (const core::Json* w = r.Get("halfWidth"))
                    road.halfWidth = static_cast<float>(w->GetNumber(4.0));
                if (const core::Json* w = r.Get("shoulder"))
                    road.shoulder = static_cast<float>(w->GetNumber(3.0));
                if (const core::Json* pts = r.Get("points")) {
                    if (pts->IsArray()) {
                        for (const core::Json& p : pts->Items()) {
                            if (p.IsArray() && p.Size() >= 2 && p.At(0) && p.At(1))
                                road.points.push_back(
                                    {static_cast<float>(p.At(0)->GetNumber()),
                                     static_cast<float>(p.At(1)->GetNumber())});
                        }
                    }
                }
                if (road.points.size() >= 2) out.roads.push_back(std::move(road));
            }
        }
    }
    if (const core::Json* pads = root.Get("pads")) {
        if (pads->IsArray()) {
            for (const core::Json& p : pads->Items()) {
                const core::Json* c = p.Get("center");
                if (c == nullptr || !c->IsArray() || c->Size() < 2) continue;
                PadSpec pad;
                pad.center = {static_cast<float>(c->At(0)->GetNumber()),
                              static_cast<float>(c->At(1)->GetNumber())};
                if (const core::Json* n = p.Get("radius"))
                    pad.radius = static_cast<float>(n->GetNumber(12.0));
                if (const core::Json* n = p.Get("falloff"))
                    pad.falloff = static_cast<float>(n->GetNumber(10.0));
                if (const core::Json* n = p.Get("height"))
                    pad.height = static_cast<float>(n->GetNumber(0.0));
                out.pads.push_back(pad);
            }
        }
    }
    return out;
}

} // namespace neon::world
