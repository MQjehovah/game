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

// ---------------------------------------------------------------------------
// Point-light cubemap shadow math (pure, headless - no GL needed).
// ---------------------------------------------------------------------------

TEST(PointCubemapFaceSelection) {
    float u = 0.0f, v = 0.0f;

    // Cardinal axes: each picks its face and lands dead-center (uv 0.5,0.5).
    CHECK_EQ(gfx::CubemapFaceAndUV({1, 0, 0}, u, v), 0);
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_NEAR(v, 0.5, 1e-5);
    CHECK_EQ(gfx::CubemapFaceAndUV({-1, 0, 0}, u, v), 1);
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_EQ(gfx::CubemapFaceAndUV({0, 1, 0}, u, v), 2);
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_EQ(gfx::CubemapFaceAndUV({0, -1, 0}, u, v), 3);
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_EQ(gfx::CubemapFaceAndUV({0, 0, 1}, u, v), 4);
    CHECK_NEAR(u, 0.5, 1e-5);
    CHECK_EQ(gfx::CubemapFaceAndUV({0, 0, -1}, u, v), 5);
    CHECK_NEAR(u, 0.5, 1e-5);

    // Off-axis directions: verify the projected coordinates per face
    // (GL cube-map spec convention: +X: u = -z/x, +Z: u = +x/z, etc).
    math::Vec3 d1 = math::Vec3{1, 0, 0.5f}.Normalized(); // +X face
    CHECK_EQ(gfx::CubemapFaceAndUV(d1, u, v), 0);
    CHECK_NEAR(u, 0.5f - 0.25f, 1e-4); // -z/x * 0.5 + 0.5 = 0.25
    CHECK_NEAR(v, 0.5, 1e-4);
    math::Vec3 d2 = math::Vec3{1, 0, -0.5f}.Normalized(); // +X face
    CHECK_EQ(gfx::CubemapFaceAndUV(d2, u, v), 0);
    CHECK_NEAR(u, 0.75, 1e-4);
    math::Vec3 d3 = math::Vec3{0.5f, 0, 1}.Normalized(); // +Z face
    CHECK_EQ(gfx::CubemapFaceAndUV(d3, u, v), 4);
    CHECK_NEAR(u, 0.75, 1e-4);
    math::Vec3 d4 = math::Vec3{-0.5f, 0, 1}.Normalized(); // +Z face
    CHECK_EQ(gfx::CubemapFaceAndUV(d4, u, v), 4);
    CHECK_NEAR(u, 0.25, 1e-4);
    math::Vec3 d5 = math::Vec3{0, 1, 0.5f}.Normalized(); // +Y face
    CHECK_EQ(gfx::CubemapFaceAndUV(d5, u, v), 2);
    CHECK_NEAR(v, 0.75, 1e-4); // z/y * 0.5 + 0.5
    math::Vec3 d6 = math::Vec3{0, -1, 0.5f}.Normalized(); // -Y face
    CHECK_EQ(gfx::CubemapFaceAndUV(d6, u, v), 3);
    CHECK_NEAR(v, 0.25, 1e-4); // -z/|y| * 0.5 + 0.5
}

TEST(PointCubemapFaceProjectionMatchesRender) {
    // A point that lies inside face f's 90-degree frustum must project to the
    // same uv via the face view-projection as CubemapFaceAndUV computes from
    // its direction. This is what guarantees the depth map rendered per face
    // is sampled at the right texel in the lit shader.
    const math::Vec3 lightPos = {1.5f, 2.0f, -0.5f};
    const float nearZ = 0.1f;
    const float farZ = 10.0f;
    const math::Vec3 samples[6][4] = {
        {{1, 0.3f, 0.2f}, {1, -0.4f, 0.1f}, {1, 0.2f, -0.5f}, {1, -0.1f, 0.4f}},   // +X
        {{-1, 0.3f, 0.2f}, {-1, -0.4f, 0.1f}, {-1, 0.2f, -0.5f}, {-1, -0.1f, 0.4f}}, // -X
        {{0.3f, 1, 0.2f}, {-0.4f, 1, 0.1f}, {0.2f, 1, -0.5f}, {-0.1f, 1, 0.4f}},    // +Y
        {{0.3f, -1, 0.2f}, {-0.4f, -1, 0.1f}, {0.2f, -1, -0.5f}, {-0.1f, -1, 0.4f}}, // -Y
        {{0.3f, 0.2f, 1}, {-0.4f, 0.1f, 1}, {0.2f, -0.5f, 1}, {-0.1f, 0.4f, 1}},     // +Z
        {{0.3f, 0.2f, -1}, {-0.4f, 0.1f, -1}, {0.2f, -0.5f, -1}, {-0.1f, 0.4f, -1}},  // -Z
    };
    for (int face = 0; face < 6; ++face) {
        const math::Mat4 vp = gfx::ComputePointLightFaceViewProj(lightPos, face, nearZ, farZ);
        for (int s = 0; s < 4; ++s) {
            const math::Vec3 dir = samples[face][s].Normalized();
            const math::Vec3 world = lightPos + dir * 5.0f;
            const math::Vec4 clip = vp.TransformVec4({world.x, world.y, world.z, 1.0f});
            CHECK(clip.w > 0.0f);
            const float uProj = clip.x / clip.w * 0.5f + 0.5f;
            const float vProj = clip.y / clip.w * 0.5f + 0.5f;
            float u = 0.0f, v = 0.0f;
            CHECK_EQ(gfx::CubemapFaceAndUV(dir, u, v), face);
            CHECK_NEAR(u, uProj, 1e-3);
            CHECK_NEAR(v, vProj, 1e-3);
            // The projected depth is inside the rendered range.
            const float ndcZ = clip.z / clip.w;
            CHECK(ndcZ >= -1.0f && ndcZ <= 1.0f);
        }
    }
}

TEST(PointLightShadowDepthCompare) {
    // Stored depth is dist/range in [0,1]; a fragment is lit when the nearest
    // stored surface is farther than itself (minus bias).
    CHECK_NEAR(gfx::PointLightShadowFactor(0.5f, 4.0f, 10.0f, 0.01f), 1.0f, 1e-6);
    CHECK_NEAR(gfx::PointLightShadowFactor(0.5f, 6.0f, 10.0f, 0.01f), 0.0f, 1e-6);
    // Bias rescues a surface grazing the stored depth.
    CHECK_NEAR(gfx::PointLightShadowFactor(0.5f, 5.2f, 10.0f, 0.1f), 1.0f, 1e-6);
    CHECK_NEAR(gfx::PointLightShadowFactor(0.5f, 5.2f, 10.0f, 0.01f), 0.0f, 1e-6);
    // Range <= 0 (disabled light) never shadows.
    CHECK_NEAR(gfx::PointLightShadowFactor(0.2f, 1.0f, 0.0f, 0.01f), 1.0f, 1e-6);
}

TEST(PointShadowNullBackendStaysDisabled) {
    test::HeadlessAssetFixture fx;
    // Same capability gate as CSM: without a real backend the point-light
    // shadow maps are never allocated and the flag stays off.
    CHECK(!fx.renderer.PointShadowsEnabled());
    CHECK(!fx.renderer.PointShadowMapActive());
    CHECK_EQ(fx.renderer.PointShadowMapSize(), 512);

    fx.renderer.SetPointLight(0, {0.0f, 1.3f, 0.0f}, {1.0f, 0.55f, 0.25f, 1.0f}, 11.0f);

    // SetCamera triggers the (disabled) shadow passes; DrawMesh must not crash
    // even though no point shadow resources exist.
    gfx::Camera cam;
    fx.renderer.SetCamera(cam, 16.0f / 9.0f);
    gfx::Mesh cube = gfx::Mesh::CreateCube(fx.renderer, 1, 1, 1, "point_shadow_test_cube");
    CHECK(cube.Valid());
    fx.renderer.DrawMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity());
    CHECK(!fx.renderer.PointShadowsEnabled());
    CHECK(!fx.renderer.PointShadowMapActive());

    // Force-disabling shadows keeps the capability off.
    fx.renderer.SetShadowsEnabled(false);
    CHECK(!fx.renderer.ShadowsEnabled());
    CHECK(!fx.renderer.PointShadowsEnabled());
}
