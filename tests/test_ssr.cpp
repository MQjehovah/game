#include "neon/neon.hpp"
#include "neon/gfx/ssr.hpp"
#include "helpers.hpp"

using namespace neon;

TEST(SsrHitWhenRayInFront) {
    // Ray at depth 0.6, surface at 0.4 (in front by 0.2 > thickness) -> hit.
    CHECK(gfx::SsrHit(0.6f, 0.4f, gfx::kSsrThickness));
}

TEST(SsrNoHitWhenSurfaceBehindOrClose) {
    // Surface behind the ray -> no hit.
    CHECK(!gfx::SsrHit(0.6f, 0.8f, gfx::kSsrThickness));
    // Surface within thickness (self-hit guard) -> no hit.
    CHECK(!gfx::SsrHit(0.6f, 0.6f - gfx::kSsrThickness * 0.5f, gfx::kSsrThickness));
}
