#include "neon/neon.hpp"
#include "neon/gfx/ssao.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

struct DepthCtx {
    float depth;
};

float UniformDepth(const math::Vec2&, void* user) {
    return static_cast<DepthCtx*>(user)->depth;
}

// A plane through `origin` at depth `base` whose linear depth grows at `grad`
// per UV unit.
struct PlaneCtx {
    math::Vec2 origin;
    math::Vec2 grad;
    float base;
};

float SlopedPlane(const math::Vec2& uv, void* user) {
    auto* c = static_cast<PlaneCtx*>(user);
    return c->base + c->grad.x * (uv.x - c->origin.x) + c->grad.y * (uv.y - c->origin.y);
}

} // namespace

TEST(SsaoDepthEncodeRoundTrip) {
    for (float d : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f}) {
        const math::Vec4 packed = gfx::EncodeLinearDepth(d);
        CHECK_NEAR(gfx::DecodeLinearDepth(packed), d, 3e-3);
    }
}

TEST(SsaoOcclusionFlatIsZero) {
    // A flat surface: every neighbour depth equals the centre, so nothing
    // occludes it (diff <= bias) -> occlusion 0.
    DepthCtx ctx{0.5f};
    const float occ = gfx::SsaoOcclusion(0.5f, gfx::kSsaoRadius, gfx::kSsaoBias, UniformDepth,
                                         &ctx, {0.5f, 0.5f}, 0.01f);
    CHECK_NEAR(occ, 0.0f, 1e-4);
}

TEST(SsaoOcclusionFacesGainOcclusion) {
    // Geometry in front of the centre (depth 0.3 vs 0.5) occludes it.
    DepthCtx ctx{0.3f};
    const float occ = gfx::SsaoOcclusion(0.5f, gfx::kSsaoRadius, gfx::kSsaoBias, UniformDepth,
                                         &ctx, {0.5f, 0.5f}, 0.01f);
    CHECK(occ > 0.0f);
    // A far-away neighbour (depth 0.9) does not occlude.
    DepthCtx farCtx{0.9f};
    const float farOcc = gfx::SsaoOcclusion(0.5f, gfx::kSsaoRadius, gfx::kSsaoBias, UniformDepth,
                                            &farCtx, {0.5f, 0.5f}, 0.01f);
    CHECK_NEAR(farOcc, 0.0f, 1e-4);
}

TEST(SsaoOcclusionSlopedPlaneIsZero) {
    // A slanted plane whose depth grows downward. Without the depth-gradient
    // term the downhill taps read as closer and self-occlude, which left a
    // view-angle-dependent AO gradient on open ground that quantised into the
    // "horizontal shadow stripes". Passing the plane's slope makes the
    // expected depth cancel the geometric change, so the plane occludes
    // nothing.
    PlaneCtx ctx{{0.5f, 0.5f}, {0.0f, 2.0f}, 0.5f};
    const float occ = gfx::SsaoOcclusion(0.5f, gfx::kSsaoRadius, gfx::kSsaoBias, SlopedPlane,
                                         &ctx, {0.5f, 0.5f}, 0.1f, ctx.grad);
    CHECK_NEAR(occ, 0.0f, 1e-4);
    // Without the gradient (the old behaviour) the same plane self-occludes.
    const float bugOcc = gfx::SsaoOcclusion(0.5f, gfx::kSsaoRadius, gfx::kSsaoBias, SlopedPlane,
                                            &ctx, {0.5f, 0.5f}, 0.1f);
    CHECK(bugOcc > 0.0f);
}
