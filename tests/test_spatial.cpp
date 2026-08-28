#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Task 3.4c: spatial clarity - scatter rejection (min-spacing + exclusion),
// determinism, minimap facing-arrow math, and fog coverage of the skinned and
// instanced lit draw paths.
// ---------------------------------------------------------------------------

namespace {

// NullBackend that records which uniform names the renderer uploaded, so the
// headless path can assert that fog state reaches the skinned and instanced
// lit programs (both share the lit fragment shader).
class FogRecordingBackend : public test::NullBackend {
public:
    std::set<std::string> floats;
    std::set<std::string> vec3s;
    int nextShaderId = 1;

    // B1: distinct programs must have distinct handles -- the renderer now
    // re-applies the scene-uniform block per program, so the mock must reflect
    // real backends (which give every CreateShader call a unique handle).
    neon::gfx::ShaderHandle CreateShader(const char*, const char*, const char*) override {
        return {static_cast<uint32_t>(nextShaderId++)};
    }

    void SetUniformFloat(const char* name, float) override {
        if (name) floats.insert(name);
    }
    void SetUniformVec3(const char* name, const math::Vec3&) override {
        if (name) vec3s.insert(name);
    }
};

// Records 2D-overlay submissions so the new DrawTriangle2D primitive can be
// asserted to reach the backend as a single filled triangle.
class UIRecordingBackend : public test::NullBackend {
public:
    int trianglesSubmitted = 0;
    int minVertexCount = 0;

    void DrawPrimitives(const void*, uint32_t vertexCount, uint32_t, const uint16_t*,
                        uint32_t indexCount, gfx::PrimitiveTopology topology) override {
        if (topology == gfx::PrimitiveTopology::Triangles) {
            ++trianglesSubmitted;
            minVertexCount = vertexCount;
        }
    }
};

// Small skinned quad (4 verts, 2 triangles, one joint each) for the fog-path
// guard test - enough to exercise the SKINNED lit shader upload path.
gfx::Mesh MakeSkinnedQuad(gfx::Renderer& renderer) {
    std::vector<gfx::Vertex3D> verts;
    std::vector<uint16_t> indices;
    std::vector<uint16_t> jointIds;
    std::vector<float> jointWeights;
    const float k = 1.0f;
    for (int i = 0; i < 4; ++i) {
        gfx::Vertex3D v;
        v.pos = {(i & 1) ? k : -k, (i & 2) ? k : -k, 0.0f};
        v.normal = {0, 0, 1};
        v.uv = {(i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f};
        v.j[0] = 0.0f;
        v.j[1] = 1.0f;
        v.w[0] = 1.0f;
        v.w[1] = 0.0f;
        verts.push_back(v);
        jointIds.insert(jointIds.end(), {0, 1, 0, 0});
        jointWeights.insert(jointWeights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
    }
    indices = {0, 1, 2, 1, 3, 2};
    gfx::Mesh mesh = gfx::Mesh::CreateFromData(renderer, verts.data(),
                                               static_cast<uint32_t>(verts.size()),
                                               indices.data(),
                                               static_cast<uint32_t>(indices.size()), "skinquad");
    mesh.AttachSkinData(std::move(jointIds), std::move(jointWeights), 0);
    return mesh;
}

// Replicates the demo's scatter loop (angle/radius, exclusion, min-spacing,
// scale on acceptance) using the shared pure helpers. Returns the placed X/Z.
std::vector<math::Vec2> RunScatter(core::Rng& rng, const math::ExclusionZone* zones,
                                   int zoneCount, float minR, float maxR, float minSpacing,
                                   int target) {
    std::vector<math::Vec2> placed;
    for (int i = 0; i < target; ++i) {
        bool placedOk = false;
        for (int attempt = 0; attempt < 64 && !placedOk; ++attempt) {
            float a = rng.Range(0.0f, math::kTwoPi);
            float r = rng.Range(minR, maxR);
            math::Vec2 xz{std::cos(a) * r, std::sin(a) * r};
            if (math::InExclusionZones(xz, zones, zoneCount)) continue;
            if (math::TooCloseToAny(xz, placed.data(), static_cast<int>(placed.size()),
                                    minSpacing))
                continue;
            (void)rng.Range(0.5f, 2.0f); // scale draw, fixed order like the demo
            placed.push_back(xz);
            placedOk = true;
        }
    }
    return placed;
}

} // namespace

// A candidate inside a zone is rejected; outside is accepted; the boundary is
// not inside (strict interior).
TEST(ExclusionZoneInsideRejectedOutsideAccepted) {
    const math::ExclusionZone zones[] = {{0.0f, 0.0f, 5.0f}};
    CHECK(math::InExclusionZones({3.0f, 0.0f}, zones, 1));   // inside
    CHECK(!math::InExclusionZones({7.0f, 0.0f}, zones, 1));  // outside
    CHECK(!math::InExclusionZones({0.0f, 5.0f}, zones, 1));  // exactly on the edge
    CHECK(math::InExclusionZones({0.0f, -4.9f}, zones, 1));  // inside (negative axis)
}

// A point inside a second zone is rejected even when the first is far away.
TEST(ExclusionZoneChecksAllZones) {
    const math::ExclusionZone zones[] = {{-100.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 3.0f}};
    CHECK(math::InExclusionZones({2.0f, 0.0f}, zones, 2));
    CHECK(!math::InExclusionZones({10.0f, 0.0f}, zones, 2));
}

// Min-spacing: a candidate closer than minDist to any placed point is rejected,
// a far candidate is accepted, and exactly-at-min is accepted (strict <).
TEST(MinSpacingRejectsClusteredAcceptsSpaced) {
    const math::Vec2 placed[2] = {{0.0f, 0.0f}, {10.0f, 0.0f}};
    const float minDist = 4.0f;
    CHECK(math::TooCloseToAny({1.0f, 0.0f}, placed, 2, minDist));    // 1 away
    CHECK(math::TooCloseToAny({10.0f, 3.0f}, placed, 2, minDist));   // 3 away from #2
    CHECK(!math::TooCloseToAny({4.0f, 0.0f}, placed, 2, minDist));   // exactly minDist
    CHECK(!math::TooCloseToAny({5.0f, 0.0f}, placed, 2, minDist));   // 5 away from both
    CHECK(!math::TooCloseToAny({50.0f, 0.0f}, placed, 2, minDist));  // far away
}

// The demo scatter loop, given a fixed seed and fixed rejection params, yields
// an identical placement set run-to-run; every placement respects the exclusion
// zone and minimum spacing.
TEST(ScatterDeterministicSpacingAndExclusion) {
    const math::ExclusionZone zones[] = {{0.0f, 0.0f, 10.0f}};
    core::Rng a(0xC0FFEEull);
    core::Rng b(0xC0FFEEull);
    std::vector<math::Vec2> pa = RunScatter(a, zones, 1, 10.0f, 50.0f, 4.0f, 60);
    std::vector<math::Vec2> pb = RunScatter(b, zones, 1, 10.0f, 50.0f, 4.0f, 60);
    CHECK_EQ(pa.size(), pb.size());
    CHECK_EQ(pa.size(), 60u); // dense-enough ring, cap never exceeded
    for (size_t i = 0; i < pa.size(); ++i) {
        CHECK_NEAR(pa[i].x, pb[i].x, 1e-6);
        CHECK_NEAR(pa[i].y, pb[i].y, 1e-6);
        CHECK(!math::InExclusionZones(pa[i], zones, 1));
        for (size_t j = i + 1; j < pa.size(); ++j) {
            float dx = pa[i].x - pa[j].x;
            float dy = pa[i].y - pa[j].y;
            CHECK(dx * dx + dy * dy >= 4.0f * 4.0f - 1e-6f);
        }
    }
}

// The minimap arrow triangle points along the world-facing direction
// (-sin yaw, -cos yaw) in screen space (top-down minimap: world +Z -> +y).
TEST(FacingArrowRotatesWithYaw) {
    math::Vec2 tip, left, right;
    const math::Vec2 center{100.0f, 100.0f};
    const float len = 9.0f;
    const float half = 3.0f;

    // yaw 0 -> forward (0,-1): tip above the center.
    math::FacingArrowPoints(center, 0.0f, len, half, tip, left, right);
    CHECK_NEAR(tip.x, center.x, 1e-5);
    CHECK_NEAR(tip.y, center.y - len, 1e-5);
    CHECK_NEAR(left.y, right.y, 1e-5); // base is horizontal (perpendicular)
    CHECK_NEAR(left.x - right.x, 2.0f * half, 1e-5);

    // yaw PI -> forward (0,1): tip below.
    math::FacingArrowPoints(center, math::kPi, len, half, tip, left, right);
    CHECK_NEAR(tip.y, center.y + len, 1e-5);

    // yaw -PI/2 -> forward (1,0): tip to the right.
    math::FacingArrowPoints(center, -math::kHalfPi, len, half, tip, left, right);
    CHECK_NEAR(tip.x, center.x + len, 1e-5);
    CHECK_NEAR(tip.y, center.y, 1e-5);

    // yaw +PI/2 -> forward (-1,0): tip to the left.
    math::FacingArrowPoints(center, math::kHalfPi, len, half, tip, left, right);
    CHECK_NEAR(tip.x, center.x - len, 1e-5);

    // Generic angle: tip direction matches the camera-forward convention.
    const float yaw = 0.7f;
    math::FacingArrowPoints(center, yaw, len, half, tip, left, right);
    CHECK_NEAR(tip.x - center.x, -std::sin(yaw) * len, 1e-5);
    CHECK_NEAR(tip.y - center.y, -std::cos(yaw) * len, 1e-5);
    // Base sits behind the tip: both corners closer to center than the tip.
    CHECK(math::Distance(left, center) < math::Distance(tip, center));
    CHECK(math::Distance(right, center) < math::Distance(tip, center));
}

// Fog coverage guard: the skinned and instanced lit paths both receive the fog
// uniforms (uFogColor / uFogStart / uFogEnd), proving scenery, skinned meshes
// and terrain share the lit fragment shader's distance fade. Consistent with
// the T3.3 renderer-state assertion approach.
TEST(FogUniformsReachSkinnedAndInstancedPaths) {
    gfx::Renderer renderer;
    auto recorder = std::make_unique<FogRecordingBackend>();
    FogRecordingBackend* rec = recorder.get();
    renderer.AttachBackendForTesting(std::move(recorder));

    renderer.SetFog({0.42f, 0.55f, 0.72f, 1.0f}, 45.0f, 170.0f);
    renderer.SetCamera({}, 16.0f / 9.0f);

    gfx::Mesh skinned = MakeSkinnedQuad(renderer);
    std::vector<math::Mat4> bones(1, math::Mat4::Identity());
    // B1: distinct programs get distinct shader handles (the mock mirrors real
    // backends); the scene-uniform block re-applies per program.
    gfx::Shader shaderA = renderer.CreateShader("", "", "s1");
    gfx::Material matA = gfx::Material::Lit({}, gfx::Color::White, 16.0f);
    matA.shader = shaderA.Handle();
    renderer.DrawSkinnedMesh(skinned, matA, math::Mat4::Identity(), bones, 1);
    CHECK(rec->floats.count("uFogStart") == 1);
    CHECK(rec->floats.count("uFogEnd") == 1);
    CHECK(rec->vec3s.count("uFogColor") == 1);

    rec->floats.clear();
    rec->vec3s.clear();
    gfx::Mesh cube = gfx::Mesh::CreateCube(renderer, 1, 1, 1, "cube");
    math::Mat4 models[2] = {math::Mat4::Identity(),
                            math::Mat4::Translation({0.0f, 0.0f, 40.0f})};
    gfx::Shader shaderB = renderer.CreateShader("", "", "s2");
    gfx::Material matB = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    matB.shader = shaderB.Handle();
    renderer.DrawMeshInstanced(cube, matB, models, 2);
    CHECK(rec->floats.count("uFogStart") == 1);
    CHECK(rec->floats.count("uFogEnd") == 1);
    CHECK(rec->vec3s.count("uFogColor") == 1);
}

// The minimap arrow's DrawTriangle2D primitive must emit a single filled
// triangle into the 2D overlay when flushed.
TEST(DrawTriangle2DEmittedAsFilledTriangle) {
    gfx::Renderer renderer;
    auto recorder = std::make_unique<UIRecordingBackend>();
    UIRecordingBackend* rec = recorder.get();
    renderer.AttachBackendForTesting(std::move(recorder));

    // One triangle: same shape the minimap facing arrow uses.
    renderer.DrawTriangle2D({100.0f, 100.0f}, {106.0f, 103.0f}, {103.0f, 109.0f},
                            gfx::Color{0.3f, 0.85f, 1.0f, 0.85f});
    renderer.Flush2D();
    CHECK_EQ(rec->trianglesSubmitted, 1);
    CHECK_EQ(rec->minVertexCount, 3);
}
