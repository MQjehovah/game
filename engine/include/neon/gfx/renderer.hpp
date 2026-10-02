#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/bloom.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/draw_batch2d.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/ibl.hpp"
#include "neon/gfx/light_probe.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/post_graph.hpp"
#include "neon/gfx/scene_state.hpp"
#include "neon/gfx/threaded_backend.hpp"
#include "neon/gfx/upload_thread.hpp"
#include "neon/gfx/shader.hpp"
#include "neon/gfx/shadow_system.hpp"
#include "neon/gfx/taa.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {


// High-level renderer: owns the backend, built-in shaders, and delegates to
// three composition services - ShadowSystem (CSM + point-light shadows),
// SceneState (camera/lights/sky/fog/IBL) and DrawBatch2D (immediate-mode 2D
// overlay + billboards) - plus the unified post-processing FrameGraph
// (postGraph_). The Renderer is a FACADE: every public method forwards to the
// service that owns the state, and the facade itself wires the shared state
// between them (the scene-uniform stamp, the shadow maps bound by the lit
// shader, the scene viewport rect, ...). The public API is unchanged from
// before the split, so callers (DrawSystem/editor/tests) are untouched.
class Renderer {
public:
    static constexpr int kDesignWidth = 1280;
    static constexpr int kDesignHeight = 720;
    static constexpr int kMaxPointLights = 8; // must match SceneState::kMaxPointLights

    Renderer() = default;
    ~Renderer();

    bool Init(platform::IWindow* window);
    void Shutdown();

    // G4: the terrain splatmap shader variant (grass texture + dirt/rock colors
    // blended by the vertex splat weights). Terrain chunks set this on their
    // material so the Draw path selects it instead of the plain lit shader.
    ShaderHandle TerrainShader() const { return terrainShader_; }

    // Selects the graphics backend before Init. "gl" (default) uses OpenGL;
    // "vulkan" uses the Vulkan backend when built with NEON_ENABLE_VULKAN=ON
    // (falls back to OpenGL with a log otherwise).
    void SetBackendName(const std::string& name) { backendName_ = name; }
    // Runs the GL backend on a dedicated render thread (command-marshaled via
    // ThreadedBackend) instead of the calling thread. Set BEFORE Init(). See
    // gfx/threaded_backend.hpp.
    void SetRenderThreadEnabled(bool enabled) { renderThreadEnabled_ = enabled; }
    bool RenderThreadEnabled() const { return renderThreadEnabled_; }

    // Headless hook used by unit tests and tooling: installs a backend
    // directly, bypassing window/GL-context creation so the CPU-side asset
    // pipeline (mesh/texture upload via CreateMesh/CreateTexture) can run
    // without a GPU. Takes ownership; InitBuiltinResources is not run.
    void AttachBackendForTesting(std::unique_ptr<IRenderBackend> backend);

    IRenderBackend* Backend() { return backend_.get(); }
    // Background GPU-upload worker (shared GL context); null when the platform
    // has no shared-context support (uploads stay on the main thread).
    UploadThread* Uploads() { return uploadThread_.get(); }

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
    // Re-runs the cascade shadow pass for the CURRENT camera, even if one was
    // already run this frame (e.g. the editor pre-ran it with its free/orbit
    // camera before the play runtime resolved the game camera). Frames that
    // render through two cameras with different views need the shadow maps
    // recomputed for the ACTUAL render camera.
    void RefreshShadowPass();
    const math::Mat4& ViewProjection() const { return sceneState_.ViewProjection(); }
    const math::Vec3& CameraPosition() const { return sceneState_.CamPos(); }
    // Frustum of the active camera (valid after SetCamera). Exposed so callers
    // that pre-cull with a spatial index before instanced draws use the exact
    // same test the renderer would.
    const math::Frustum& ViewFrustum() const { return sceneState_.Frustum(); }

    // Atmosphere / lights
    void SetSky(const Color& top, const Color& horizon);
    void SetSkyTexture(TextureHandle tex) { sceneState_.SetSkyTexture(tex); }
    void DrawSky();
    // A4 procedural skybox overrides the static gradient: a view-ray sky with a
    // sun disc + halo, a moon, and procedural clouds. sunYaw/sunPitch in
    // radians; set enabled=true to replace the plain gradient. Defaults fill a
    // healthy daytime look so callers opt-in with one line.
    struct SkyBoxParams {
        bool enabled = false;
        float sunYaw = 0.6f;
        float sunPitch = 0.8f;      // above horizon
        bool sunVisible = true;
        bool moonVisible = true;
        bool cloudsEnabled = true;
        float cloudCoverage = 0.35f;
        float cloudScale = 2.0f;
    };
    void SetSkyBox(const SkyBoxParams& params) { skyBox_ = params; }
    const SkyBoxParams& SkyBox() const { return skyBox_; }
    // Convenience: enable the procedural skybox and aim the sun/moon from a
    // world-space sun direction (the same dir the directional light uses), so a
    // scene's DirectionalLight drives the sky disc placement automatically.
    void EnableSkyBox(const math::Vec3& sunDir, bool clouds = true);
    void SetFog(const Color& color, float start, float end);
    // Volumetric exponential distance fog applied at composite time (reads the
    // scene depth). Off by default; densifies with distance independent of the
    // lit shader's linear fog. SetVolumetricFogDensity sets the curve rate.
    void SetVolumetricFogEnabled(bool enabled) { sceneState_.SetVolumetricFogEnabled(enabled); }
    bool VolumetricFogEnabled() const { return sceneState_.VolumetricFogEnabled(); }
    void SetVolumetricFogDensity(float density) { sceneState_.SetVolumetricFogDensity(density); }
    float VolumetricFogDensity() const { return sceneState_.VolumetricFogDensity(); }
    void SetDirectionalLight(const math::Vec3& direction, const Color& color, float ambientStrength);
    // Set the flat ambient term used by the lit shader. `color` tints the
    // ambient and `strength` scales it; a non-default color lets an explicit
    // ambient light object control the scene's base fill independently of the
    // sky-based IBL environment.
    void SetAmbientLight(const Color& color, float strength);
    // A3 hemisphere ambient: ground-bounce color for downward-facing surfaces
    // (sky color still sets the up-facing tint via SetAmbientLight). Defaults to
    // a dark sky-tinted bounce; override per scene or leave default.
    void SetAmbientGroundColor(const Color& color) { sceneState_.SetAmbientGroundColor(color); }
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
    // calls) - see SceneState::RecomputeIbl.
    void SetIblStrength(float strength);
    float IblStrength() const { return sceneState_.IblStrength(); }
    // A3 probe-field GI: bind a baked 2D irradiance atlas (light_probe.hpp's
    // BakeProbeAtlas) and sample it in the lit shader for indirect diffuse,
    // blended into the IBL ambient. Empty/invalid texture disables the GI term.
    // `bounds` is the world AABB the probe grid spans, `res` the grid dims,
    // `maxIrradiance` the value that maps to 1.0 in the LDR atlas.
    void SetLightProbes(TextureHandle atlas, const math::AABB& bounds, int res,
                        float maxIrradiance);
    // A3 convenience: build a probe field from `input` spread over `bounds`,
    // bake it to an LDR 2D atlas, upload a texture and call SetLightProbes. A
    // single entry point for the scene host / editor / test to enable probe GI
    // without touching texture plumbing. res is the grid resolution per axis.
    // Returns false when the field/atlas is empty (nothing baked).
    bool BakeLightProbes(const math::AABB& bounds, int res, const ProbeLightInput& input);
    // True once the IBL environment textures exist (first SetSky with a
    // positive strength).
    bool IblValid() const { return sceneState_.IblValid(); }
    // Number of times the IBL environment was actually recomputed (SetSky
    // rebuilds lazily: only when the sky moved enough and the throttle elapsed).
    // Exposed so tests can assert that a recompute really happened.
    uint64_t IblBuildCount() const { return sceneState_.IblBuildCount(); }

    // 3D drawing
    // Cascaded shadow mapping. When enabled, shadow casters are recorded
    // automatically from DrawMesh/DrawSkinnedMesh/DrawMeshInstanced, and the
    // 3 cascade shadow maps are rendered inside SetCamera (before any main
    // pass draws). CSM replaces the CPU projected-contact shadows; when it is
    // unavailable (broken FBO/depth driver, or --disable-fbo) ShadowsEnabled()
    // is false and the game falls back to DrawProjectedShadow.
    void SetShadowsEnabled(bool enabled);
    bool ShadowsEnabled() const { return shadowSystem_.Enabled(); }
    // G1-5 screen-space ambient occlusion. Off by default (and on drivers where
    // the colour-encoded depth pass or AO targets fail); a no-op when disabled,
    // so the composite output is unchanged. SSAO uses a depth pre-pass that
    // re-renders opaque casters into a colour-encoded linear depth target
    // (FBO depth TEXTUREs are unreliable on the same drivers that made shadows
    // use colour-encoded depth), then AO + blur, multiplied into the composite.
    void SetSsaoEnabled(bool enabled) { ssaoEnabled_ = enabled; }
    bool SsaoEnabled() const { return ssaoEnabled_; }
    void SetSsaoIntensity(float intensity) { ssaoIntensity_ = intensity; }
    float SsaoIntensity() const { return ssaoIntensity_; }
    // G1-5 screen-space volumetric light shafts (god rays). Off by default; a
    // cheap radial accumulation toward the sun over the HDR scene colour.
    void SetVolumetricEnabled(bool enabled) { volumetricEnabled_ = enabled; }
    bool VolumetricEnabled() const { return volumetricEnabled_; }
    void SetVolumetricIntensity(float intensity) { volumetricIntensity_ = intensity; }
    float VolumetricIntensity() const { return volumetricIntensity_; }
    // G1-5 screen-space reflections. Off by default; ray-marches the reflected
    // view ray in screen space against the scene depth, pulling the HDR colour.
    void SetSsrEnabled(bool enabled) { ssrEnabled_ = enabled; }
    bool SsrEnabled() const { return ssrEnabled_; }
    void SetSsrIntensity(float intensity) { ssrIntensity_ = intensity; }
    float SsrIntensity() const { return ssrIntensity_; }
    // Editor tooling: temporarily suppress shadow-caster recording so a mesh
    // rendered into its own offscreen target (e.g. an asset thumbnail) never
    // pollutes the main scene's shadow pass. Enabled by default.
    void SetShadowRecording(bool enabled) { shadowSystem_.SetShadowRecording(enabled); }
    bool ShadowRecording() const { return shadowSystem_.Recording(); }
    // True when a shadow map pass actually ran this frame (maps are valid).
    bool ShadowMapActive() const { return shadowSystem_.CsmActive(); }
    // Shadow map size in pixels per cascade.
    int ShadowMapSize() const { return shadowSystem_.ShadowSize(); }
    // Point-light (cubemap) shadows. Engage only when the CSM capability
    // self-test passed (same FBO/color-encoded-depth path) and the scene has
    // point lights; the lit shader shadows the first kShadowPointLights point
    // lights by sampling a 6-face depth map computed from the fragment->light
    // direction. Disabled by --no-shadows like CSM.
    bool PointShadowsEnabled() const { return shadowSystem_.PointShadowsEnabled(); }
    // True when a point-light shadow pass actually ran this frame.
    bool PointShadowMapActive() const { return shadowSystem_.PointShadowsActive(); }
    // Shadow map size in pixels per face.
    int PointShadowMapSize() const { return ShadowSystem::kPointShadowSize; }
    // True when the backend's depth buffer is usable (self-tested at init).
    // Callers that rely on depth-tested draw order (e.g. instanced batching of
    // opaque scene entities) must fall back to per-entity draws when false:
    // without a depth buffer the scene is painter's-sorted, so batching would
    // change the result.
    bool DepthTestAvailable() const { return sceneState_.DepthAvailable(); }

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
    // A/RenderStack: data-driven bloom threshold + add-back strength (defaults to
    // the original constants). The RenderStack component drives these each frame.
    void SetBloomParams(float threshold, float strength);
    float BloomThreshold() const { return bloomThreshold_; }
    float BloomStrength() const { return bloomStrength_; }
    // Upsample/bloom filter footprint: 1 = the classic narrow upsample, > 1
    // widens the glow skirt around bright pixels (additive VFX read as a real
    // bloom instead of a local blur). Data-driven via RenderStack.bloomWidth.
    void SetBloomWidth(float width);
    float BloomWidth() const { return bloomWidth_; }
    // Shadow quality controls (CSM). shadowDistance clamps the cascades to a
    // world range (0 = the camera far plane), which is the single biggest
    // texel-density win for an overhead camera. softness scales the PCSS kernel
    // radius (0 falls back to the legacy 2x2 comparison), and normalOffset
    // shifts receivers along their normal by that many cascade texels.
    void SetShadowDistance(float distance) { shadowSystem_.SetShadowDistance(distance); }
    float ShadowDistance() const { return shadowSystem_.ShadowDistance(); }
    void SetShadowSoftness(float softness) { shadowSoftness_ = softness > 0.0f ? softness : 0.0f; }
    float ShadowSoftness() const { return shadowSoftness_; }
    void SetShadowNormalOffset(float texels) { shadowNormalOffset_ = texels; }
    float ShadowNormalOffset() const { return shadowNormalOffset_; }
    void SetCascadeLambda(float lambda) { shadowSystem_.SetCascadeLambda(lambda); }
    // T3.7: the composite applies the ACES fitted tonemapper to
    // ACES((hdr + bloom*strength) * exposure). exposure_ defaults to 1.0
    // (identity); SetExposure lets the editor/T4.7 tune later. tonemapEnabled_
    // keeps the T3.6 `min(c,1)` clamp as the --no-tonemap reference so the
    // tonemap diff isolates only the operator.
    void SetExposure(float exposure);
    float Exposure() const { return exposure_; }
    void SetTonemapEnabled(bool enabled);
    bool TonemapEnabled() const { return tonemapEnabled_; }
    // A1 color grading: procedural (LUT-free) post-tonemap grade driving the
    // composite. SetColorGrade copies the params; the RenderStack component (or
    // a Lua-driven script) applies it each frame so grading is data-driven.
    void SetColorGrade(const ColorGrade& grade) { colorGrade_ = grade; }
    const ColorGrade& GetColorGrade() const { return colorGrade_; }
    // A5 auto-exposure + vignette setters (composite post-tonemap). autoExposure
    // adapts the exposure to the scene's average luminance; vignette darkens the
    // frame corners. Both off by default -> existing scenes unchanged.
    void SetAutoExposure(const AutoExposure& ae) { autoExposure_ = ae; }
    const AutoExposure& GetAutoExposure() const { return autoExposure_; }
    // 丢弃自动曝光的适应历史（场景加载/切换时调用）：下一个 AE 帧从种子 1.0
    // 重新收敛，而不是从上一个场景的适应亮度漂移过来。
    void ResetAutoExposure();
    void SetVignette(const Vignette& v) { vignette_ = v; }
    const Vignette& GetVignette() const { return vignette_; }
    // MSAA (Task 3.7): when requested AND the driver passes the multisample
    // FBO + blit-resolve self-test, the HDR scene renders into a samples-per-
    // pixel multisample target and EndScene/CompositeFrame resolve it into the
    // single-sample bloom source. The bloom pyramid, shadow maps and backbuffer
    // UI stay single-sample. Failing drivers fall back to the single-sample
    // path with a log (and --no-msaa forces the fallback for diffing).
    void SetMsaaEnabled(bool enabled);
    bool MsaaEnabled() const { return msaaEnabled_; }
    // Multisample count requested when MSAA is on (2/4/8). Changing it rebuilds
    // the HDR targets at the next BeginFrame. Falls back to a lower supported
    // count when the driver rejects it.
    void SetMsaaSamples(int samples);
    int MsaaSamples() const { return msaaSamples_; }

    // --- Quality / scalability (Step D) -------------------------------------
    // One preset that drives MSAA, shadow resolution/reach/softness, point
    // shadows, the screen-space effects (SSAO / volumetric / SSR) and the render
    // resolution scale. Individual setters above stay authoritative, so a
    // script can apply a preset and then override any single knob.
    enum class Quality { Low, Medium, High, Ultra };
    void SetQuality(Quality quality);
    Quality GetQuality() const { return quality_; }
    const char* QualityName() const;
    // Render resolution scale in [0.4, 1.0]. Below 1 the whole 3D scene + post
    // chain render into a smaller HDR target and the composite upscales to the
    // window (bilinear). Above 1 supersamples (SSAA). This is the main GPU lever
    // on fill-rate-bound mobile/Intel parts.
    void SetRenderScale(float scale);
    float RenderScale() const { return renderScale_; }
    // Dynamic resolution: nudges the effective scale between `minScale` and
    // RenderScale() to hold `targetFps`, from an EMA of the measured frame time.
    // Applied only to the full-window scene (a custom scene viewport pins it).
    void SetDynamicResolution(bool enabled, float targetFps = 60.0f, float minScale = 0.6f);
    bool DynamicResolution() const { return dynResEnabled_; }
    float DynamicScale() const { return dynScale_; }
    // --- Temporal AA (Step E) -----------------------------------------------
    // Renders the scene with a per-frame sub-pixel camera jitter and resolves
    // the result against an accumulated history buffer (camera-only
    // reprojection + neighbourhood clamp). Needs the resolved main-pass depth,
    // so it engages on the MSAA path and silently no-ops otherwise.
    void SetTaaEnabled(bool enabled);
    bool TaaEnabled() const { return taaEnabled_; }
    // Weight of the CURRENT frame in the resolve (0.05 = heavy accumulation,
    // 0.3 = little). Higher values trade smoothness for less ghosting.
    void SetTaaBlend(float blend);
    float TaaBlend() const { return taaBlend_; }
    // High-pass gain of the unsharp mask run after the resolve (0 = off). The
    // accumulation is what removes edge aliasing, but it also averages away
    // texture detail; this puts a controllable slice of it back.
    void SetTaaSharpen(float amount);
    float TaaSharpen() const { return taaSharpen_; }
    bool TaaActive() const { return taaOutput_.Valid(); }
    // Particle budget hint. The renderer does not own the gameplay particle
    // system (GameRuntime does); the runtime polls this and applies it, so a
    // quality preset also scales VFX density.
    size_t ParticleBudget() const { return particleBudget_; }
    // True when the HDR float-target pipeline is active (float RT works and
    // the window-sized target was created). False on drivers without
    // RGBA16F-FBO support: the renderer then draws straight to the backbuffer
    // exactly like before HDR existed.
    bool HdrEnabled() const { return hdrEnabled_; }

    void DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model);
    // Step E2 velocity variant: `prevModel` is the same object's model matrix
    // LAST frame. When it differs from `model` and temporal AA is on, the mesh is
    // also rasterized into the velocity buffer so the TAA resolve reprojects it
    // with a real motion vector instead of the camera-only fallback. Pass null
    // (or the default overload) for static geometry.
    void DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model,
                  const math::Mat4* prevModel);
    // Skinned variant: binds the SKINNED lit program and uploads up to 64 bone
    // matrices (from anim::Skeleton::ComputeBoneMatrices). The mesh must be
    // Skinned() (have per-vertex joint ids/weights in its vertex buffer).
    void DrawSkinnedMesh(const Mesh& mesh, const Material& material, const math::Mat4& model,
                         const std::vector<math::Mat4>& boneMatrices, int boneCount);
    // Velocity variant. The motion vector is taken from the ENTITY transform only
    // (rest-pose geometry through prevModel): in-place skeletal deformation has
    // no per-bone velocity yet, so limbs still rely on the neighbourhood clamp.
    void DrawSkinnedMesh(const Mesh& mesh, const Material& material, const math::Mat4& model,
                         const std::vector<math::Mat4>& boneMatrices, int boneCount,
                         const math::Mat4* prevModel);
    // True when this frame is writing per-object motion vectors (TAA active and
    // the velocity target exists). Hosts poll this to skip per-object transform
    // tracking when the buffer would be ignored anyway.
    bool VelocityEnabled() const { return velocityEnabled_ && velocityRT_.Valid(); }
    TextureHandle VelocityTexture() const;
    void DrawMeshInstanced(const Mesh& mesh, const Material& material, const math::Mat4* models,
                           uint32_t count, bool frustumCull = true);
    // Instanced draw with a per-instance RGBA color (sprite-billboard particles
    // that vary color per instance). `colors` has `count` entries.
    void DrawMeshInstancedColored(const Mesh& mesh, const Material& material,
                                  const math::Mat4* models, const math::Vec4* colors,
                                  uint32_t count, bool frustumCull = true);
    // GPU-particle billboards: `positions`/`sizes`/`colors` draw as a single
    // camera-facing instanced quad batch (depth-tested, unlit tinted texture).
    // Unlike the screen-space DrawBillboard helper this stays in the 3D scene
    // pass with correct depth/fog/light occlusion.
    void DrawBillboards(const math::Vec3* positions, const float* sizes,
                        const Color* colors, TextureHandle texture, uint32_t count,
                        BlendMode blend = BlendMode::Additive, float intensity = 1.0f,
                        const float* rotations = nullptr, const math::Vec4* uvRects = nullptr);
    // CPU-side projected shadow: projects the mesh onto the ground plane
    // (y=0) along lightDir. Works without any depth buffer or FBO. Used as the
    // fallback when CSM is disabled.
    void DrawProjectedShadow(const Mesh& mesh, const math::Mat4& model,
                             const math::Vec3& lightDir, const Color& color);
    // Skinned variant: skins the mesh CPU-side with the given bone matrices
    // (world * inverseBind, as produced by anim::Skeleton::ComputeBoneMatrices)
    // before projecting onto the ground plane. Falls back to the static
    // version when the mesh is not skinned.
    void DrawProjectedShadowSkinned(const Mesh& mesh, const math::Mat4& model,
                                    const std::vector<math::Mat4>& bones, int boneCount,
                                    const math::Vec3& lightDir, const Color& color);

    struct LineVertex {
        math::Vec3 pos;
        Color color;
    };
    void DrawLines(const LineVertex* vertices, uint32_t count, const math::Mat4& model);
    // Camera-facing additive ribbon through `points` (oldest first). `head` is
    // the colour at the newest point, `tail` at the oldest; `width` in world
    // units. Depth-tested (no write). Used for projectile/skill trails; the
    // buffer is rebuilt per call (a handful of points).
    void DrawTrail(const math::Vec3* points, uint32_t count, float width, const Color& head,
                   const Color& tail, float tailWidthScale = 1.0f);
    // One ribbon submission (points oldest-first) for DrawTrails.
    struct TrailDraw {
        const math::Vec3* points = nullptr;
        uint32_t count = 0;
        float width = 0.4f;
        float tailWidthScale = 1.0f; // width multiplier at the oldest point
        Color head{1.0f, 1.0f, 1.0f, 1.0f};
        Color tail{1.0f, 1.0f, 1.0f, 0.0f};
    };
    // Draws many ribbons as ONE buffer upload + ONE draw call (projectile and
    // skill trails all live in the same additive pass). Splits internally when
    // the 16-bit index buffer would overflow.
    void DrawTrails(const TrailDraw* draws, uint32_t drawCount);
    void DrawBox(const math::AABB& box, const Color& color);
    void DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments = 20);

    struct RenderStats {
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
        uint32_t instances = 0;
        // Step E2: meshes that also wrote a motion vector this frame (subset of
        // drawCalls; 0 whenever the velocity pass is off).
        uint32_t velocityDraws = 0;
    };
    const RenderStats& Stats() const { return stats_; }
    // G6-1: driver-reported GPU memory budget/usage (zeros when unavailable).
    IRenderBackend::GpuMemStats GpuMemory() const {
        return backend_ ? backend_->GpuMemory() : IRenderBackend::GpuMemStats{};
    }

    // Resources
    Texture CreateTexture(const TextureDesc& desc);
    // Releases a texture created by CreateTexture (dynamic masks/fonts). No-op
    // for an invalid texture.
    void DestroyTexture(Texture& tex);
    // Uploads an RGBA8 sub-rectangle into an existing texture (dynamic masks /
    // font atlases). No-op for an invalid texture.
    void UpdateTexture(const Texture& tex, int x, int y, int w, int h, const void* rgba);
    // Compressed (BC1/DXT1) texture upload; format is the backend format code
    // (assets::kBc1Format). Returns an invalid Texture when the driver rejects
    // compressed uploads - the asset layer then falls back to RGBA8.
    Texture CreateTextureCompressed(int width, int height, uint32_t format, const void* data,
                                    size_t size);
    Shader CreateShader(const char* vertexSource, const char* fragmentSource, const char* name);
    // P2-6 shader hot reload: creates a program from a CUSTOM fragment source
    // paired with the built-in unlit vertex shader (vUV/vColor + uTex contract).
    // The GL backend supports arbitrary fragment sources; the Vulkan backend
    // rejects custom shaders (documented limitation) and returns an invalid
    // handle so callers can fall back to the built-in material shader.
    Shader CreateUnlitFragmentShader(const std::string& fragmentSource,
                                     const std::string& name);
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
    // Filled triangle with per-vertex (Gouraud) colors.
    void DrawTriangle2DColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                               const Color& ca, const Color& cb, const Color& cc);
    void DrawText(const Font& font, const std::string& text, const math::Vec2& pos, float size,
                  const Color& color, bool centerX = false, bool centerY = false);

    // Maps the 1280x720 2D design space into a screen-space rect (fit + center,
    // preserving aspect). `zoom` scales around the design center (1 = fit the
    // whole design into the rect) and `pan` shifts the design point at the
    // rect's center (design units). Used by the editor to render the 2D canvas /
    // playtest inside the viewport dock instead of the whole window, with
    // zoom/pan camera control. Reset2DViewport restores the default full-window
    // mapping.
    void Set2DViewport(float x, float y, float w, float h, float zoom = 1.0f,
                       const math::Vec2& pan = {0.0f, 0.0f},
                       float aspect = 16.0f / 9.0f);
    void Reset2DViewport();
    // Maps the 2D design space 1:1 into the screen with its origin at (x, y):
    // a design pixel is a screen pixel. Used for the 3D playtest overlay so
    // HUD text/panels keep their intended size inside the viewport dock
    // (elements outside the rect are clipped by the caller's scissor).
    void Set2DViewportPixels(float x, float y);
    // Renders the 3D scene into a sub-rect of the target (the editor viewport
    // dock): sets the backend rasterization viewport to the rect. The caller
    // must also pass the rect's aspect to SetCamera so the projection matches.
    // ResetSceneViewport restores the full-target viewport (call before the
    // 2D overlay flush, which is in full-window pixel coordinates).
    void SetSceneViewport(float x, float y, float w, float h);
    void ResetSceneViewport();
    // Aspect ratio of the active 3D scene viewport (w/h), falling back to the
    // full target when no sub-rect is active. Callers that render into a dock
    // (editor viewport) use this so projections match the rasterization rect.
    float SceneAspect() const;
    void DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                       TextureHandle texture, BlendMode blend = BlendMode::Additive);

    // Flushes the batched 2D overlay now (useful to order custom UI before
    // other passes such as a tool overlay rendered directly on the backend).
    void Flush2D();

    math::Vec2 ScreenToUI(const math::Vec2& screenPixels) const;
    // Design -> screen for the 2D overlay (inverse of ScreenToUI).
    math::Vec2 ToScreen(const math::Vec2& design) const;
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
    float UIScale() const { return draw2d_.UiScale(); }
    // Top-left offset of the 2D design space inside the screen (with UIScale,
    // exactly the inverse mapping ScreenToUI uses). Lets hosts snapshot the
    // current 2D mapping without depending on the renderer's live state.
    math::Vec2 UI2DOffset() const { return {draw2d_.UiOffsetX(), draw2d_.UiOffsetY()}; }
    // The game area's design size: the letterboxed 16:9 rect the canvas
    // mapping projects (1280x720 under fit-within). UI layout, WorldToScreen
    // and GetViewportSize all resolve against THIS - the game area, not the
    // dock - so the modern box UI adapts within the 16:9 frame.
    math::Vec2 UIDesignSize() const {
        if (draw2d_.UiScale() <= 0.0f) return {static_cast<float>(kDesignWidth),
                                               static_cast<float>(kDesignHeight)};
        return {draw2d_.SceneViewport().w / draw2d_.UiScale(),
                draw2d_.SceneViewport().h / draw2d_.UiScale()};
    }
    // The screen rect the 1280x720 design space currently maps to (set by
    // Set2DViewport/Set2DViewportPixels). Hosts size the 3D scene viewport
    // with this exact rect so 3D geometry and the 2D HUD/anchor space share one
    // framing (no drift between world-anchored UI and the rendered scene).
    math::Rect2 DesignSpaceRect() const {
        return {draw2d_.UiOffsetX(), draw2d_.UiOffsetY(),
                static_cast<float>(kDesignWidth) * draw2d_.UiScale(),
                static_cast<float>(kDesignHeight) * draw2d_.UiScale()};
    }
    // The active 3D scene rasterization rect (set by SetSceneViewport). The
    // design-space rect above should equal this for world-anchored 2D UI.
    const math::Rect2& SceneViewport() const { return draw2d_.SceneViewport(); }
    // Last non-reset scene viewport (see sceneVpLast_). Post FX bound
    // themselves to this rect even after the live viewport is reset.
    const math::Rect2& SceneVpLastRect() const { return sceneVpLast_; }
    int ScreenWidth() const { return screenW_; }
    int ScreenHeight() const { return screenH_; }

private:
    void InitBuiltinResources();
    void DrawProjectedShadowVerts(const std::vector<Vertex3D>& verts,
                                  const std::vector<uint16_t>& indices,
                                  const math::Mat4& model, const math::Vec3& lightDir,
                                  const Color& color);
    void ApplyMaterial(const Material& material, const math::Mat4& mvp, const math::Mat4& model,
                       const math::Mat4& normalMat, ShaderHandle shader);
    // Depth-projected decal path (Material::decal). Rebuilds the receiving
    // surface from the resolved scene depth and writes gl_FragDepth so the decal
    // sits exactly on the geometry. Falls back to a plain unlit quad when no
    // sampleable depth is available this frame.
    void DrawDecal(const Mesh& mesh, const Material& material, const math::Mat4& model);
    // Wires the backend + the shared scene-uniform stamp into the subsystems
    // (called from Init and AttachBackendForTesting).
    void ConnectSubsystems();

    // HDR + bloom post-processing.
    // (Re)creates the main-scene HDR targets (hdrRT_/hdrMsaaRT_) at the current
    // window resolution and rebuilds the unified post-processing FrameGraph
    // (postGraph_) at that resolution. Called from BeginFrame when the size
    // changed; the post graph's transient targets (bloom pyramid / depth/AO/
    // vol/SSR) all live in its own pool, so nothing else manages them.
    void RebuildHdrTargets();
    void DestroyHdrTargets();
    // Step D: the scale actually used this frame (dynamic-resolution adjusted,
    // pinned to 1.0 while a custom scene viewport is active).
    float EffectiveRenderScale() const;
    void UpdateDynamicResolution();
    // Step E: resolve this frame's jittered HDR scene against the TAA history
    // (no-op unless TAA is on and a sampleable depth is available).
    void ApplyTaa();
    // Step E2: record one object's motion vector for this frame's velocity pass
    // (no-op when the pass is off). The draws are BATCHED and replayed once per
    // frame by FlushVelocityPass: switching render targets flushes and submits
    // the pending command buffer in the Vulkan backend, so one target bind per
    // moving object cost two full GPU stalls per object per frame.
    void SubmitVelocity(const Mesh& mesh, const math::Mat4& model, const math::Mat4& prevModel);
    // Rasterizes every recorded motion vector into velocityRT_ in ONE pass, then
    // rebinds the main target. Called before the TAA resolve samples velocityRT_.
    void FlushVelocityPass();
    void RestoreSceneViewport();
    // Builds the per-frame post graph input from the current renderer state.
    // chains=false forces every post chain (ssao/vol/ssr/depth/fog) off while
    // keeping bloom + composite, matching the old CaptureBloom/TonemapComparison
    // behaviour (they never ran the post graph, only bloom + composite).
    PostGraph::FrameParams MakePostParams(bool chains) const;
    void DrawSsaoDepthCasters(const math::Mat4& viewProj);
    bool TestFloatTargetCapability();
    // B1: uploads the per-FRAME scene uniforms (sun/lights/fog/view/shadow/IBL)
    // once per (frame, program) pair -- draws after the first in a frame skip
    // ~40 redundant SetUniform/Bind calls, but a program switch re-applies so
    // mixed-path frames (skinned + instanced + terrain) never miss the block.
    void ApplySceneUniforms(ShaderHandle shader);
    // MSAA: multisample HDR render target + resolve self-test (4x then 2x).
    bool TestMsaaCapability();
    // Ensures the main-pass depth is resolved to a sampleable texture for this
    // frame (once), so soft particles can read scene depth. False when the
    // backend cannot provide a depth texture (MSAA off / no depth target).
    bool EnsureSoftDepth();
    // Binds whichever target the main scene renders into (the HDR float target
    // when active, else the default framebuffer).
    void RebindMainTarget();
    // Resolves the MSAA HDR scene target into the single-sample bloom source
    // (no-op when MSAA is inactive). Called before any pass that samples the
    // HDR target: CompositeSceneToBackbuffer and the capture helpers.
    void ResolveMainTarget();
    // True when the post chain will read the main-pass depth this frame (MSAA
    // targets present and the backend supports depth resolve), in which case the
    // SSAO/SSR colour-depth path reuses it and the caster fallback list is not
    // collected at all.
    bool PostDepthReuseOk() const {
        return depthResolveSupported_ && msaaEnabled_ && hdrMsaaRT_.Valid() &&
               hdrDepthRT_.Valid();
    }
    // Composite HDR (+ bloom) to the backbuffer with the composite shader.
    // Runs the whole unified post chain (postGraph_) once: the SSAO/vol/SSR/
    // depth/bloom chains execute only when their enabled flags are on, and the
    // final composite pass draws the result to the backbuffer.
    void CompositeSceneToBackbuffer();
    // Bloom + composite + Flush2D unless the frame was already composited
    // (EndScene composited early so the HUD is drawn on top, unbloomed).
    void CompositeFrame();

    std::unique_ptr<IRenderBackend> backend_;
    std::string backendName_ = "gl";
    // When true, backend_ is wrapped in a ThreadedBackend that replays all
    // rendering on a dedicated thread. Defaults off (unchanged behavior).
    bool renderThreadEnabled_ = false;

    // Composition services (see the class comment): shadow pass, scene state
    // and the 2D overlay. The facade owns them and forwards every public call.
    ShadowSystem shadowSystem_;
    SceneState sceneState_;
    DrawBatch2D draw2d_;
    // Most recent SetSceneViewport rect (pixels). Kept across ResetSceneViewport
    // so per-frame post FX (volume god-rays) can bound themselves to the rect
    // the 3D scene actually rasterized into, even though the host resets the
    // live viewport after the scene pass. A zero width/height means "full
    // target" (no letterbox), in which case effects sample the whole HDR frame.
    math::Rect2 sceneVpLast_{0.0f, 0.0f, 0.0f, 0.0f};

    ShaderHandle litShader_;
    ShaderHandle terrainShader_;
    ShaderHandle skinnedLitShader_;
    ShaderHandle unlitShader_;
    ShaderHandle linesShader_;
    ShaderHandle litInstancedShader_;
    ShaderHandle unlitInstancedShader_;
    ShaderHandle unlitInstancedColoredShader_;
    ShaderHandle particleSoftShader_;   // depth-faded billboard variant (soft particles)
    ShaderHandle particleShader_;       // atlas/flipbook billboard (per-instance UV rect)
    ShaderHandle decalShader_;          // depth-projected ground decal
    bool softDepthReady_ = false;       // resolved the main-pass depth this frame
    float softFadeRange_ = 0.75f;       // world-unit fade band for soft particles
    gfx::Mesh billboardQuad_;  // unit XY quad used by DrawBillboards
    // Post-processing (HDR + bloom).
    ShaderHandle brightPassShader_;
    ShaderHandle blurShader_;
    ShaderHandle downsampleShader_;
    ShaderHandle upsampleAddShader_;
    ShaderHandle luminanceShader_;
    ShaderHandle luminanceReduceShader_;
    ShaderHandle exposureAdaptShader_;
    ShaderHandle compositeShader_;
    ShaderHandle ssaoShader_;
    ShaderHandle depthEncodeShader_; // B4: fullscreen resolved-depth encode
    ShaderHandle ssaoBlurShader_;
    ShaderHandle ssaoDepthShader_;   // SSAO depth pre-pass (linear camera depth)
    ShaderHandle ssaoDepthMeshShader_;   // non-instanced variant
    ShaderHandle ssaoDepthSkinnedShader_; // GPU-skinned variant (linear depth)
    ShaderHandle volumetricShader_;
    ShaderHandle ssrShader_;
    ShaderHandle ssrBlurShader_;
    ShaderHandle skyboxShader_;
    TextureHandle white_;
    MeshHandle probeQuadMesh_;
    // Fullscreen NDC quad (uv 0..1) for the post passes.
    MeshHandle postQuadMesh_;
    // A4 procedural skybox state (view-ray sun/moon/clouds; off = old gradient).
    SkyBoxParams skyBox_;
    float skyTime_ = 0.0f; // monotonically increasing (cloud drift)
    // B1: bumped whenever the per-frame scene uniform set changes; the first
    // lit draw of a frame (or after any change) uploads the whole scene block.
    // Shared with the subsystems (SceneState light/fog/IBL setters and the
    // ShadowSystem point-light pass bump it via ConnectSubsystems).
    uint64_t sceneUniformStamp_ = 0;
    // B1+: the per-frame scene uniform block is identical across every draw;
    // track the applied stamp PER PROGRAM (uniform locations are per-program)
    // so alternating shaders (lit / lit-instanced / skinned) do not re-upload
    // the whole block on every draw. Cleared when it grows past shader churn.
    std::unordered_map<uint32_t, uint64_t> sceneUniformProgramStamps_;

    // HDR scene target (window size, RGBA16F). hdrMsaaRT_ is the 4x/2x
    // multisample target the scene renders into when MSAA is active; hdrRT_ is
    // the single-sample target the MSAA target is resolved into and the source
    // the post chain samples from. Both are MAIN-SCENE targets owned by the
    // renderer (the scene draws into them); every post target (bloom pyramid /
    // depth/AO/vol/SSR) lives in postGraph_'s FrameGraph transient pool.
    RenderTargetHandle hdrRT_;
    RenderTargetHandle hdrMsaaRT_;
    // B4: single-sample depth target resolved from the MSAA HDR depth so the post
    // chain can sample the main pass depth instead of redrawing the casters.
    RenderTargetHandle hdrDepthRT_;
    bool depthResolved_ = false;
    // Set false the first time ResolveDepth() fails with valid targets, so the
    // SSAO/SSR caster fallback collection switches back on (a driver that cannot
    // resolve depth must keep the legacy depth pre-pass path).
    bool depthResolveSupported_ = true;
    // G1-5 SSAO/volumetric/SSR/depth + Task 2 bloom + Task 4 composite: one
    // unified post-processing FrameGraph. The depth/AO/blur/vol/ssr targets and
    // the bloom pyramid live in its transient pool; the composite pass reads
    // the scene HDR (hdrRT_, injected as the external input) plus each chain's
    // final and draws the result to the backbuffer. hdrScene is resolved (MSAA)
    // before Execute.
    PostGraph postGraph_;
    int hdrW_ = 0;
    int hdrH_ = 0;
    bool hdrEnabled_ = false;
    bool bloomEnabled_ = true;
    float bloomThreshold_ = kBloomThreshold;
    float bloomStrength_ = kBloomStrength;
    float bloomWidth_ = 1.6f;
    // CSM quality: PCSS kernel scale + normal-offset bias in cascade texels.
    float shadowSoftness_ = 1.0f;
    float shadowNormalOffset_ = 1.5f;
    float exposure_ = 1.0f;
    bool tonemapEnabled_ = true;
    // A1 color grading (post-tonemap procedural "film look"); default disabled.
    ColorGrade colorGrade_;
    // A5 auto-exposure + vignette (composite). autoExposure_/vignette_ drive
    // the composite; the exposure is adapted per-frame in the shader from the
    // measured average log-luminance (no CPU readback).
    AutoExposure autoExposure_;
    Vignette vignette_;
    // A3 probe-field GI: the baked irradiance atlas + its grid metadata. When
    // disabled (invalid texture) the lit shader contributes no GI term.
    TextureHandle lightProbeAtlas_;
    math::AABB lightProbeBounds_{};
    int lightProbeRes_ = 0;
    float lightProbeMaxIrr_ = 1.0f;
    bool msaaRequested_ = true;
    bool msaaEnabled_ = false;
    bool iblWasValid_ = false; // last frame had a live IBL set (else-branch reset)
    int msaaSamples_ = 0;
    int msaaSampleRequest_ = 0; // 0 = auto (probe 4x then 2x)
    // Step D quality/scalability state.
    Quality quality_ = Quality::High;
    float renderScale_ = 1.0f;
    float dynScale_ = 1.0f;
    float dynMinScale_ = 0.6f;
    float dynTargetFps_ = 60.0f;
    bool dynResEnabled_ = false;
    size_t particleBudget_ = 32768;
    // Step E temporal-AA state.
    Taa taa_;
    // -1 = unresolved (probe NEON_SHADOW_DEBUG once), 0 = off, 1 = shadow-factor view.
    int shadowDebug_ = -1;
    bool taaEnabled_ = false;
    // Step E2 motion-vector pass (only allocated/used while TAA is on).
    ShaderHandle velocityShader_;
    RenderTargetHandle velocityRT_;
    // One entry per moving object, replayed by FlushVelocityPass (the colour
    // pass order is unaffected: velocity rasterizes into its own target).
    struct VelocityDraw {
        MeshHandle mesh;
        math::Mat4 model;
        math::Mat4 prevModel;
    };
    std::vector<VelocityDraw> velocityBatch_;
    bool velocityEnabled_ = false;
    float taaBlend_ = 0.12f;
    float taaSharpen_ = 0.5f;
    bool taaHistoryValid_ = false;
    bool prevViewProjValid_ = false;
    math::Mat4 prevViewProj_;
    RenderTargetHandle taaOutput_;
    int taaFrame_ = 0;
    math::Vec2 jitterPixels_{0.0f, 0.0f};
    double frameTimeEmaMs_ = 0.0;
    long long lastFrameClock_ = 0;
    float lastFrameDtSec_ = 1.0f / 60.0f; // raw wall-clock dt (AE adapt rate)
    // G1-5 SSAO state.
    bool ssaoEnabled_ = false;
    float ssaoIntensity_ = 1.0f; // AO blend amount in [0,1]
    std::vector<ShadowSystem::ShadowDraw> ssaoCasters_;
    // G1-5 volumetric shafts state.
    bool volumetricEnabled_ = false;
    float volumetricIntensity_ = 1.0f;
    // G1-5 SSR state.
    bool ssrEnabled_ = false;
    float ssrIntensity_ = 1.0f;

    RenderStats stats_;

    // Reusable per-frame scratch buffers. The draw paths below used to build a
    // fresh std::vector on every call (instanced culling, bone-matrix flatten,
    // shadow-caster sort, projected-shadow projection); they are reused across
    // calls within a frame (and across frames) so a busy scene stops paying
    // for heap churn in the hot path.
    std::vector<math::Mat4> instancedVisible_;
    std::vector<math::Vec4> instancedVisibleColored_;
    // Billboard scratch: the model/colour instance streams are rebuilt every
    // frame (particles), so keep the buffers instead of allocating per call.
    std::vector<math::Mat4> billboardModels_;
    std::vector<math::Vec4> billboardColors_;
    std::vector<math::Vec4> billboardUvRects_;
    std::vector<LineVertex> trailVerts_;      // merged ribbon batch scratch
    std::vector<uint16_t> trailIndices_;
    std::vector<float> boneUniformFlat_;
    std::vector<ShadowSystem::ShadowSortKey> shadowSortKeys_;
    std::vector<LineVertex> projectedShadowVerts_;

    platform::IWindow* window_ = nullptr;
    int screenW_ = 1280;
    int screenH_ = 720;
    // Background GPU-upload worker on a shared GL context. Available only when
    // the platform supports shared contexts; AssetManager uploads may route
    // through it (gfx/upload_thread.hpp).
    std::unique_ptr<UploadThread> uploadThread_;
};

} // namespace neon::gfx
