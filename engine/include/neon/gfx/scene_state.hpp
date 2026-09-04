#pragma once
#include <cstdint>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/draw_batch2d.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// Scene-state subsystem (split out of Renderer): the active camera + view
// matrices/frustum, the sky/fog parameters, the directional/point/player
// lights and the IBL environment (baked from the sky gradient; see ibl.hpp).
// SetCamera recomputes viewProj_/view_/camPos_/frustum_ that every 3D draw
// consumes; DrawSky paints the vertical gradient into the caller's 2D overlay.
//
// The Renderer facade forwards all public Set*/getters here, reads the state
// back for the per-frame lit-shader scene uniforms (ApplySceneUniforms) and
// the post-graph params (MakePostParams), and hands the point-light arrays to
// ShadowSystem::RunPass. Light/sky/fog/IBl setters that change what the
// uniforms carry bump the shared scene-uniform stamp (SetSceneUniformStamp).
//
// Uses the backend pointer set by SetBackend (Renderer::ConnectSubsystems); in
// the headless test path that is a NullBackend, and the IBL textures it
// creates are fake-but-valid handles exactly like before.
class SceneState {
public:
    static constexpr int kMaxPointLights = 8; // must match Renderer::kMaxPointLights

    SceneState() = default;
    ~SceneState() = default;

    // Reads depthAvailable_ from the backend (Renderer::ConnectSubsystems).
    void SetBackend(IRenderBackend* backend) {
        backend_ = backend;
        depthAvailable_ = backend ? backend->DepthAvailable() : true;
    }
    void SetSceneUniformStamp(uint64_t* stamp) { sceneUniformStamp_ = stamp; }
    void Shutdown(IRenderBackend& backend);

    // 3D camera
    void SetCamera(const Camera& camera, float aspect);
    // NOTE: named ActiveCamera (not Camera) so the type name `Camera` is not
    // shadowed by the member within the class scope.
    const Camera& ActiveCamera() const { return camera_; }
    // Last SetCamera aspect (the shadow cascade frusta match it).
    float ViewAspect() const { return viewAspect_; }
    const math::Mat4& ViewProjection() const { return viewProj_; }
    const math::Mat4& View() const { return view_; }
    const math::Vec3& CamPos() const { return camPos_; }
    const math::Frustum& Frustum() const { return frustum_; }
    bool FrustumValid() const { return frustumValid_; }
    // True when the backend's depth buffer is usable (self-tested at init).
    bool DepthAvailable() const { return depthAvailable_; }

    // Atmosphere / sky
    void SetSky(const Color& top, const Color& horizon);
    void DrawSky(DrawBatch2D& overlay);
    const Color& SkyTop() const { return skyTop_; }
    const Color& SkyHorizon() const { return skyHorizon_; }
    // 写实天空贴图：非空时 DrawSky 用全屏纹理 quad 替代纯色渐变（HDRI
    // tonemapped 天空 JPG），IBL 仍基于 SetSky 的渐变环境色。
    void SetSkyTexture(TextureHandle tex) { skyTexture_ = tex; }
    TextureHandle SkyTexture() const { return skyTexture_; }

    // Fog (linear lit-shader fog + the composite-time volumetric curve).
    void SetFog(const Color& color, float start, float end);
    void SetVolumetricFogEnabled(bool enabled) { volumetricFog_ = enabled; }
    bool VolumetricFogEnabled() const { return volumetricFog_; }
    void SetVolumetricFogDensity(float density) { fogDensity_ = density; }
    float VolumetricFogDensity() const { return fogDensity_; }
    const Color& FogColor() const { return fogColor_; }
    float FogStart() const { return fogStart_; }
    float FogEnd() const { return fogEnd_; }

    // Lights
    void SetDirectionalLight(const math::Vec3& direction, const Color& color,
                             float ambientStrength);
    void SetAmbientLight(const Color& color, float strength);
    // A3 hemisphere ambient: the legacy flat ambient is split into a sky/ground
    // gradient driven by the world normal's Y so upward-facing surfaces take the
    // sky tint and downward-facing take a ground bounce (the biggest cheap
    // indirect-lighting quality step). groundColor defaults to a dark sky-tinted
    // bounce; SetAmbientGroundColor overrides it; "strength" from SetAmbientLight
    // scales both. Empty/invalid ground color falls back to the sky color * 0.5.
    void SetAmbientGroundColor(const Color& color);
    void SetPointLight(int index, const math::Vec3& position, const Color& color, float radius);
    void SetPlayerLight(const math::Vec3& position, const Color& color, float radius);
    const math::Vec3& SunDir() const { return sunDir_; }
    const Color& SunColor() const { return sunColor_; }
    float Ambient() const { return ambient_; }
    const Color& AmbientColor() const { return ambientColor_; }
    const Color& AmbientGroundColor() const { return ambientGroundColor_; }
    const math::Vec3* PointPos() const { return pointPos_; }
    const Color* PointColor() const { return pointColor_; }
    const float* PointRadius() const { return pointRadius_; }
    int PointCount() const { return pointCount_; }
    const math::Vec3& PlayerLightPos() const { return playerLightPos_; }
    const Color& PlayerLightColor() const { return playerLightColor_; }
    float PlayerLightRadius() const { return playerLightRadius_; }
    bool PlayerLightEnabled() const { return playerLightEnabled_; }

    // IBL environment lighting (Task 3.8). SetSky procedurally generates a
    // vertical-gradient environment and precomputes irradiance + prefiltered
    // specular + the BRDF LUT on the CPU (gfx/ibl.hpp). RecomputeIbl is lazy
    // (only when the sky moved enough AND at most once every ~20 SetSky calls).
    void SetIblStrength(float strength);
    float IblStrength() const { return iblStrength_; }
    // True once the IBL environment textures exist (first SetSky with a
    // positive strength).
    bool IblValid() const { return iblValid_; }
    // Number of times the IBL environment was actually recomputed (SetSky
    // rebuilds lazily). Exposed so tests can assert a recompute happened.
    uint64_t IblBuildCount() const { return iblBuildCount_; }
    TextureHandle IblIrradianceTex() const { return iblIrradianceTex_; }
    TextureHandle IblPrefilteredTex() const { return iblPrefilteredTex_; }
    TextureHandle IblBrdfLutTex() const { return iblBrdfLutTex_; }

private:
    static constexpr float kIblGradientPower = 0.65f;
    static constexpr float kIblSkyEpsilon = 0.008f;
    static constexpr uint64_t kIblRecomputeInterval = 20;
    void RecomputeIbl(const Color& top, const Color& horizon);

    IRenderBackend* backend_ = nullptr;
    uint64_t* sceneUniformStamp_ = nullptr;

    Camera camera_;
    math::Mat4 viewProj_;
    math::Mat4 view_;
    math::Vec3 camPos_;
    math::Frustum frustum_;
    bool frustumValid_ = false;
    bool depthAvailable_ = true;
    float viewAspect_ = 16.0f / 9.0f; // last SetCamera aspect (shadow frusta)

    Color skyTop_{0.05f, 0.07f, 0.12f, 1.0f};
    Color skyHorizon_{0.2f, 0.3f, 0.45f, 1.0f};
    TextureHandle skyTexture_;
    Color fogColor_{0.2f, 0.3f, 0.45f, 1.0f};
    float fogStart_ = 60.0f;
    float fogEnd_ = 200.0f;
    bool volumetricFog_ = false;
    float fogDensity_ = 0.02f;

    math::Vec3 sunDir_{-0.4f, -1.0f, -0.3f};
    Color sunColor_{1.0f, 0.95f, 0.85f, 1.0f};
    float ambient_ = 0.25f;
    Color ambientColor_{1.0f, 1.0f, 1.0f, 1.0f};
    Color ambientGroundColor_{0.5f, 0.5f, 0.5f, 1.0f};
    math::Vec3 pointPos_[kMaxPointLights];
    Color pointColor_[kMaxPointLights];
    float pointRadius_[kMaxPointLights];
    int pointCount_ = 0;
    math::Vec3 playerLightPos_{};
    Color playerLightColor_{1.0f, 0.8f, 0.6f, 1.0f};
    float playerLightRadius_ = 14.0f;
    bool playerLightEnabled_ = false;

    // IBL environment (Task 3.8). ibl.cpp precomputes the three byte maps
    // (irradiance 1x128, prefiltered 24x128, BRDF LUT 128x128) from the sky
    // gradient; they upload as plain RGBA8 2D textures (units 20..22) and the
    // lit shader samples them. The environment is recomputed from SetSky when
    // the accumulated sky delta passes a small epsilon and at most once every
    // kIblRecomputeInterval SetSky calls, so the animated day/night sky tracks
    // smoothly without a per-frame hitch; the BRDF LUT is sky-independent and
    // built once.
    TextureHandle iblIrradianceTex_;
    TextureHandle iblPrefilteredTex_;
    TextureHandle iblBrdfLutTex_;
    bool iblValid_ = false;
    bool iblBrdfLutReady_ = false;
    float iblStrength_ = 1.0f;
    Color iblLastTop_{};
    Color iblLastHorizon_{};
    float iblAccumDelta_ = 0.0f;
    uint64_t iblFrameCounter_ = 0;
    uint64_t iblLastRecomputeFrame_ = 0;
    uint64_t iblBuildCount_ = 0;
};

} // namespace neon::gfx
