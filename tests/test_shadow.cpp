#include <cmath>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// CSM cascade math (pure, headless - no GL needed).
// ---------------------------------------------------------------------------

TEST(CsmSplits) {
    float splits[4] = {0, 0, 0, 0};
    gfx::ComputeCascadeSplits(0.1f, 100.0f, splits);
    CHECK_NEAR(splits[0], 0.1, 1e-4);
    CHECK_NEAR(splits[3], 100.0, 1e-4);
    // Linear interior fractions {0.2, 0.6} of the near..far range.
    CHECK_NEAR(splits[1], 0.1f + 0.2f * 99.9f, 1e-3);
    CHECK_NEAR(splits[2], 0.1f + 0.6f * 99.9f, 1e-3);
    // Monotonic strictly increasing.
    CHECK(splits[0] < splits[1] && splits[1] < splits[2] && splits[2] < splits[3]);
    // Degenerate range still yields a valid 3-cascade partition.
    gfx::ComputeCascadeSplits(5.0f, 5.0f, splits);
    CHECK(splits[0] == 5.0f && splits[3] == 5.0f);
    CHECK(splits[1] <= splits[3] && splits[2] <= splits[3]);
}

TEST(CsmSelectCascade) {
    float splits[4] = {0, 0, 0, 0};
    gfx::ComputeCascadeSplits(0.1f, 100.0f, splits);
    CHECK_EQ(gfx::SelectCascade(5.0f, splits), 0);
    CHECK_EQ(gfx::SelectCascade(splits[1] - 0.01f, splits), 0);
    CHECK_EQ(gfx::SelectCascade(splits[1], splits), 1);
    CHECK_EQ(gfx::SelectCascade(splits[2] - 0.01f, splits), 1);
    CHECK_EQ(gfx::SelectCascade(splits[2], splits), 2);
    CHECK_EQ(gfx::SelectCascade(99.0f, splits), 2);
    // Outside the range clamps to the nearest cascade instead of going OOB.
    CHECK_EQ(gfx::SelectCascade(0.001f, splits), 0);
    CHECK_EQ(gfx::SelectCascade(1e6f, splits), 2);
}

TEST(CsmLightViewProjCoversSlice) {
    gfx::Camera cam;
    cam.position = {0.0f, 3.0f, 10.0f};
    cam.target = {0.0f, 1.0f, 0.0f};
    const float aspect = 16.0f / 9.0f;
    const math::Vec3 lightDir = math::Vec3{-0.4f, -1.0f, -0.3f}.Normalized();

    float splits[4] = {0, 0, 0, 0};
    gfx::ComputeCascadeSplits(cam.nearPlane, cam.farPlane, splits);

    // Recompute the slice corners the same way the cascade builder does, then
    // assert every corner lands inside the light ortho clip volume [-1,1].
    const math::Vec3 forward = (cam.target - cam.position).Normalized();
    const math::Vec3 right = math::Cross(forward, cam.up).Normalized();
    const math::Vec3 up = math::Cross(right, forward);
    const float tanHalf = std::tan(cam.fovY * 0.5f);

    for (int cascade = 0; cascade < 3; ++cascade) {
        math::Mat4 lvp =
            gfx::ComputeCascadeLightViewProj(lightDir, cam, aspect, splits[cascade],
                                             splits[cascade + 1]);
        const float distances[2] = {splits[cascade], splits[cascade + 1]};
        for (int d = 0; d < 2; ++d) {
            const float halfH = tanHalf * distances[d];
            const float halfW = halfH * aspect;
            const math::Vec3 center = cam.position + forward * distances[d];
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    math::Vec3 corner = center + right * (halfW * static_cast<float>(sx)) +
                                        up * (halfH * static_cast<float>(sy));
                    math::Vec4 clip = lvp.TransformVec4({corner.x, corner.y, corner.z, 1.0f});
                    CHECK(clip.w > 0.0f);
                    float nx = clip.x / clip.w, ny = clip.y / clip.w, nz = clip.z / clip.w;
                    CHECK(nx >= -1.01f && nx <= 1.01f);
                    CHECK(ny >= -1.01f && ny <= 1.01f);
                    CHECK(nz >= -1.01f && nz <= 1.01f);
                }
            }
        }
    }

    // A light direction parallel to the camera up axis must not produce a
    // degenerate basis (right vector fallback).
    math::Mat4 lvp = gfx::ComputeCascadeLightViewProj({0.0f, 1.0f, 0.0f}, cam, aspect,
                                                       splits[0], splits[1]);
    math::Vec4 clip = lvp.TransformVec4({cam.position.x, cam.position.y, cam.position.z, 1.0f});
    CHECK(std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z));
}

// ---------------------------------------------------------------------------
// Renderer capability path (NullBackend): CSM must stay disabled, no crash.
// ---------------------------------------------------------------------------

TEST(CsmNullBackendStaysDisabled) {
    test::HeadlessAssetFixture fx;
    CHECK(!fx.renderer.ShadowsEnabled());
    // Forcing it off is a no-op on the headless path (no GL init at all).
    fx.renderer.SetShadowsEnabled(false);
    CHECK(!fx.renderer.ShadowsEnabled());
    CHECK(!fx.renderer.ShadowMapActive());

    // SetCamera + DrawMesh with the null backend must not crash even though no
    // shadow resources exist (csmEnabled_ stays false).
    gfx::Camera cam;
    fx.renderer.SetCamera(cam, 16.0f / 9.0f);
    fx.renderer.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 1.0f, 1.0f}, 0.3f);

    gfx::Mesh cube = gfx::Mesh::CreateCube(fx.renderer, 1, 1, 1, "shadow_test_cube");
    CHECK(cube.Valid());
    fx.renderer.DrawMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity());
    fx.renderer.DrawMeshInstanced(cube, gfx::Material::Lit({}), nullptr, 0);
    std::vector<math::Mat4> bones(2, math::Mat4::Identity());
    fx.renderer.DrawSkinnedMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity(), bones, 2);
    CHECK(!fx.renderer.ShadowsEnabled());
}
