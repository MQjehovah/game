#pragma once
#include <cstdint>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// Cascaded + point-light shadow subsystem (split out of Renderer). Owns the
// colour-encoded depth shaders, the 3 cascade + 2x6 point-light render targets,
// the recorded caster list and the per-frame passes. The Renderer facade:
//   - collects casters from the draw paths (RecordCaster, when Recording() &&
//     Enabled() && the material is opaque),
//   - triggers the pass with RunPass (from SetCamera / RefreshShadowPass),
//   - reads the resulting maps / splits back for the lit shader's per-frame
//     scene uniforms (LightViewProj/CascadeSplits/ShadowDepthTex/...).
//
// RunPass takes the current camera / aspect / sun direction plus the scene's
// point lights (positions + radii) as parameters - the shadow system owns no
// scene state. It also bumps the renderer's per-frame scene-uniform stamp
// (SetSceneUniformStamp) when a point-light pass actually ran, matching the
// old `++sceneUniformStamp_` in RunPointShadowPass.
class ShadowSystem {
public:
    static constexpr int kShadowCascades = 3;
    static constexpr int kShadowMapSize = 1024;
    // Point-light shadows: the first kShadowPointLights point lights each get 6
    // faces (2D maps; layered cubemap FBOs are unreliable on the tested Intel
    // driver). The lit shader computes the face + uv from the fragment->light
    // direction and samples the matching 2D map.
    static constexpr int kShadowPointLights = 2;
    static constexpr int kPointShadowSize = 512;
    static constexpr float kPointShadowNear = 0.1f;

    // One recorded shadow caster per 3D draw call. Exactly one of models/bones
    // is non-empty: plain meshes use neither, instanced use `models`, skinned
    // use `bones`. bounds is the mesh AABB in object space (used to
    // painter-sort casters since the colour-encoded shadow pass has no depth
    // buffer - the window/FBO depth path is broken on some Intel drivers).
    struct ShadowDraw {
        MeshHandle mesh;
        math::Mat4 model;
        std::vector<math::Mat4> models;
        std::vector<math::Mat4> bones;
        int boneCount = 0;
        math::AABB bounds;
    };
    // Painter's-order key used to sort shadow casters (the colour-encoded
    // shadow pass has no depth buffer). Stored in a member vector so the
    // per-frame shadow passes do not allocate.
    struct ShadowSortKey {
        const ShadowDraw* draw;
        float z;
    };

    ShadowSystem() = default;
    ~ShadowSystem() = default;

    void SetBackend(IRenderBackend* backend) { backend_ = backend; }
    void SetSceneUniformStamp(uint64_t* stamp) { sceneUniformStamp_ = stamp; }
    // Creates the depth/point-depth shaders + the shadow render targets and
    // runs the FBO capability self-tests. Gated on SetShadowsEnabled(false)
    // having been called (shadowsForcedOff_): the shaders are always created,
    // but the targets + capability test are skipped, leaving csmEnabled_/
    // pointShadowsEnabled_ false (CPU projected-shadow fallback). `probeQuad`
    // (NDC unit quad) and `unlitShader` (flat program) are owned by the
    // Renderer and only used by the self-test.
    void Init(IRenderBackend& backend, MeshHandle probeQuad, ShaderHandle unlitShader);
    void Shutdown(IRenderBackend& backend);
    // Frame reset (Renderer::BeginFrame): clears the "maps valid" latches.
    void BeginFrame();

    // Shadow enablement + caster recording.
    void SetShadowsEnabled(bool enabled);
    bool Enabled() const { return csmEnabled_; }
    void SetShadowRecording(bool enabled) { shadowRecording_ = enabled; }
    bool Recording() const { return shadowRecording_; }
    // True when a shadow pass already ran for this frame (SetCamera checks it
    // to avoid running the pass twice per frame).
    bool ShadowPassRanThisFrame() const { return shadowPassRanThisFrame_; }
    void RecordCaster(const ShadowDraw& draw) { shadowCasters_.push_back(draw); }

    // Re-runs the cascade + point-light shadow passes for `camera`. Called from
    // Renderer::SetCamera (once per frame) and Renderer::RefreshShadowPass
    // (always, for a changed render camera). Consumes and clears the recorded
    // casters.
    void RunPass(const Camera& camera, float aspect, const math::Vec3& sunDir,
                 const math::Vec3* pointPos, const float* pointRadius, int pointCount);

    // Public API forwards + per-frame scene-uniform reads.
    bool CsmActive() const { return csmActive_; }
    int ShadowSize() const { return shadowSize_; }
    bool PointShadowsEnabled() const { return pointShadowsEnabled_; }
    bool PointShadowsActive() const { return pointShadowsActive_; }
    const math::Mat4* LightViewProj() const { return lightViewProj_; }
    const float* CascadeSplits() const { return cascadeSplits_; }
    const TextureHandle* ShadowDepthTex() const { return shadowDepthTex_; }
    const TextureHandle* PointShadowDepthTex() const { return &pointShadowDepthTex_[0][0]; }
    // The skinned depth program is shared with the SSAO depth pre-pass in the
    // Renderer (DrawSsaoDepthCasters), so it is exposed here.
    ShaderHandle SkinnedDepthShader() const { return depthSkinnedShader_; }

private:
    void DrawShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP);
    void DrawShadowCastersSorted(const math::Mat4& lightVP);
    void RunPointShadowPass(const math::Vec3* pointPos, const float* pointRadius, int pointCount);
    void DrawPointShadowCastersSorted(int lightIndex, const math::Vec3& lightPos, float range);
    void DrawPointShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP,
                               const math::Vec3& lightPos, float range);
    bool TestDepthTargetCapability();

    IRenderBackend* backend_ = nullptr;
    uint64_t* sceneUniformStamp_ = nullptr;
    ShaderHandle depthShader_;
    ShaderHandle depthInstancedShader_;
    ShaderHandle depthSkinnedShader_;
    ShaderHandle pointDepthShader_;
    ShaderHandle pointDepthInstancedShader_;
    ShaderHandle pointDepthSkinnedShader_;
    MeshHandle probeQuad_;
    ShaderHandle unlitShader_;

    RenderTargetHandle shadowRT_[kShadowCascades];
    TextureHandle shadowDepthTex_[kShadowCascades];
    int shadowSize_ = kShadowMapSize;
    math::Mat4 lightViewProj_[kShadowCascades];
    float cascadeSplits_[kShadowCascades + 1] = {0.1f, 20.0f, 60.0f, 100.0f};
    bool csmEnabled_ = false;
    bool csmActive_ = false;
    bool shadowsForcedOff_ = false;
    bool shadowPassRanThisFrame_ = false;
    bool shadowRecording_ = true;
    std::vector<ShadowDraw> shadowCasters_;

    RenderTargetHandle pointShadowRT_[kShadowPointLights][6];
    TextureHandle pointShadowDepthTex_[kShadowPointLights][6];
    math::Mat4 pointLightViewProj_[kShadowPointLights][6];
    bool pointShadowsEnabled_ = false;
    bool pointShadowsActive_ = false;

    // Reusable per-frame scratch buffers (kept out of the hot path's per-call
    // allocations): painter's-order sort keys + flattened bone matrices.
    std::vector<ShadowSortKey> shadowSortKeys_;
    std::vector<float> boneUniformFlat_;
};

} // namespace neon::gfx
