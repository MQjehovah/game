#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/draw_batch2d.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/ibl.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/post_graph.hpp"
#include "neon/gfx/scene_state.hpp"
#include "neon/gfx/shader.hpp"
#include "neon/gfx/shadow_system.hpp"
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
    void DrawSky();
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
                        BlendMode blend = BlendMode::Additive, float intensity = 1.0f);
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
    void DrawBox(const math::AABB& box, const Color& color);
    void DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments = 20);

    struct RenderStats {
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
        uint32_t instances = 0;
    };
    const RenderStats& Stats() const { return stats_; }
    // G6-1: driver-reported GPU memory budget/usage (zeros when unavailable).
    IRenderBackend::GpuMemStats GpuMemory() const {
        return backend_ ? backend_->GpuMemory() : IRenderBackend::GpuMemStats{};
    }

    // Resources
    Texture CreateTexture(const TextureDesc& desc);
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
    // Binds whichever target the main scene renders into (the HDR float target
    // when active, else the default framebuffer).
    void RebindMainTarget();
    // Resolves the MSAA HDR scene target into the single-sample bloom source
    // (no-op when MSAA is inactive). Called before any pass that samples the
    // HDR target: CompositeSceneToBackbuffer and the capture helpers.
    void ResolveMainTarget();
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

    // Composition services (see the class comment): shadow pass, scene state
    // and the 2D overlay. The facade owns them and forwards every public call.
    ShadowSystem shadowSystem_;
    SceneState sceneState_;
    DrawBatch2D draw2d_;

    ShaderHandle litShader_;
    ShaderHandle terrainShader_;
    ShaderHandle skinnedLitShader_;
    ShaderHandle unlitShader_;
    ShaderHandle linesShader_;
    ShaderHandle litInstancedShader_;
    ShaderHandle unlitInstancedShader_;
    ShaderHandle unlitInstancedColoredShader_;
    gfx::Mesh billboardQuad_;  // unit XY quad used by DrawBillboards
    // Post-processing (HDR + bloom).
    ShaderHandle brightPassShader_;
    ShaderHandle blurShader_;
    ShaderHandle downsampleShader_;
    ShaderHandle upsampleAddShader_;
    ShaderHandle compositeShader_;
    ShaderHandle ssaoShader_;
    ShaderHandle ssaoBlurShader_;
    ShaderHandle ssaoDepthShader_;   // SSAO depth pre-pass (linear camera depth)
    ShaderHandle ssaoDepthMeshShader_;   // non-instanced variant
    ShaderHandle volumetricShader_;
    ShaderHandle ssrShader_;
    TextureHandle white_;
    MeshHandle probeQuadMesh_;
    // Fullscreen NDC quad (uv 0..1) for the post passes.
    MeshHandle postQuadMesh_;
    // B1: bumped whenever the per-frame scene uniform set changes; the first
    // lit draw of a frame (or after any change) uploads the whole scene block.
    // Shared with the subsystems (SceneState light/fog/IBL setters and the
    // ShadowSystem point-light pass bump it via ConnectSubsystems).
    uint64_t sceneUniformStamp_ = 0;
    uint64_t sceneUniformAppliedStamp_ = ~0ull;
    ShaderHandle lastSceneUniformShader_;

    // HDR scene target (window size, RGBA16F). hdrMsaaRT_ is the 4x/2x
    // multisample target the scene renders into when MSAA is active; hdrRT_ is
    // the single-sample target the MSAA target is resolved into and the source
    // the post chain samples from. Both are MAIN-SCENE targets owned by the
    // renderer (the scene draws into them); every post target (bloom pyramid /
    // depth/AO/vol/SSR) lives in postGraph_'s FrameGraph transient pool.
    RenderTargetHandle hdrRT_;
    RenderTargetHandle hdrMsaaRT_;
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
    float exposure_ = 1.0f;
    bool tonemapEnabled_ = true;
    bool msaaRequested_ = true;
    bool msaaEnabled_ = false;
    int msaaSamples_ = 0;
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
    std::vector<float> boneUniformFlat_;
    std::vector<ShadowSystem::ShadowSortKey> shadowSortKeys_;
    std::vector<LineVertex> projectedShadowVerts_;

    platform::IWindow* window_ = nullptr;
    int screenW_ = 1280;
    int screenH_ = 720;
};

} // namespace neon::gfx
