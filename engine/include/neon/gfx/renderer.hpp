#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/ibl.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/shader.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// High-level renderer: owns the backend, built-in shaders, lights, fog,
// 3D camera pass and a 2D immediate-mode overlay in "design units".
class Renderer {
public:
    static constexpr int kDesignWidth = 1280;
    static constexpr int kDesignHeight = 720;
    static constexpr int kMaxPointLights = 8;

    Renderer() = default;
    ~Renderer();

    bool Init(platform::IWindow* window);
    void Shutdown();

    // Headless hook used by unit tests and tooling: installs a backend
    // directly, bypassing window/GL-context creation so the CPU-side asset
    // pipeline (mesh/texture upload via CreateMesh/CreateTexture) can run
    // without a GPU. Takes ownership; InitBuiltinResources is not run.
    void AttachBackendForTesting(std::unique_ptr<IRenderBackend> backend);

    IRenderBackend* Backend() { return backend_.get(); }

    // Frame
    void BeginFrame(const Color& clearColor, float clearDepth = 1.0f);
    void EndFrame();
    // Ends the 3D scene phase. In HDR mode this (a) runs bloom + composites the
    // HDR target to the backbuffer and (b) binds the backbuffer so ALL
    // subsequent 2D/HUD draws land there unbloomed and unclamped. Call it right
    // after the last 3D/entity draw and before the HUD (nameplates, minimap,
    // bars, overlays, editor UI). Mid-scene 2D drawn BEFORE EndScene (sky, the
    // ground marker) stays in the HDR target and is bloomed, which is correct.
    // In the legacy non-HDR path EndScene is a no-op (2D already draws straight
    // to the backbuffer). Frames that never call EndScene keep the previous
    // behaviour: EndFrame/CaptureFrame composite + flush everything at once.
    void EndScene();

    // 3D camera
    void SetCamera(const Camera& camera, float aspect);
    const math::Mat4& ViewProjection() const { return viewProj_; }
    const math::Vec3& CameraPosition() const { return camPos_; }

    // Atmosphere / lights
    void SetSky(const Color& top, const Color& horizon);
    void DrawSky();
    void SetFog(const Color& color, float start, float end);
    void SetDirectionalLight(const math::Vec3& direction, const Color& color, float ambientStrength);
    void SetPointLight(int index, const math::Vec3& position, const Color& color, float radius);
    void SetPlayerLight(const math::Vec3& position, const Color& color, float radius);

    // IBL environment lighting (Task 3.8). SetSky procedurally generates a
    // vertical-gradient environment and precomputes irradiance + prefiltered
    // specular + the BRDF LUT on the CPU (gfx/ibl.hpp); the lit shader then
    // samples them for the ambient term. SetIblStrength is the global intensity
    // in [0,1]: 0 keeps the legacy flat `uAmbient` exactly (the `--ibl 0`
    // reference), 1 replaces it with the full environment lighting. Because
    // the demo animates the sky every frame, the environment is recomputed
    // lazily (only when the sky moved enough AND at most once every ~20 SetSky
    // calls) - see RecomputeIbl.
    void SetIblStrength(float strength);
    float IblStrength() const { return iblStrength_; }
    // True once the IBL environment textures exist (first SetSky with a
    // positive strength).
    bool IblValid() const { return iblValid_; }
    // Number of times the IBL environment was actually recomputed (SetSky
    // rebuilds lazily: only when the sky moved enough and the throttle elapsed).
    // Exposed so tests can assert that a recompute really happened.
    uint64_t IblBuildCount() const { return iblBuildCount_; }

    // 3D drawing
    // Cascaded shadow mapping. When enabled, shadow casters are recorded
    // automatically from DrawMesh/DrawSkinnedMesh/DrawMeshInstanced, and the
    // 3 cascade shadow maps are rendered inside SetCamera (before any main
    // pass draws). CSM replaces the CPU projected-contact shadows; when it is
    // unavailable (broken FBO/depth driver, or --disable-fbo) ShadowsEnabled()
    // is false and the game falls back to DrawProjectedShadow.
    void SetShadowsEnabled(bool enabled);
    bool ShadowsEnabled() const { return csmEnabled_; }
    // True when a shadow map pass actually ran this frame (maps are valid).
    bool ShadowMapActive() const { return csmActive_; }
    // Shadow map size in pixels per cascade.
    int ShadowMapSize() const { return shadowSize_; }
    // Point-light (cubemap) shadows. Engage only when the CSM capability
    // self-test passed (same FBO/color-encoded-depth path) and the scene has
    // point lights; the lit shader shadows the first kShadowPointLights point
    // lights by sampling a 6-face depth map computed from the fragment->light
    // direction. Disabled by --no-shadows like CSM.
    bool PointShadowsEnabled() const { return pointShadowsEnabled_; }
    // True when a point-light shadow pass actually ran this frame.
    bool PointShadowMapActive() const { return pointShadowsActive_; }
    // Shadow map size in pixels per face.
    int PointShadowMapSize() const { return kPointShadowSize; }

    // HDR + bloom post-processing (Task 3.6). When the backend supports
    // half-float render targets (self-tested at init), the 3D scene renders
    // into an RGBA16F target at window resolution and EndFrame runs a
    // bright-pass -> blur pyramid -> composite to the backbuffer; the 2D/HUD
    // overlay is then flushed on top. bloomEnabled_ is the user toggle
    // (default on; --no-bloom / NEON_NO_BLOOM disable the bloom term while
    // keeping the same HDR path, so bloom-on vs bloom-off screenshots diff
    // only by the bloom contribution).
    void SetBloomEnabled(bool enabled);
    bool BloomEnabled() const { return bloomEnabled_; }
    // T3.7: the composite applies the ACES fitted tonemapper to
    // ACES((hdr + bloom*strength) * exposure). exposure_ defaults to 1.0
    // (identity); SetExposure lets the editor/T4.7 tune later. tonemapEnabled_
    // keeps the T3.6 `min(c,1)` clamp as the --no-tonemap reference so the
    // tonemap diff isolates only the operator.
    void SetExposure(float exposure);
    float Exposure() const { return exposure_; }
    void SetTonemapEnabled(bool enabled);
    bool TonemapEnabled() const { return tonemapEnabled_; }
    // MSAA (Task 3.7): when requested AND the driver passes the multisample
    // FBO + blit-resolve self-test, the HDR scene renders into a samples-per-
    // pixel multisample target and EndScene/CompositeFrame resolve it into the
    // single-sample bloom source. The bloom pyramid, shadow maps and backbuffer
    // UI stay single-sample. Failing drivers fall back to the single-sample
    // path with a log (and --no-msaa forces the fallback for diffing).
    void SetMsaaEnabled(bool enabled);
    bool MsaaEnabled() const { return msaaEnabled_; }
    // True when the HDR float-target pipeline is active (float RT works and
    // the window-sized target was created). False on drivers without
    // RGBA16F-FBO support: the renderer then draws straight to the backbuffer
    // exactly like before HDR existed.
    bool HdrEnabled() const { return hdrEnabled_; }

    void DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model);
    // Skinned variant: binds the SKINNED lit program and uploads up to 64 bone
    // matrices (from anim::Skeleton::ComputeBoneMatrices). The mesh must be
    // Skinned() (have per-vertex joint ids/weights in its vertex buffer).
    void DrawSkinnedMesh(const Mesh& mesh, const Material& material, const math::Mat4& model,
                         const std::vector<math::Mat4>& boneMatrices, int boneCount);
    void DrawMeshInstanced(const Mesh& mesh, const Material& material, const math::Mat4* models,
                           uint32_t count, bool frustumCull = true);
    // CPU-side projected shadow: projects the mesh onto the ground plane
    // (y=0) along lightDir. Works without any depth buffer or FBO. Used as the
    // fallback when CSM is disabled.
    void DrawProjectedShadow(const Mesh& mesh, const math::Mat4& model,
                             const math::Vec3& lightDir, const Color& color);

    struct LineVertex {
        math::Vec3 pos;
        Color color;
    };
    void DrawLines(const LineVertex* vertices, uint32_t count, const math::Mat4& model);
    void DrawBox(const math::AABB& box, const Color& color);
    void DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments = 20);

    struct RenderStats {
        uint32_t drawCalls = 0;
        uint32_t instances = 0;
        uint32_t triangles = 0;
    };
    const RenderStats& Stats() const { return stats_; }

    // Resources
    Texture CreateTexture(const TextureDesc& desc);
    Shader CreateShader(const char* vertexSource, const char* fragmentSource, const char* name);
    Font CreateFontFromMemory(const uint8_t* data, size_t size, int pixelHeight);
    Font CreateFontFromMemoryWithCodepoints(const uint8_t* data, size_t size, int pixelHeight,
                                            const int32_t* codepoints, int codepointCount);

    // 2D overlay (design units: 1280x720, uniform scale, centered)
    void DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                  TextureHandle texture = {}, const math::Vec2& uv0 = {0.0f, 1.0f},
                  const math::Vec2& uv1 = {1.0f, 0.0f},
                  BlendMode blend = BlendMode::Alpha);
    void DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color);
    void DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color);
    // Filled triangle in design units (same immediate-mode 2D buffer as quads).
    void DrawTriangle2D(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                        const Color& color);
    void DrawText(const Font& font, const std::string& text, const math::Vec2& pos, float size,
                  const Color& color, bool centerX = false, bool centerY = false);
    void DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                       TextureHandle texture, BlendMode blend = BlendMode::Additive);

    // Flushes the batched 2D overlay now (useful to order custom UI before
    // other passes such as a tool overlay rendered directly on the backend).
    void Flush2D();

    math::Vec2 ScreenToUI(const math::Vec2& screenPixels) const;
    // Copies the current back buffer (RGBA8, top-down) into out.
    bool CaptureFrame(std::vector<uint8_t>& out);
    // T3.6 verification helper: composites the current HDR frame twice - once
    // with bloom disabled and once with bloom enabled - and captures both from
    // the SAME HDR target (identical game state, same composite shader, only
    // the bloom term differs). Returns false when the HDR pipeline is inactive.
    bool CaptureBloomComparison(std::vector<uint8_t>& bloomOff,
                                std::vector<uint8_t>& bloomOn);
    // T3.7 verification helper: same-frame ACES tonemap diff. Composites the
    // current HDR frame twice - once with tonemapping disabled (T3.6 clamp
    // reference) and once with ACES + exposure - capturing both from the SAME
    // resolved HDR target. Bloom runs once so the two images differ only by
    // the tone-mapping operator.
    bool CaptureTonemapComparison(std::vector<uint8_t>& clamped,
                                  std::vector<uint8_t>& tonemapped);
    float UIScale() const { return uiScale_; }
    int ScreenWidth() const { return screenW_; }
    int ScreenHeight() const { return screenH_; }

private:
    void InitBuiltinResources();
    void ApplyMaterial(const Material& material, const math::Mat4& mvp, const math::Mat4& model,
                       const math::Mat4& normalMat, ShaderHandle shader);
    math::Vec2 ToScreen(const math::Vec2& design) const;
    void PushQuad(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c, const math::Vec2& d,
                  const Color& color, const math::Vec2& uv0, const math::Vec2& uv1,
                  TextureHandle texture, BlendMode blend);
    void PushQuadColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                         const math::Vec2& d, const Color& ca, const Color& cb, const Color& cc,
                         const Color& cd, const math::Vec2& uv0, const math::Vec2& uv1,
                         TextureHandle texture, BlendMode blend);

    // CSM
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
    // is non-empty: plain meshes use neither, instanced use `models`,
    // skinned use `bones`. bounds is the mesh AABB in object space (used to
    // painter-sort casters since the color-encoded shadow pass has no depth
    // buffer - the window/FBO depth path is broken on some Intel drivers).
    struct ShadowDraw {
        MeshHandle mesh;
        math::Mat4 model;
        std::vector<math::Mat4> models;
        std::vector<math::Mat4> bones;
        int boneCount = 0;
        math::AABB bounds;
    };
    void RunShadowPass();
    void DrawShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP);
    void DrawShadowCastersSorted(const math::Mat4& lightVP);
    bool TestDepthTargetCapability();
    // Point-light cubemap shadows (see RunShadowPass).
    void RunPointShadowPass();
    void DrawPointShadowCastersSorted(int lightIndex);
    void DrawPointShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP,
                               const math::Vec3& lightPos, float range);

    // HDR + bloom post-processing.
    void EnsurePostTargets();
    void DestroyPostTargets();
    bool TestFloatTargetCapability();
    // MSAA: multisample HDR render target + resolve self-test (4x then 2x).
    bool TestMsaaCapability();
    // Binds whichever target the main scene renders into (the HDR float target
    // when active, else the default framebuffer).
    void RebindMainTarget();
    // Resolves the MSAA HDR scene target into the single-sample bloom source
    // (no-op when MSAA is inactive). Called before any pass that samples the
    // HDR target: CompositeSceneToBackbuffer and the capture helpers.
    void ResolveMainTarget();
    // Bloom pyramid (all fullscreen passes on RGBA16F targets). Ends with
    // bloomHalfA_ holding the accumulated bloom at half resolution.
    bool RunBloom();
    // Composite HDR (+ bloom) to the backbuffer with the composite shader.
    void CompositeToBackbuffer();
    // Runs bloom + composite + Flush2D unless the frame was already composited
    // (CaptureFrame composited early so the screenshot is the final image).
    void CompositeFrame();
    // Bloom + composite + Flush2D unconditionally (no compositedThisFrame_
    // latch); used by CompositeFrame and CaptureBloomComparison.
    void CompositeSceneToBackbuffer();

    std::unique_ptr<IRenderBackend> backend_;

    ShaderHandle litShader_;
    ShaderHandle skinnedLitShader_;
    ShaderHandle unlitShader_;
    ShaderHandle uiShader_;
    ShaderHandle linesShader_;
    ShaderHandle litInstancedShader_;
    ShaderHandle unlitInstancedShader_;
    ShaderHandle depthShader_;
    ShaderHandle depthInstancedShader_;
    ShaderHandle depthSkinnedShader_;
    ShaderHandle pointDepthShader_;
    ShaderHandle pointDepthInstancedShader_;
    ShaderHandle pointDepthSkinnedShader_;
    // Post-processing (HDR + bloom).
    ShaderHandle brightPassShader_;
    ShaderHandle blurShader_;
    ShaderHandle downsampleShader_;
    ShaderHandle upsampleAddShader_;
    ShaderHandle compositeShader_;
    TextureHandle white_;
    MeshHandle probeQuadMesh_;
    // Fullscreen NDC quad (uv 0..1) for the post passes.
    MeshHandle postQuadMesh_;
    RenderTargetHandle shadowRT_[kShadowCascades];
    TextureHandle shadowDepthTex_[kShadowCascades];
    int shadowSize_ = kShadowMapSize;
    math::Mat4 lightViewProj_[kShadowCascades];
    float cascadeSplits_[kShadowCascades + 1] = {0.1f, 20.0f, 60.0f, 100.0f};
    bool csmEnabled_ = false;
    bool csmActive_ = false;
    bool shadowsForcedOff_ = false;
    bool shadowPassRanThisFrame_ = false;
    std::vector<ShadowDraw> shadowCasters_;

    RenderTargetHandle pointShadowRT_[kShadowPointLights][6];
    TextureHandle pointShadowDepthTex_[kShadowPointLights][6];
    math::Mat4 pointLightViewProj_[kShadowPointLights][6];
    bool pointShadowsEnabled_ = false;
    bool pointShadowsActive_ = false;

    // HDR scene target (window size, RGBA16F) + the bloom pyramid targets.
    // hdrMsaaRT_ is the 4x/2x multisample target the scene renders into when
    // MSAA is active; hdrRT_ is the single-sample target the MSAA target is
    // resolved into and the source the bloom pyramid + composite sample from.
    RenderTargetHandle hdrRT_;
    RenderTargetHandle hdrMsaaRT_;
    RenderTargetHandle bloomHalfA_;    // 1/2 res: bright pass, then blurred, then accumulated
    RenderTargetHandle bloomHalfB_;    // 1/2 res scratch (blur ping-pong + upsample-add result)
    RenderTargetHandle bloomQuarterA_; // 1/4 res: downsample, then blurred
    RenderTargetHandle bloomQuarterB_; // 1/4 res scratch (blur ping-pong)
    int hdrW_ = 0;
    int hdrH_ = 0;
    bool hdrEnabled_ = false;
    bool bloomEnabled_ = true;
    float exposure_ = 1.0f;
    bool tonemapEnabled_ = true;
    bool msaaRequested_ = true;
    bool msaaEnabled_ = false;
    int msaaSamples_ = 0;
    // True once RunBloom wrote the accumulated bloom into bloomHalfB_ this
    // frame (set by RunBloom, reset in BeginFrame); CompositeToBackbuffer only
    // adds the bloom term when it ran, so a failed shader/target never blends
    // uninitialized content into the composite.
    bool bloomRanThisFrame_ = false;
    bool compositedThisFrame_ = false;

    Camera camera_;
    math::Mat4 viewProj_;
    math::Mat4 view_;
    math::Vec3 camPos_;
    math::Frustum frustum_;
    bool frustumValid_ = false;
    bool depthAvailable_ = true;
    RenderStats stats_;

    Color skyTop_{0.05f, 0.07f, 0.12f, 1.0f};
    Color skyHorizon_{0.2f, 0.3f, 0.45f, 1.0f};
    Color fogColor_{0.2f, 0.3f, 0.45f, 1.0f};
    float fogStart_ = 60.0f;
    float fogEnd_ = 200.0f;

    // IBL environment (Task 3.8). ibl.cpp precomputes the three byte maps
    // (irradiance 1x128, prefiltered 24x128, BRDF LUT 128x128) from the sky
    // gradient; they upload as plain RGBA8 2D textures (units 20..22) and the
    // lit shader samples them with the map conventions documented in ibl.hpp.
    // The environment is recomputed from SetSky when the accumulated sky delta
    // passes a small epsilon and at most once every kIblRecomputeInterval SetSky
    // calls, so the animated day/night sky tracks smoothly without a per-frame
    // hitch; the BRDF LUT is sky-independent and built once.
    static constexpr float kIblGradientPower = 0.65f;
    static constexpr float kIblSkyEpsilon = 0.008f;
    static constexpr uint64_t kIblRecomputeInterval = 20;
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
    void RecomputeIbl(const Color& top, const Color& horizon);

    math::Vec3 sunDir_{-0.4f, -1.0f, -0.3f};
    Color sunColor_{1.0f, 0.95f, 0.85f, 1.0f};
    float ambient_ = 0.25f;
    math::Vec3 pointPos_[kMaxPointLights];
    Color pointColor_[kMaxPointLights];
    float pointRadius_[kMaxPointLights];
    int pointCount_ = 0;
    math::Vec3 playerLightPos_{};
    Color playerLightColor_{1.0f, 0.8f, 0.6f, 1.0f};
    float playerLightRadius_ = 14.0f;
    bool playerLightEnabled_ = false;

    int screenW_ = 1280;
    int screenH_ = 720;
    float uiScale_ = 1.0f;
    float uiOffsetX_ = 0.0f;

    struct UIVertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    std::vector<UIVertex> uiVerts_;
    std::vector<uint16_t> uiIndices_;
    TextureHandle currentUITexture_;
    BlendMode currentUIBlend_ = BlendMode::Alpha;
    platform::IWindow* window_ = nullptr;
};

} // namespace neon::gfx
