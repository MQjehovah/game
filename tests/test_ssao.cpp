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
