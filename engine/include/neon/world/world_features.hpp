#pragma once

#include <functional>
#include <vector>

#include "neon/math/vec2.hpp"

namespace neon::world {

// A polyline road with a flat core and a blending shoulder. The surface height
// follows the terrain at the nearest centreline point, so roads bend over
// hills instead of cutting through them.
struct RoadSpec {
    std::vector<math::Vec2> points;
    float halfWidth = 4.0f;
    float shoulder = 3.0f;
};

// A flat pad under a POI / camp. `falloff` is the band outside `radius` over
// which the terrain blends back to its natural height.
struct PadSpec {
    math::Vec2 center;
    float radius = 12.0f;
    float falloff = 10.0f;
};

struct WorldFeatures {
    std::vector<RoadSpec> roads;
    std::vector<PadSpec> pads;
};

struct FlattenSample {
    float weight = 0.0f;  // 0 = natural terrain, 1 = fully flattened
    float height = 0.0f;  // target height for the winning feature
};

using HeightFn = std::function<float(float, float)>;

// Nearest point on the polyline to `p`. Returns the distance; writes the
// nearest point (and its segment) into `closest` when non-null.
float DistanceToPolyline(const std::vector<math::Vec2>& points, math::Vec2 p,
                         math::Vec2* closest);

// Strongest flattening feature at (x, z). `baseHeight` is the un-flattened
// terrain height function.
FlattenSample SampleFlattening(const WorldFeatures& features, float x, float z,
                               const HeightFn& baseHeight);

void AddPad(WorldFeatures& features, math::Vec2 center, float radius,
            float falloff = 10.0f);

} // namespace neon::world
