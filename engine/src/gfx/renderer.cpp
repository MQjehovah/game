#include "neon/gfx/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "neon/core/log.hpp"
#include "neon/gfx/bloom.hpp"
#include "neon/gfx/builtin_shaders.hpp"
#include "neon/gfx/fog.hpp"
#include "neon/gfx/ssao.hpp"
#include "neon/gfx/ssr.hpp"
#include "neon/gfx/volumetric.hpp"
#include "neon/gfx/skybox.hpp"

// Step E: Halton(2,3) sub-pixel jitter helper (defined below with the TAA code).
namespace neon::gfx { static math::Vec2 TaaJitterOffset(int index); }

namespace neon::gfx {
namespace {

// Inverse-transpose of the upper 3x3 of a model matrix (normal matrix).
math::Mat4 NormalMatrix(const math::Mat4& m) {
    float a00 = m.m[0], a01 = m.m[1], a02 = m.m[2];
    float a10 = m.m[4], a11 = m.m[5], a12 = m.m[6];
    float a20 = m.m[8], a21 = m.m[9], a22 = m.m[10];
    float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
                a02 * (a10 * a21 - a11 * a20);
    math::Mat4 r;
    if (std::fabs(det) < 1e-8f) return r;
    float invDet = 1.0f / det;
    r.m[0] = (a11 * a22 - a12 * a21) * invDet;
    r.m[1] = (a02 * a21 - a01 * a22) * invDet;
    r.m[2] = (a01 * a12 - a02 * a11) * invDet;
    r.m[4] = (a12 * a20 - a10 * a22) * invDet;
    r.m[5] = (a00 * a22 - a02 * a20) * invDet;
    r.m[6] = (a02 * a10 - a00 * a12) * invDet;
    r.m[8] = (a10 * a21 - a11 * a20) * invDet;
    r.m[9] = (a01 * a20 - a00 * a21) * invDet;
    r.m[10] = (a00 * a11 - a01 * a10) * invDet;
    return r;
}

} // namespace

Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(platform::IWindow* window) {
    window_ = window;
    backend_ = CreateOpenGLBackend();
#if defined(NEON_ENABLE_VULKAN)
    if (backendName_ == "vulkan") {
        backend_ = CreateVulkanBackend();
    }
#endif
    if (!backend_) {
        backend_ = CreateOpenGLBackend();
    }
    // Render thread: wrap the real (GL) backend so every call is marshaled to
    // a dedicated thread that owns the window's GL context. Disabled for
    // Vulkan (the shared-context migration is GL-specific).
    if (renderThreadEnabled_ && backend_->Name()[0] == 'O') {  // "OpenGL 3.3"
        backend_ = std::make_unique<ThreadedBackend>(std::move(backend_));
    }
    if (!backend_ || !backend_->Init(window)) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "Renderer: %s backend initialization failed",
                     backendName_ == "vulkan" ? "Vulkan" : "OpenGL");
        return false;
    }
    ConnectSubsystems();
    InitBuiltinResources();

    screenW_ = window_->Width();
    screenH_ = window_->Height();
    draw2d_.Resize(screenW_, screenH_);
    // Background GPU-upload worker on a shared GL context (resource context).
    // OpenGL-only (the shared-context abstraction is GL-specific); optional —
    // on platforms without shared-context support Start() fails and uploads
    // stay on the main thread.
    const char* bn = backend_->Name();
    if (bn && std::strncmp(bn, "OpenGL", 6) == 0) {
        uploadThread_ = std::make_unique<UploadThread>();
        if (!uploadThread_->Start(window_)) uploadThread_.reset();
    }
    return true;
}

void Renderer::AttachBackendForTesting(std::unique_ptr<IRenderBackend> backend) {
    backend_ = std::move(backend);
    ConnectSubsystems();
}

void Renderer::ConnectSubsystems() {
    shadowSystem_.SetBackend(backend_.get());
    shadowSystem_.SetSceneUniformStamp(&sceneUniformStamp_);
    sceneState_.SetBackend(backend_.get());
    sceneState_.SetSceneUniformStamp(&sceneUniformStamp_);
    draw2d_.SetBackend(backend_.get());
}

void Renderer::Shutdown() {
    if (!backend_) return;
    taa_.Shutdown(*backend_, /*keepShader=*/false);
    // Stop the background upload worker before destroying any shared GL
    // resources or the window's context (the worker holds the shared context).
    if (uploadThread_) {
        uploadThread_->Shutdown();
        uploadThread_.reset();
    }
    if (litShader_.Valid()) backend_->DestroyShader(litShader_);
    if (skinnedLitShader_.Valid()) backend_->DestroyShader(skinnedLitShader_);
    if (unlitShader_.Valid()) backend_->DestroyShader(unlitShader_);
    if (decalShader_.Valid()) backend_->DestroyShader(decalShader_);
    if (velocityShader_.Valid()) backend_->DestroyShader(velocityShader_);
    if (linesShader_.Valid()) backend_->DestroyShader(linesShader_);
    if (litInstancedShader_.Valid()) backend_->DestroyShader(litInstancedShader_);
    if (unlitInstancedShader_.Valid()) backend_->DestroyShader(unlitInstancedShader_);
    if (brightPassShader_.Valid()) backend_->DestroyShader(brightPassShader_);
    if (blurShader_.Valid()) backend_->DestroyShader(blurShader_);
    if (downsampleShader_.Valid()) backend_->DestroyShader(downsampleShader_);
    if (upsampleAddShader_.Valid()) backend_->DestroyShader(upsampleAddShader_);
    if (compositeShader_.Valid()) backend_->DestroyShader(compositeShader_);
    if (probeQuadMesh_.Valid()) backend_->DestroyMesh(probeQuadMesh_);
    if (postQuadMesh_.Valid()) backend_->DestroyMesh(postQuadMesh_);
    shadowSystem_.Shutdown(*backend_);
    sceneState_.Shutdown(*backend_);
    draw2d_.Shutdown(*backend_);
    DestroyHdrTargets();
    if (white_.Valid()) backend_->DestroyTexture(white_);
    backend_->Shutdown();
    backend_.reset();
}

void Renderer::InitBuiltinResources() {
    unsigned char whitePx[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.rgba = whitePx;
    white_ = backend_->CreateTexture(whiteDesc);

    litShader_ = backend_->CreateShader(kLitVertexShader, kLitFragmentShader, "lit");
    {
        // Skinned lit variant: same source with #define SKINNED 1 inserted
        // right after the #version line (GLSL requires #version first) so the
        // shader enables the joint/weight attributes + uBoneMatrices.
        std::string skinnedSrc(kLitVertexShader);
        size_t versionPos = skinnedSrc.find("#version");
        size_t versionEnd = skinnedSrc.find('\n', versionPos);
        skinnedSrc.insert(versionEnd + 1, "#define SKINNED 1\n");
        skinnedLitShader_ =
            backend_->CreateShader(skinnedSrc.c_str(), kLitFragmentShader, "lit_skinned");
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: skinned lit shader %s",
                     skinnedLitShader_.Valid() ? "ok" : "FAILED");
    }
    unlitShader_ = backend_->CreateShader(kUnlitVertexShader, kUnlitFragmentShader, "unlit");
    {
        // G4 terrain splatmap variant: same lit source with #define TERRAIN_SPLAT
        // to blend grass/dirt/rock layers by the vertex splat weights. Terrain
        // chunks use this; every other mesh keeps the plain lit shader.
        std::string fragSrc(kLitFragmentShader);
        size_t v = fragSrc.find("#version");
        size_t ve = fragSrc.find('\n', v);
        fragSrc.insert(ve + 1, "#define TERRAIN_SPLAT 1\n");
        terrainShader_ =
            backend_->CreateShader(kLitVertexShader, fragSrc.c_str(), "lit_terrain");
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: terrain lit shader %s",
                     terrainShader_.Valid() ? "ok" : "FAILED");
    }
    linesShader_ = backend_->CreateShader(kLineVertexShader, kLineFragmentShader, "lines");
    litInstancedShader_ =
        backend_->CreateShader(kLitInstancedVertexShader, kLitFragmentShader, "lit_instanced");
    unlitInstancedShader_ =
        backend_->CreateShader(kUnlitInstancedVertexShader, kUnlitFragmentShader, "unlit_instanced");
    unlitInstancedColoredShader_ =
        backend_->CreateShader(kUnlitInstancedColoredVertexShader,
                               kUnlitInstancedColoredFragmentShader, "unlit_instanced_colored");
    particleSoftShader_ =
        backend_->CreateShader(kParticleSoftVertexShader, kParticleSoftFragmentShader,
                               "particle_soft");
    // Atlas/flipbook billboard variant (per-instance UV rectangle). Invalid on
    // backends that resolve shaders by name without this program registered (the
    // Vulkan table), in which case DrawBillboards falls back to the plain
    // instanced-coloured program (no flipbook, everything else identical).
    particleShader_ = backend_->CreateShader(kParticleVertexShader,
                                             kUnlitInstancedColoredFragmentShader, "particle");
    // Depth-projected ground decal (Material::decal). Registered as "decal";
    // backends that resolve programs by name without this entry leave it invalid
    // and DrawDecal degrades to the plain unlit quad.
    decalShader_ = backend_->CreateShader(kDecalVertexShader, kDecalFragmentShader, "decal");
    // Step E2 motion vectors for TAA (see DrawMesh's prevModel overload). A
    // backend without this program simply leaves it invalid and the velocity
    // target is never allocated -> the resolve keeps using depth reprojection.
    velocityShader_ =
        backend_->CreateShader(kVelocityVertexShader, kVelocityFragmentShader, "velocity");
    billboardQuad_ = Mesh::CreateQuad(*this, 1.0f, 1.0f, "billboard");

    // Post-processing shaders (HDR + bloom). Sources live in bloom.hpp so the
    // pure math and the shader tokens are unit-testable headlessly.
    brightPassShader_ =
        backend_->CreateShader(kPostVertexShader, kBrightPassFragmentShader, "bloom_bright");
    blurShader_ = backend_->CreateShader(kPostVertexShader, kBlurFragmentShader, "bloom_blur");
    downsampleShader_ =
        backend_->CreateShader(kPostVertexShader, kDownsampleFragmentShader, "bloom_downsample");
    upsampleAddShader_ =
        backend_->CreateShader(kPostVertexShader, kUpsampleAddFragmentShader, "bloom_upsample_add");
    luminanceShader_ =
        backend_->CreateShader(kPostVertexShader, kLuminanceShader, "autoexposure_lum");
    luminanceReduceShader_ =
        backend_->CreateShader(kPostVertexShader, kLuminanceReduceShader, "autoexposure_avg");
    exposureAdaptShader_ =
        backend_->CreateShader(kPostVertexShader, kExposureAdaptShader, "autoexposure_adapt");
    compositeShader_ =
        backend_->CreateShader(kPostVertexShader, kCompositeFragmentShader, "bloom_composite");
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: bloom shaders %s",
                 (brightPassShader_.Valid() && blurShader_.Valid() && downsampleShader_.Valid() &&
                  upsampleAddShader_.Valid() && compositeShader_.Valid())
                     ? "ok"
                     : "FAILED");
    // G1-5 SSAO: the AO + blur programs. The depth pass reuses the shadow
    // depth shaders with the main-camera VP (colour-encoded gl_FragCoord.z).
    ssaoDepthShader_ = backend_->CreateShader(kSsaoDepthVertexShader, kSsaoDepthFragmentShader,
                                              "ssao_depth");
    ssaoDepthSkinnedShader_ = backend_->CreateShader(
        kSsaoDepthSkinnedVertexShader, kSsaoDepthFragmentShader, "ssao_depth_skinned");
    ssaoDepthMeshShader_ = backend_->CreateShader(kSsaoDepthMeshVertexShader,
                                                  kSsaoDepthFragmentShader, "ssao_depth_mesh");
    ssaoShader_ = backend_->CreateShader(kPostVertexShader, kSsaoFragmentShader, "ssao");
    depthEncodeShader_ =
        backend_->CreateShader(kPostVertexShader, kDepthEncodeFragmentShader, "depth_encode");
    ssaoBlurShader_ =
        backend_->CreateShader(kPostVertexShader, kSsaoBlurFragmentShader, "ssao_blur");
    volumetricShader_ =
        backend_->CreateShader(kPostVertexShader, kVolumetricFragmentShader, "volumetric");
    ssrShader_ = backend_->CreateShader(kPostVertexShader, kSsrFragmentShader, "ssr");
    skyboxShader_ = backend_->CreateShader(kSkyboxVertexShader, kSkyboxFragmentShader, "skybox");
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: SSAO/SSR/volumetric shaders %s",
                 (ssaoShader_.Valid() && ssaoBlurShader_.Valid() && volumetricShader_.Valid() &&
                  ssrShader_.Valid())
                     ? "ok"
                     : "FAILED");
    if (std::getenv("NEON_NO_BLOOM")) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: bloom disabled by NEON_NO_BLOOM");
        bloomEnabled_ = false;
    }
    // Diagnostic override for A/B screenshot diffs (same role as NEON_NO_BLOOM):
    // disables CSM so a render can be diffed against the same frame with shadows
    // on, which isolates the shadow contribution without touching scene data.
    if (std::getenv("NEON_NO_SHADOWS")) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: shadows disabled by NEON_NO_SHADOWS");
        shadowSystem_.SetShadowsEnabled(false);
    }

    // NDC unit quad used by the FBO capability self-test.
    const Vertex3D quadVerts[4] = {
        {{-1, -1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, -1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, 1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{-1, 1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    const uint16_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    probeQuadMesh_ = backend_->CreateMesh(quadVerts, 4, quadIndices, 6);

    // Fullscreen NDC quad with texture coordinates for the post passes.
    const Vertex3D postVerts[4] = {
        {{-1, -1, 0}, {}, {0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, -1, 0}, {}, {1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, 1, 0}, {}, {1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{-1, 1, 0}, {}, {0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    postQuadMesh_ = backend_->CreateMesh(postVerts, 4, quadIndices, 6);

    // 2D overlay: UI program + batch buffers (DrawBatch2D).
    draw2d_.Init(*backend_, white_);

    // HDR float-target capability (independent of the shadow path, so
    // --no-shadows still gets HDR + bloom). If the driver cannot render into a
    // half-float FBO, the renderer falls back to the legacy direct-to-backbuffer
    // flow and bloom is skipped.
    hdrEnabled_ = TestFloatTargetCapability();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx,
                 hdrEnabled_ ? neon::core::LogLevel::Info : neon::core::LogLevel::Warn,
                 "Renderer: HDR float-target pipeline %s (bloom %s)",
                 hdrEnabled_ ? "ACTIVE" : "UNAVAILABLE (legacy backbuffer path)",
                 hdrEnabled_ && bloomEnabled_ ? "on" : "off");

    // MSAA on the HDR scene target (Task 3.7): gated on the float path AND the
    // multisample FBO + blit-resolve self-test. A failure (or --no-msaa) keeps
    // the single-sample HDR target, so every fallback still composites.
    if (hdrEnabled_ && msaaRequested_) {
        msaaEnabled_ = TestMsaaCapability();
        if (!msaaEnabled_) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: MSAA unavailable -> single-sample HDR path");
        }
    } else if (hdrEnabled_ && !msaaRequested_) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: MSAA disabled by flag -> single-sample HDR path");
    }

    // Shadow subsystem: depth/point-depth shaders + cascade/point render
    // targets + the FBO capability self-tests (ShadowSystem).
    shadowSystem_.Init(*backend_, probeQuadMesh_, unlitShader_);
}

void Renderer::BeginFrame(const Color& clearColor, float clearDepth) {
    ++sceneUniformStamp_; // B1: a new frame invalidates the scene uniform cache
    skyTime_ += 1.0f / 60.0f; // cloud drift clock (close enough for a sky)
    stats_ = RenderStats{};
    shadowSystem_.BeginFrame();
    // Previous frame's post graph is fully consumed (the composite pass sampled
    // its finals); return its exported/pooled targets and clear the "already
    // composited this frame" latch.
    postGraph_.ResetFrame();
    ssaoCasters_.clear();
    softDepthReady_ = false;
    // Motion vectors recorded by a frame that never reached its composite (an
    // early-out capture path) must not leak into this frame's velocity pass.
    velocityBatch_.clear();
    screenW_ = window_ ? window_->Width() : screenW_;
    screenH_ = window_ ? window_->Height() : screenH_;
    draw2d_.Resize(screenW_, screenH_);
    UpdateDynamicResolution(); // Step D: may change the render scale this frame
    // Step E: advance the sub-pixel jitter once per frame (idempotent across
    // multiple SetCamera calls in the same frame).
    if (taaEnabled_) jitterPixels_ = TaaJitterOffset(taaFrame_++);
    else jitterPixels_ = {0.0f, 0.0f};
    RebuildHdrTargets();
    if (hdrEnabled_ && hdrRT_.Valid()) {
        // Scene + sky draw into the (multisample when MSAA is active) HDR
        // target; the final composite (bloom -> backbuffer) happens in
        // EndFrame / CaptureFrame after resolving the MSAA samples.
        RebindMainTarget();
        // The HDR target may be smaller than the window (render scale < 1):
        // render into it at ITS size, the composite upscales to the window.
        backend_->SetViewport(0, 0, hdrW_, hdrH_);
        backend_->Clear(clearColor, clearDepth);
        if (velocityEnabled_ && velocityRT_.Valid()) {
            // Zero-filled: alpha 0 means "no moving object covers this pixel",
            // so the resolve falls back to depth reprojection there.
            backend_->BindRenderTarget(velocityRT_);
            backend_->SetViewport(0, 0, hdrW_, hdrH_);
            backend_->Clear({0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
            RebindMainTarget();
            backend_->SetViewport(0, 0, hdrW_, hdrH_);
        }
    } else {
        backend_->BindDefaultTarget();
        backend_->SetViewport(0, 0, screenW_, screenH_);
        backend_->Clear(clearColor, clearDepth);
    }
}

void Renderer::EndFrame() {
    CompositeFrame();
    backend_->EndFrame();
}

void Renderer::SetCamera(const Camera& camera, float aspect) {
    ++sceneUniformStamp_; // B1: scene uniforms (view/proj/camPos) changed
    sceneState_.SetCamera(camera, aspect);
    // Step E: bake the frame's sub-pixel jitter into the projection so the
    // whole frame (scene, shadow cascades, depth pre-pass) shares one offset.
    if (taaEnabled_) sceneState_.SetProjectionJitter(jitterPixels_.x, jitterPixels_.y, hdrW_, hdrH_);
    // Render the cascade shadow maps now: they are sampled by the main-pass
    // draws that follow this SetCamera. Uses the previous frame's recorded
    // casters (one frame of staleness, imperceptible) and the current camera.
    // Guard mirrors RefreshShadowPass: an offscreen tool render (thumbnail,
    // model preview) that calls SetCamera must not consume-and-clear the
    // pending scene casters and re-render the cascades empty - that wiped the
    // shadows for the following main frame (surfaces popping lit/dark).
    // Recording()==false marks exactly those tool renders (they disable
    // caster recording), so a suppressed shadow pass here also stops the
    // model preview from overwriting the scene's cascades with ITS tiny
    // close-up camera every frame.
    if (shadowSystem_.Enabled() && shadowSystem_.Recording() &&
        !shadowSystem_.ShadowPassRanThisFrame() &&
        (shadowSystem_.HasRecordedCasters() || !shadowSystem_.MapsInitialized())) {
        shadowSystem_.RunPass(sceneState_.ActiveCamera(), sceneState_.ViewAspect(),
                              sceneState_.SunDir(), sceneState_.PointPos(),
                              sceneState_.PointRadius(), sceneState_.PointCount());
        // RunPass ends with BindDefaultTarget (shadow FBOs unbound); route the
        // main pass back into the HDR target when active.
        RebindMainTarget();
        // Both rebinds reset the backend viewport to the target's full size.
        // Restore the active scene viewport (a dock sub-rect in the editor)
        // so the main pass still rasterizes into the intended rect - hosts
        // that render into a sub-viewport (e.g. the 2D playtest) would
        // otherwise see the scene stretched/offset to the full window.
        // When render scaling is active the target is smaller than the window
        // and RebindMainTarget's full-target viewport is already the right one
        // (a full-window scene viewport scales to exactly that rect).
        RestoreSceneViewport();
    }
}

void Renderer::RefreshShadowPass() {
    // Re-run the cascade shadow pass for the current camera even when a shadow
    // pass already ran this frame (e.g. the editor pre-ran one with its free
    // orbit camera before play resolved the game camera). This keeps the light
    // frusta locked to the ACTUAL render view so orbiting the editor camera
    // slides the shadows incorrectly.
    if (!shadowSystem_.Enabled()) return;
    // The caster list is recorded by the MAIN pass (one frame of staleness) and
    // consumed by the first RunPass of a frame. A host calling this a second
    // time - or before any caster has ever been recorded - would otherwise clear
    // every cascade to 'far' with nothing drawn and wipe out the dynamic shadows
    // for the whole frame.
    if (!shadowSystem_.HasRecordedCasters() && shadowSystem_.MapsInitialized()) return;
    shadowSystem_.RunPass(sceneState_.ActiveCamera(), sceneState_.ViewAspect(),
                          sceneState_.SunDir(), sceneState_.PointPos(),
                          sceneState_.PointRadius(), sceneState_.PointCount());
    RebindMainTarget();
    if (draw2d_.SceneViewportActive() && EffectiveRenderScale() == 1.0f) {
        const math::Rect2& vp = draw2d_.SceneViewport();
        backend_->SetViewport(static_cast<int>(vp.x), static_cast<int>(vp.y),
                              static_cast<int>(vp.w), static_cast<int>(vp.h));
    }
}

void Renderer::SetSky(const Color& top, const Color& horizon) {
    sceneState_.SetSky(top, horizon);
}

void Renderer::SetIblStrength(float strength) {
    sceneState_.SetIblStrength(strength);
}

void Renderer::SetFog(const Color& color, float start, float end) {
    sceneState_.SetFog(color, start, end);
}

void Renderer::SetDirectionalLight(const math::Vec3& direction, const Color& color,
                                   float ambientStrength) {
    sceneState_.SetDirectionalLight(direction, color, ambientStrength);
}

void Renderer::SetAmbientLight(const Color& color, float strength) {
    sceneState_.SetAmbientLight(color, strength);
}

void Renderer::SetShadowsEnabled(bool enabled) {
    shadowSystem_.SetShadowsEnabled(enabled);
}

void Renderer::SetBloomEnabled(bool enabled) {
    bloomEnabled_ = enabled;
    if (!enabled)
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: bloom disabled");
}

void Renderer::SetBloomParams(float threshold, float strength) {
    bloomThreshold_ = threshold;
    bloomStrength_ = strength;
}

void Renderer::SetBloomWidth(float width) { bloomWidth_ = width > 0.0f ? width : 1.0f; }

void Renderer::SetLightProbes(TextureHandle atlas, const math::AABB& bounds, int res,
                              float maxIrradiance) {
    lightProbeAtlas_ = atlas;
    lightProbeBounds_ = bounds;
    lightProbeRes_ = res;
    lightProbeMaxIrr_ = maxIrradiance > 0.0f ? maxIrradiance : 1.0f;
    if (atlas.Valid()) NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                                    "Renderer: light-probe GI atlas bound (%dx%d grid, res=%d)",
                                    bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z, res);
}

bool Renderer::BakeLightProbes(const math::AABB& bounds, int res, const ProbeLightInput& input) {
    std::vector<IrradianceProbe> probes;
    if (BuildProbeField(bounds, res, input, probes) == 0) return false;
    // Scale the LDR atlas by the largest probe irradiance so the brightest probe
    // maps to 1.0; the shader multiplies back by the same factor.
    float maxIrr = 1e-4f;
    for (const IrradianceProbe& p : probes)
        maxIrr = std::max({maxIrr, std::fabs(p.irradiance.x), std::fabs(p.irradiance.y),
                           std::fabs(p.irradiance.z)});
    std::vector<uint8_t> atlas;
    if (BakeProbeAtlas(probes, res, bounds, maxIrr, atlas) == 0) return false;
    if (auto* backend = Backend()) {
        TextureDesc desc;
        desc.width = res;
        desc.height = res * res;
        desc.rgba = atlas.data();
        desc.filter = Filter::Linear;
        desc.wrap = Wrap::Clamp;
        const TextureHandle tex = backend->CreateTexture(desc);
        if (tex.Valid()) {
            SetLightProbes(tex, bounds, res, maxIrr);
            return true;
        }
    }
    return false;
}

void Renderer::SetExposure(float exposure) {
    if (exposure_ == exposure) return; // called every frame; log only on change
    exposure_ = exposure;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: composite exposure = %.3f", exposure_);
}

void Renderer::SetTonemapEnabled(bool enabled) {
    if (tonemapEnabled_ == enabled) return; // called every frame; log only on change
    tonemapEnabled_ = enabled;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: tonemap %s", enabled ? "enabled" : "disabled (legacy clamp)");
}

void Renderer::ResetAutoExposure() {
    if (!backend_) return;
    postGraph_.ResetAutoExposure(*backend_);
}

void Renderer::SetMsaaEnabled(bool enabled) {
    if (msaaRequested_ == enabled) return; // called every frame; log only on change
    msaaRequested_ = enabled;
    hdrW_ = -1; // force RebuildHdrTargets to apply the change this frame
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: MSAA %s", enabled ? "requested" : "disabled by flag");
}

void Renderer::SetMsaaSamples(int samples) {
    if (samples != 0 && samples != 2 && samples != 4 && samples != 8) samples = 4;
    if (samples == msaaSampleRequest_) return;
    msaaSampleRequest_ = samples;
    msaaSamples_ = 0; // force a fresh capability probe for the new count
    hdrW_ = -1;
}

// --- Step D: quality presets + render-scale / dynamic resolution ------------

const char* Renderer::QualityName() const {
    switch (quality_) {
        case Quality::Low: return "low";
        case Quality::Medium: return "medium";
        case Quality::Ultra: return "ultra";
        case Quality::High:
        default: return "high";
    }
}

void Renderer::SetQuality(Quality quality) {
    quality_ = quality;
    // Every knob set here is re-applied explicitly (not "only when it changes")
    // so switching presets is idempotent even after a script overrode one of
    // them. Individual setters remain usable afterwards.
    switch (quality) {
        case Quality::Low:
            SetMsaaEnabled(false);
            shadowSystem_.SetShadowSize(512);
            SetShadowDistance(60.0f);
            SetShadowSoftness(0.6f);
            SetSsaoEnabled(false);
            SetVolumetricEnabled(false);
            SetSsrEnabled(false);
            SetBloomEnabled(true);
            SetBloomParams(0.62f, 0.30f);
            SetBloomWidth(1.2f);
            SetRenderScale(0.75f);
            particleBudget_ = 8192;
            break;
        case Quality::Medium:
            SetMsaaEnabled(false);
            shadowSystem_.SetShadowSize(1024);
            SetShadowDistance(90.0f);
            SetShadowSoftness(1.0f);
            SetSsaoEnabled(true);
            SetSsaoIntensity(0.8f);
            SetVolumetricEnabled(false);
            SetSsrEnabled(false);
            SetBloomEnabled(true);
            SetBloomParams(0.58f, 0.30f);
            SetBloomWidth(1.4f);
            SetRenderScale(0.9f);
            particleBudget_ = 16384;
            break;
        case Quality::Ultra:
            SetMsaaEnabled(true);
            SetMsaaSamples(8);
            shadowSystem_.SetShadowSize(2048);
            SetShadowDistance(220.0f);
            SetShadowSoftness(1.4f);
            SetSsaoEnabled(true);
            SetSsaoIntensity(1.0f);
            SetVolumetricEnabled(true);
            SetVolumetricIntensity(1.0f);
            SetSsrEnabled(true);
            SetSsrIntensity(1.0f);
            SetBloomEnabled(true);
            SetBloomParams(0.55f, 0.34f);
            SetBloomWidth(2.0f);
            SetRenderScale(1.0f);
            SetTaaEnabled(true);
            SetTaaBlend(0.10f);
            SetTaaSharpen(0.55f);
            particleBudget_ = 65536;
            break;
        case Quality::High:
        default:
            SetMsaaEnabled(true);
            SetMsaaSamples(4);
            shadowSystem_.SetShadowSize(2048);
            SetShadowDistance(150.0f);
            SetShadowSoftness(1.0f);
            SetSsaoEnabled(true);
            SetSsaoIntensity(1.0f);
            SetVolumetricEnabled(true);
            SetVolumetricIntensity(1.0f);
            SetSsrEnabled(false);
            SetBloomEnabled(true);
            SetBloomParams(0.55f, 0.32f);
            SetBloomWidth(1.6f);
            SetRenderScale(1.0f);
            SetTaaEnabled(true);
            SetTaaBlend(0.12f);
            SetTaaSharpen(0.50f);
            particleBudget_ = 32768;
            break;
    }
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: quality preset '%s' (scale %.2f, msaa %s, shadow %d, particles %zu)",
                 QualityName(), renderScale_, msaaRequested_ ? "on" : "off", ShadowMapSize(),
                 particleBudget_);
}

void Renderer::SetRenderScale(float scale) {
    if (scale < 0.4f) scale = 0.4f;
    if (scale > 2.0f) scale = 2.0f;
    if (std::fabs(scale - renderScale_) < 1e-4f) return;
    renderScale_ = scale;
    if (dynScale_ > renderScale_) dynScale_ = renderScale_;
    hdrW_ = -1; // resize the HDR/post targets on the next BeginFrame
}

void Renderer::SetDynamicResolution(bool enabled, float targetFps, float minScale) {
    dynResEnabled_ = enabled;
    dynTargetFps_ = targetFps > 1.0f ? targetFps : 60.0f;
    dynMinScale_ = std::min(std::max(minScale, 0.4f), 1.0f);
    if (enabled && dynScale_ > renderScale_) dynScale_ = renderScale_;
}

void Renderer::SetTaaEnabled(bool enabled) {
    if (taaEnabled_ == enabled) return;
    taaEnabled_ = enabled;
    // Motion vectors only exist to feed the temporal resolve; toggling in
    // RebuildHdrTargets' early-out re-allocates/releases the target for us.
    // NEON_NO_VELOCITY forces the pass off for A/B screenshots (same role as
    // NEON_NO_DECAL_PROJECT / NEON_NO_BLOOM).
    static const bool velocityOff = std::getenv("NEON_NO_VELOCITY") != nullptr;
    velocityEnabled_ = enabled && !velocityOff;
    taaHistoryValid_ = false;   // never blend across the toggle
    taaFrame_ = 0;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: temporal AA %s (motion vectors %s)", enabled ? "enabled" : "disabled",
                 velocityEnabled_ ? "on" : "off");
}

void Renderer::SetTaaBlend(float blend) {
    taaBlend_ = std::min(std::max(blend, 0.02f), 0.6f);
}

void Renderer::SetTaaSharpen(float amount) {
    taaSharpen_ = std::min(std::max(amount, 0.0f), 1.5f);
    taa_.SetSharpen(taaSharpen_);
}

// Halton(2,3) low-discrepancy sub-pixel offsets: the same sequence a TAA
// implementation uses to turn N frames into Nx supersampling.
static math::Vec2 TaaJitterOffset(int index) {
    auto halton = [](int i, int base) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(i % base);
            i /= base;
        }
        return r;
    };
    const int i = index % 8 + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

void Renderer::ApplyTaa() {
    taaOutput_ = {};
    if (!taaEnabled_ || !backend_ || !hdrRT_.Valid()) return;
    if (!taa_.Ready() && !taa_.Init(*backend_, hdrW_, hdrH_)) return;
    // Camera-only reprojection needs the resolved main-pass depth, which only
    // exists on the MSAA HDR path; without it temporal AA stays off (logged once
    // by SetTaaEnabled's caller-side state).
    if (!EnsureSoftDepth()) return;
    taa_.SetSharpen(taaSharpen_);
    const TextureHandle depth = backend_->RenderTargetDepthTexture(hdrDepthRT_);
    if (!depth.Valid()) return;
    const bool valid = taaHistoryValid_ && prevViewProjValid_ &&
                       taa_.Width() == hdrW_ && taa_.Height() == hdrH_;
    taaOutput_ = taa_.Resolve(*backend_, hdrRT_, depth, sceneState_.ViewProjection().Inverse(),
                              prevViewProj_, postQuadMesh_, taaBlend_, valid,
                              VelocityTexture());
    prevViewProj_ = sceneState_.ViewProjection();
    prevViewProjValid_ = true;
    taaHistoryValid_ = taaOutput_.Valid();
    // Resolve left the history FBO bound; put the main target back for the post
    // graph (the composite binds the backbuffer itself).
    RebindMainTarget();
}

float Renderer::EffectiveRenderScale() const {
    // A scene viewport that is SMALLER than the window (the editor's docked
    // sub-rect) maps its pixels 1:1; scaling the HDR target would break that
    // mapping, so pin the scale. A full-window scene viewport (the player, which
    // sets the design-space rect every frame) scales cleanly.
    if (draw2d_.SceneViewportActive()) {
        const math::Rect2& vp = draw2d_.SceneViewport();
        if (vp.w < static_cast<float>(screenW_) - 0.5f ||
            vp.h < static_cast<float>(screenH_) - 0.5f)
            return 1.0f;
    }
    const float s = dynResEnabled_ ? std::min(dynScale_, renderScale_) : renderScale_;
    return std::min(std::max(s, 0.4f), 2.0f);
}

void Renderer::UpdateDynamicResolution() {
    // Frame-to-frame wall time is the only timing signal the backend exposes
    // (there is no GPU timer query). With vsync on, a GPU-bound frame still
    // stretches the swap interval, so this tracks missed budgets well enough to
    // steer the scale.
    const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    if (lastFrameClock_ != 0) {
        const double ms = static_cast<double>(ns - lastFrameClock_) / 1.0e6;
        if (ms > 0.05 && ms < 500.0) {
            frameTimeEmaMs_ = frameTimeEmaMs_ <= 0.0 ? ms : frameTimeEmaMs_ * 0.9 + ms * 0.1;
            // Raw (unsmoothed) wall-clock dt for frame-rate independent
            // adaptation curves (auto exposure).
            lastFrameDtSec_ = static_cast<float>(std::min(std::max(ms * 0.001, 0.0005), 0.1));
        }
    }
    lastFrameClock_ = ns;
    if (!dynResEnabled_ || frameTimeEmaMs_ <= 0.0) return;
    const double targetMs = 1000.0 / static_cast<double>(dynTargetFps_);
    float next = dynScale_;
    if (frameTimeEmaMs_ > targetMs * 1.08) next = dynScale_ * 0.94f;      // over budget
    else if (frameTimeEmaMs_ < targetMs * 0.90) next = dynScale_ * 1.02f; // headroom
    const float lo = std::min(dynMinScale_, renderScale_);
    const float hi = std::max(renderScale_, 0.4f);
    dynScale_ = std::min(std::max(next, lo), hi);
}

void Renderer::SetPointLight(int index, const math::Vec3& position, const Color& color,
                             float radius) {
    sceneState_.SetPointLight(index, position, color, radius);
}

void Renderer::SetPlayerLight(const math::Vec3& position, const Color& color, float radius) {
    sceneState_.SetPlayerLight(position, color, radius);
}

void Renderer::DrawSky() {
    // A4 procedural skybox: a view-ray fullscreen background pass with a sun
    // disc + halo, a locked moon and procedural clouds. Drawn into the CURRENT
    // scene target (HDR or backbuffer); the sky quad has depth test OFF and the
    // target was cleared to depth=1, so the sky at depth=1 never blocks the
    // geometry drawn on top. Replaces the old screen-space vertical gradient,
    // which did not follow the camera. When disabled (default) the legacy
    // gradient/HDRI path runs.
    draw2d_.Flush2D();
    if (skyBox_.enabled && skyboxShader_.Valid() && postQuadMesh_.Valid()) {
        backend_->UseShader(skyboxShader_);
        backend_->SetBlendMode(BlendMode::Opaque);
        backend_->SetDepthTest(false, false);
        backend_->SetCullMode(CullMode::None);
        backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
        // Reconstruct world rays with the inverse(view*proj) of the rendered
        // camera; the sky texture (optional HDRI) is sampled when valid.
        backend_->SetUniformMat4("uInvViewProj", sceneState_.ViewProjection().Inverted());
        const math::Vec3 camPos = sceneState_.CamPos();
        backend_->SetUniformVec3("uCamPos", camPos);
        const gfx::TextureHandle skyTex = sceneState_.SkyTexture();
        backend_->BindTexture(0, skyTex.Valid() ? skyTex : white_);
        backend_->SetUniformInt("uSkyTexture", 0);
        backend_->SetUniformInt("uSkyTextureValid", skyTex.Valid() ? 1 : 0);
        const Color& top = sceneState_.SkyTop();
        const Color& hor = sceneState_.SkyHorizon();
        backend_->SetUniformVec3("uSkyTop", {top.r, top.g, top.b});
        backend_->SetUniformVec3("uSkyHorizon", {hor.r, hor.g, hor.b});
        backend_->SetUniformFloat("uSunYaw", skyBox_.sunYaw);
        backend_->SetUniformFloat("uSunPitch", skyBox_.sunPitch);
        backend_->SetUniformInt("uSunVisible", skyBox_.sunVisible ? 1 : 0);
        backend_->SetUniformInt("uMoonVisible", skyBox_.moonVisible ? 1 : 0);
        backend_->SetUniformInt("uCloudsEnabled", skyBox_.cloudsEnabled ? 1 : 0);
        backend_->SetUniformFloat("uCloudCoverage", skyBox_.cloudCoverage);
        backend_->SetUniformFloat("uCloudScale", skyBox_.cloudScale);
        backend_->SetUniformFloat("uTime", skyTime_);
        backend_->DrawMesh(postQuadMesh_);
        return;
    }
    sceneState_.DrawSky(draw2d_);
}

void Renderer::EnableSkyBox(const math::Vec3& sunDir, bool clouds) {
    skyBox_.enabled = true;
    skyBox_.cloudsEnabled = clouds;
    // Derive yaw/pitch (radians) from a world sun direction so the disc matches
    // the directional light. sunDir points AWAY from the sun (light travels its
    // negative), so negate to get "toward the sun".
    const math::Vec3 s = (-sunDir).Normalized();
    skyBox_.sunPitch = std::asin(std::max(-1.0f, std::min(1.0f, s.y)));
    skyBox_.sunYaw = std::atan2(s.z, s.x);
}

void Renderer::DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model) {
    DrawMesh(mesh, material, model, nullptr);
}

void Renderer::DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model,
                        const math::Mat4* prevModel) {
    if (!mesh.Valid()) return;
    Flush2D();

    // Record shadow/SSAO casters BEFORE the camera frustum cull. A caster that
    // is off-screen (behind or beside the view, but lying along the light
    // direction) still casts into the visible region; culling it here made the
    // shadows depend on the view direction - "shadows only on one side",
    // "turning toward the light there is no shadow". The shadow pass consumes
    // this list, so it must not be limited to the main camera's view.
    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent &&
        material.castShadow)
        shadowSystem_.RecordCaster({mesh.Handle(), model, {}, {}, 0, mesh.Bounds()});
    // SSAO/SSR have their own colour-encoded depth pre-pass and do NOT depend
    // on CSM being enabled: collect the caster whenever one is active.
    // The SSAO/SSR colour-depth pre-pass only needs the caster list when the
    // main-pass depth cannot be resolved for the post chain (no MSAA / a backend
    // without ResolveDepth). Collecting otherwise is pure CPU waste.
    if ((ssaoEnabled_ || ssrEnabled_) && shadowSystem_.Recording() && !material.transparent &&
        !PostDepthReuseOk())
        ssaoCasters_.push_back({mesh.Handle(), model, {}, {}, 0, mesh.Bounds()});

    if (sceneState_.FrustumValid() &&
        !sceneState_.Frustum().Intersects(math::TransformAABB(mesh.Bounds(), model)))
        return;

    // Step E2: a moving object also rasterizes a motion vector for the TAA
    // resolve. Submitted before the material path so the colour draw afterwards
    // re-establishes every piece of state the velocity pass touched.
    // Transparent materials are excluded: the velocity pass rasterises opaque
    // with depth-write, so a moving translucent card would claim the velocity
    // of the pixels it covers (its own alpha-blended colour) and ghost the
    // moving object behind it; those surfaces fall back to depth reprojection.
    if (prevModel && velocityEnabled_ && velocityRT_.Valid() && velocityShader_.Valid() &&
        !material.transparent)
        SubmitVelocity(mesh, model, *prevModel);

    // Depth-projected decal: bypass the material path (no shadow/SSAO caster
    // recording, custom state). Falls through to the plain unlit quad when the
    // frame has no sampleable scene depth. NEON_NO_DECAL_PROJECT forces the flat
    // quad for A/B screenshots (same role as NEON_NO_BLOOM / NEON_NO_SHADOWS).
    static const bool decalProjectOff = std::getenv("NEON_NO_DECAL_PROJECT") != nullptr;
    if (material.decal && !decalProjectOff && decalShader_.Valid() && EnsureSoftDepth()) {
        DrawDecal(mesh, material, model);
        return;
    }

    ShaderHandle shader = material.shader.Valid() ? material.shader
                                                  : (material.lit ? litShader_ : unlitShader_);
    ApplyMaterial(material, sceneState_.ViewProjection() * model, model, NormalMatrix(model),
                  shader);
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

TextureHandle Renderer::VelocityTexture() const {
    if (!backend_ || !velocityRT_.Valid()) return {};
    return backend_->RenderTargetColorTexture(velocityRT_);
}

void Renderer::SubmitVelocity(const Mesh& mesh, const math::Mat4& model,
                              const math::Mat4& prevModel) {
    // Motion vectors are only RECORDED here; the whole batch is rasterized by
    // one pass per frame (FlushVelocityPass). Binding velocityRT_ per object
    // used to switch targets twice per moving object, and every switch flushes
    // and submits the command buffer in the Vulkan backend - two GPU stalls
    // and one dropped pending clear per object, per frame.
    if (!velocityEnabled_ || !velocityRT_.Valid() || !velocityShader_.Valid()) return;
    velocityBatch_.push_back({mesh.Handle(), model, prevModel});
}

void Renderer::FlushVelocityPass() {
    if (velocityBatch_.empty()) return;
    // Swap the batch out first so a draw inside the pass can never re-enter it.
    std::vector<VelocityDraw> batch;
    batch.swap(velocityBatch_);
    if (!hdrEnabled_ || hdrW_ <= 0 || hdrH_ <= 0) return;
    if (!velocityEnabled_ || !velocityRT_.Valid() || !velocityShader_.Valid()) return;
    backend_->BindRenderTarget(velocityRT_);
    backend_->SetViewport(0, 0, hdrW_, hdrH_);
    // The pass owns this target: clear colour (alpha 0 = 'no moving object
    // covers this pixel', so the TAA resolve falls back to depth
    // reprojection) and depth (a stale depth rejects the whole batch).
    backend_->Clear({0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
    backend_->SetBlendMode(BlendMode::Opaque);
    // Depth test/write on so the nearest object owns the pixel regardless of the
    // draw order (the colour pass sorts opaques front-to-back, but transparency
    // and per-entity paths do not).
    backend_->SetDepthTest(true, true);
    backend_->SetCullMode(CullMode::Back);
    backend_->UseShader(velocityShader_);
    // Same view-projection pair the TAA resolve uses: both are the frame's
    // JITTERED matrices, so the offset lands on the right texel of the jittered
    // history buffer.
    const math::Mat4 viewProj = sceneState_.ViewProjection();
    for (const VelocityDraw& d : batch) {
        backend_->SetUniformMat4("uViewProj", viewProj);
        backend_->SetUniformMat4("uPrevViewProj", prevViewProj_);
        backend_->SetUniformMat4("uModel", d.model);
        backend_->SetUniformMat4("uPrevModel", d.prevModel);
        backend_->DrawMesh(d.mesh);
        ++stats_.velocityDraws;
    }
    // Route the main pass back into the HDR target: everything drawn after this
    // point (including this frame's post chain) must not land in the velocity
    // buffer. Also restores a docked sub-viewport, since binding a target resets
    // the viewport to its full size.
    RebindMainTarget();
    RestoreSceneViewport();
}

void Renderer::RestoreSceneViewport() {
    if (draw2d_.SceneViewportActive() && EffectiveRenderScale() == 1.0f) {
        const math::Rect2& vp = draw2d_.SceneViewport();
        backend_->SetViewport(static_cast<int>(vp.x), static_cast<int>(vp.y),
                              static_cast<int>(vp.w), static_cast<int>(vp.h));
    }
}

void Renderer::DrawDecal(const Mesh& mesh, const Material& material, const math::Mat4& model) {
    const math::Mat4 viewProj = sceneState_.ViewProjection();
    ApplyMaterial(material, viewProj * model, model, NormalMatrix(model), decalShader_);
    // ApplyMaterial only uploads uModel for lit materials; the decal VS needs it
    // (and the inverse maps the depth-rebuilt world point into decal-local space).
    backend_->SetUniformMat4("uModel", model);
    backend_->SetUniformMat4("uDecalInvModel", model.Inverted());
    backend_->SetUniformMat4("uInvViewProj", viewProj.Inverse());
    backend_->SetUniformInt("uDecalProject", 1);
    backend_->SetUniformInt("uDecalAdditive", material.additive ? 1 : 0);
    // World-unit depth bias (linearised against the camera near/far inside the
    // shader): large enough to beat resolved-depth vs per-sample MSAA
    // differences at silhouette edges, small enough that the decal never pokes
    // through geometry in front of it. The old fixed window-depth bias grew
    // with camera distance squared (~1 world unit at gameplay range) and
    // painted decals over the legs of units standing in them.
    backend_->SetUniformFloat("uDecalBias", 0.05f);
    const Camera& cam = sceneState_.ActiveCamera();
    backend_->SetUniformFloat("uNear", cam.nearPlane);
    backend_->SetUniformFloat("uFar", cam.farPlane);
    const TextureHandle sceneDepth = backend_->RenderTargetDepthTexture(hdrDepthRT_);
    // Unit 24 (0..4 material maps, 5..19 shadows, 20..22 IBL, 23 normal map):
    // reusing the IBL unit would leave a stale depth binding behind when the
    // cached scene-uniform upload is skipped for the next lit draw.
    backend_->BindTexture(24, sceneDepth);
    backend_->SetUniformInt("uSceneDepth", 24);
    backend_->SetUniformVec2(
        "uScreenSize", {static_cast<float>(hdrW_), static_cast<float>(hdrH_)});
    // One quad lies inside the projection volume, so exactly one fragment per
    // pixel is shaded (no double blending). Depth test on so a unit standing in
    // front still occludes the decal; no depth write so it never hides what is
    // behind it.
    backend_->SetCullMode(CullMode::None);
    backend_->SetDepthTest(sceneState_.DepthAvailable(), false);
    backend_->SetBlendMode(material.additive ? BlendMode::Additive : BlendMode::Alpha);
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

void Renderer::DrawSkinnedMesh(const Mesh& mesh, const Material& material,
                               const math::Mat4& model,
                               const std::vector<math::Mat4>& boneMatrices, int boneCount) {
    DrawSkinnedMesh(mesh, material, model, boneMatrices, boneCount, nullptr);
}

void Renderer::DrawSkinnedMesh(const Mesh& mesh, const Material& material,
                               const math::Mat4& model,
                               const std::vector<math::Mat4>& boneMatrices, int boneCount,
                               const math::Mat4* prevModel) {
    if (!mesh.Valid()) return;
    Flush2D();

    // Record the caster BEFORE the frustum cull and honour material.castShadow,
    // mirroring DrawMesh: an off-screen character (behind/beside the view but
    // along the light direction) still casts into the visible region, and a
    // material authored castShadow=false must not write shadow depth.
    int count = boneCount >= 0 ? std::min(boneCount, static_cast<int>(boneMatrices.size()))
                               : static_cast<int>(boneMatrices.size());
    count = std::min(count, 128);
    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent &&
        material.castShadow)
        shadowSystem_.RecordCaster({mesh.Handle(), model, {}, boneMatrices, count, mesh.Bounds()});
    if ((ssaoEnabled_ || ssrEnabled_) && shadowSystem_.Recording() && !material.transparent &&
        !PostDepthReuseOk())
        ssaoCasters_.push_back({mesh.Handle(), model, {}, boneMatrices, count, mesh.Bounds()});

    if (sceneState_.FrustumValid() &&
        !sceneState_.Frustum().Intersects(math::TransformAABB(mesh.Bounds(), model)))
        return;

    // Step E2: entity-level motion vector (rest-pose geometry through the current
    // and previous entity transform). Per-bone deformation is not captured yet,
    // so a limb swinging in place still relies on the neighbourhood clamp.
    if (prevModel && velocityEnabled_ && velocityRT_.Valid() && velocityShader_.Valid())
        SubmitVelocity(mesh, model, *prevModel);

    ShaderHandle shader = material.shader.Valid() ? material.shader : skinnedLitShader_;
    ApplyMaterial(material, sceneState_.ViewProjection() * model, model, NormalMatrix(model),
                  shader);

    if (count > 0) {
        boneUniformFlat_.resize(static_cast<size_t>(count) * 16);
        for (int i = 0; i < count; ++i)
            std::memcpy(boneUniformFlat_.data() + static_cast<size_t>(i) * 16,
                        boneMatrices[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", boneUniformFlat_.data(), count);
    }
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

void Renderer::DrawMeshInstanced(const Mesh& mesh, const Material& material,
                                 const math::Mat4* models, uint32_t count, bool frustumCull) {
    if (!mesh.Valid() || !models || count == 0) return;
    Flush2D();

    instancedVisible_.clear();
    instancedVisible_.reserve(count);
    const math::AABB& bounds = mesh.Bounds();
    for (uint32_t i = 0; i < count; ++i) {
        if (frustumCull && sceneState_.FrustumValid() &&
            !sceneState_.Frustum().Intersects(math::TransformAABB(bounds, models[i]))) {
            continue;
        }
        instancedVisible_.push_back(models[i]);
    }
    if (instancedVisible_.empty()) return;

    // Opaque, depth-tested instances of one mesh+material: submit them
    // front-to-back so the nearest fragments fill the depth buffer first and the
    // remaining ones are rejected before shading. The rendered result is
    // identical (depth test), only the overdraw drops -- the Rift's terrain
    // chunks and the instanced vegetation fields are the big winners.
    if (!material.transparent && instancedVisible_.size() > 1) {
        const math::Vec3 camPos = sceneState_.CamPos();
        std::stable_sort(instancedVisible_.begin(), instancedVisible_.end(),
                         [&camPos](const math::Mat4& a, const math::Mat4& b) {
                             const float da = math::Distance(
                                 math::Vec3{a.m[3], a.m[7], a.m[11]}, camPos);
                             const float db = math::Distance(
                                 math::Vec3{b.m[3], b.m[7], b.m[11]}, camPos);
                             return da < db;
                         });
    }

    // Instances are still recorded from the frustum-visible subset only: veg
    // fields batch thousands of instances and the off-screen ones are the
    // cheap-to-skip half. material.castShadow IS honoured, as in DrawMesh.
    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent &&
        material.castShadow)
        shadowSystem_.RecordCaster(
            {mesh.Handle(), math::Mat4::Identity(), instancedVisible_, {}, 0, mesh.Bounds()});
    if ((ssaoEnabled_ || ssrEnabled_) && shadowSystem_.Recording() && !material.transparent &&
        !PostDepthReuseOk())
        ssaoCasters_.push_back(
            {mesh.Handle(), math::Mat4::Identity(), instancedVisible_, {}, 0, mesh.Bounds()});

    ShaderHandle shader = material.shader.Valid()
                              ? material.shader
                              : (material.lit ? litInstancedShader_ : unlitInstancedShader_);
    ApplyMaterial(material, sceneState_.ViewProjection(), math::Mat4::Identity(),
                  math::Mat4::Identity(), shader);
    backend_->DrawMeshInstanced(mesh.Handle(), instancedVisible_.data(),
                                static_cast<uint32_t>(instancedVisible_.size()));
    ++stats_.drawCalls;
    stats_.instances += static_cast<uint32_t>(instancedVisible_.size());
    stats_.triangles += mesh.TriangleCount() * static_cast<uint32_t>(instancedVisible_.size());
}

void Renderer::DrawMeshInstancedColored(const Mesh& mesh, const Material& material,
                                        const math::Mat4* models, const math::Vec4* colors,
                                        uint32_t count, bool frustumCull) {
    if (!mesh.Valid() || !models || !colors || count == 0) return;
    Flush2D();
    instancedVisible_.clear();
    instancedVisibleColored_.clear();
    instancedVisible_.reserve(count);
    instancedVisibleColored_.reserve(count);
    const math::AABB& bounds = mesh.Bounds();
    for (uint32_t i = 0; i < count; ++i) {
        if (frustumCull && sceneState_.FrustumValid() &&
            !sceneState_.Frustum().Intersects(math::TransformAABB(bounds, models[i]))) {
            continue;
        }
        instancedVisible_.push_back(models[i]);
        instancedVisibleColored_.push_back(colors[i]);
    }
    if (instancedVisible_.empty()) return;

    ShaderHandle shader =
        material.shader.Valid() ? material.shader : unlitInstancedColoredShader_;
    ApplyMaterial(material, sceneState_.ViewProjection(), math::Mat4::Identity(),
                  math::Mat4::Identity(), shader);
    backend_->DrawMeshInstancedColored(mesh.Handle(), instancedVisible_.data(),
                                       instancedVisibleColored_.data(),
                                       static_cast<uint32_t>(instancedVisible_.size()));
    ++stats_.drawCalls;
    stats_.instances += static_cast<uint32_t>(instancedVisible_.size());
    stats_.triangles +=
        mesh.TriangleCount() * static_cast<uint32_t>(instancedVisible_.size());
}

void Renderer::DrawBillboards(const math::Vec3* positions, const float* sizes,
                              const Color* colors, TextureHandle texture, uint32_t count,
                              BlendMode blend, float intensity, const float* rotations,
                              const math::Vec4* uvRects) {
    if (!backend_ || !billboardQuad_.Valid() || !positions || !sizes || !colors || count == 0)
        return;
    Flush2D();

    // Camera-facing billboard basis (right/up from the camera frame).
    const Camera& cam = sceneState_.ActiveCamera();
    math::Vec3 fwd = cam.target - cam.position;
    if (fwd.LengthSq() < 1e-6f) fwd = {0.0f, 0.0f, -1.0f};
    fwd = fwd.Normalized();
    math::Vec3 up0 = cam.up;
    if (up0.LengthSq() < 1e-6f) up0 = {0.0f, 1.0f, 0.0f};
    math::Vec3 right = math::Cross(fwd, up0);
    if (right.LengthSq() < 1e-6f) right = {1.0f, 0.0f, 0.0f};
    right = right.Normalized();
    const math::Vec3 up = math::Cross(right, fwd).Normalized();

    // Per-frame scratch (reused across calls): particles rebuild these streams
    // every frame, so a fresh allocation per burst was pure heap churn.
    billboardModels_.resize(static_cast<size_t>(count));
    billboardColors_.resize(static_cast<size_t>(count));
    billboardUvRects_.resize(static_cast<size_t>(count));
    math::Mat4* models = billboardModels_.data();
    math::Vec4* colc = billboardColors_.data();
    math::Vec4* uvs = billboardUvRects_.data();
    for (uint32_t i = 0; i < count; ++i) {
        const float s = sizes[i];
        // Per-particle roll: rotate the camera-facing basis in the quad's own
        // plane. Baking it into the instance matrix keeps the shader (and both
        // backends) untouched.
        math::Vec3 r = right;
        math::Vec3 u = up;
        if (rotations && rotations[i] != 0.0f) {
            const float ca = std::cos(rotations[i]);
            const float sa = std::sin(rotations[i]);
            r = right * ca + up * sa;
            u = up * ca - right * sa;
        }
        math::Mat4 m; // identity; fill the basis columns below
        // Local +X -> camera right, +Y -> camera up, +Z -> toward camera.
        m.m[0] = r.x * s;  m.m[4] = r.y * s;  m.m[8] = r.z * s;
        m.m[1] = u.x * s;  m.m[5] = u.y * s;  m.m[9] = u.z * s;
        m.m[2] = -fwd.x * s;   m.m[6] = -fwd.y * s;   m.m[10] = -fwd.z * s;
        m.m[3] = positions[i].x;
        m.m[7] = positions[i].y;
        m.m[11] = positions[i].z;
        models[i] = m;
        uvs[i] = uvRects ? uvRects[i] : math::Vec4{0.0f, 0.0f, 1.0f, 1.0f};
        // Multiply RGB by `intensity` so additive glow particles emit HDR
        // values > 1.0 and the bloom pass picks them up (the "big game" glow).
        colc[i] = {colors[i].r * intensity, colors[i].g * intensity,
                   colors[i].b * intensity, colors[i].a};
    }

    Material mat = Material::Unlit(texture, Color::White);
    mat.transparent = true; // pick any; blend mode is overridden below
    mat.doubleSided = true;
    // Soft particles: when a sampleable scene depth is available this frame,
    // use the depth-faded billboard shader so glow quads fade where they cross
    // geometry (no hard intersection line). Falls back cleanly otherwise.
    const bool soft = particleSoftShader_.Valid() && EnsureSoftDepth();
    // Both billboard programs take the per-instance UV rectangle; the plain
    // instanced-coloured program (the fallback when a backend could not build
    // the atlas variant) does not, so it draws without the UV remap.
    const bool atlas = soft || particleShader_.Valid();
    const ShaderHandle shader = soft ? particleSoftShader_
                                     : (atlas ? particleShader_ : unlitInstancedColoredShader_);
    ApplyMaterial(mat, sceneState_.ViewProjection(), math::Mat4::Identity(),
                  math::Mat4::Identity(), shader);
    // Particles blend appropriately and respect the scene depth (unlike the
    // screen-space DrawBillboard helper which is depth-unaware).
    backend_->SetBlendMode(blend);
    backend_->SetDepthTest(sceneState_.DepthAvailable(), false);
    backend_->SetCullMode(CullMode::None);
    if (soft) {
        backend_->BindTexture(22, backend_->RenderTargetDepthTexture(hdrDepthRT_));
        backend_->SetUniformInt("uSceneDepth", 22);
        backend_->SetUniformVec2("uScreenSize",
                                 {static_cast<float>(hdrW_), static_cast<float>(hdrH_)});
        // uSoftFade is in world units: the shader linearises the raw scene
        // depth (uNear/uFar) and compares against the particle's view-axis
        // distance, so the fade band no longer grows with camera distance^2.
        backend_->SetUniformFloat("uNear", cam.nearPlane);
        backend_->SetUniformFloat("uFar", cam.farPlane);
        backend_->SetUniformFloat("uSoftFade", softFadeRange_);
    }
    if (atlas)
        backend_->DrawMeshInstancedColoredUv(billboardQuad_.Handle(), models, colc, uvs, count);
    else
        backend_->DrawMeshInstancedColored(billboardQuad_.Handle(), models, colc, count);
    ++stats_.drawCalls;
    stats_.instances += count;
    stats_.triangles += billboardQuad_.TriangleCount() * count;
}

void Renderer::DrawProjectedShadowVerts(const std::vector<Vertex3D>& verts,
                                        const std::vector<uint16_t>& indices,
                                        const math::Mat4& model, const math::Vec3& lightDir,
                                        const Color& color) {
    if (verts.empty() || indices.size() < 3 || std::fabs(lightDir.y) < 1e-4f) return;

    projectedShadowVerts_.clear();
    projectedShadowVerts_.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        math::Vec3 w0 = model.TransformPoint(verts[indices[i]].pos);
        math::Vec3 w1 = model.TransformPoint(verts[indices[i + 1]].pos);
        math::Vec3 w2 = model.TransformPoint(verts[indices[i + 2]].pos);
        if (w0.y < 0.02f && w1.y < 0.02f && w2.y < 0.02f) continue; // below ground
        auto projectToGround = [&](const math::Vec3& p) {
            float t = -p.y / lightDir.y;
            return p + lightDir * t;
        };
        math::Vec3 p0 = projectToGround(w0);
        math::Vec3 p1 = projectToGround(w1);
        math::Vec3 p2 = projectToGround(w2);
        projectedShadowVerts_.push_back({p0, color});
        projectedShadowVerts_.push_back({p1, color});
        projectedShadowVerts_.push_back({p2, color});
    }
    if (projectedShadowVerts_.empty()) return;

    Flush2D();
    backend_->SetBlendMode(BlendMode::Alpha);
    backend_->SetDepthTest(false, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(linesShader_);
    backend_->SetUniformMat4("uMVP", sceneState_.ViewProjection());
    backend_->DrawPrimitives(projectedShadowVerts_.data(),
                             static_cast<uint32_t>(projectedShadowVerts_.size()), 28, nullptr, 0,
                             PrimitiveTopology::Triangles);
}

void Renderer::DrawProjectedShadow(const Mesh& mesh, const math::Mat4& model,
                                   const math::Vec3& lightDir, const Color& color) {
    if (!mesh.Valid()) return;
    DrawProjectedShadowVerts(mesh.CpuVerts(), mesh.CpuIndices(), model, lightDir, color);
}

void Renderer::DrawProjectedShadowSkinned(const Mesh& mesh, const math::Mat4& model,
                                          const std::vector<math::Mat4>& bones, int boneCount,
                                          const math::Vec3& lightDir, const Color& color) {
    if (!mesh.Valid()) return;
    const std::vector<Vertex3D>& src = mesh.CpuVerts();
    const std::vector<uint16_t>& indices = mesh.CpuIndices();
    if (!mesh.Skinned() || src.empty() || boneCount <= 0) {
        DrawProjectedShadow(mesh, model, lightDir, color);
        return;
    }

    std::vector<Vertex3D> skinned = src;
    for (size_t i = 0; i < src.size(); ++i) {
        const Vertex3D& v = src[i];
        math::Vec3 p{0, 0, 0};
        math::Vec3 n{0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            float w = v.w[k];
            if (w == 0.0f) continue;
            int j = static_cast<int>(v.j[k]);
            if (j < 0 || j >= boneCount) continue;
            const math::Mat4& bm = bones[static_cast<size_t>(j)];
            p += bm.TransformPoint(v.pos) * w;
            n += bm.TransformDir(v.normal) * w;
        }
        skinned[i].pos = p;
        skinned[i].normal = n.LengthSq() > 1e-8f ? n.Normalized() : v.normal;
    }
    DrawProjectedShadowVerts(skinned, indices, model, lightDir, color);
}

void Renderer::ApplyMaterial(const Material& material, const math::Mat4& mvp,
                             const math::Mat4& model, const math::Mat4& normalMat,
                             ShaderHandle shader) {
    backend_->UseShader(shader);
    backend_->SetCullMode(material.doubleSided ? CullMode::None : CullMode::Back);
    // Alpha-blended geometry keeps the DEPTH TEST (fragments behind opaque
    // geometry are still rejected - without it every fur shell / particle
    // layer stacks from every angle into a dark smear) and only disables the
    // depth WRITE so blended pixels do not occlude later draws.
    backend_->SetDepthTest(sceneState_.DepthAvailable() && material.depthTest,
                           !material.transparent);
    backend_->SetBlendMode(material.transparent
                               ? (material.additive ? BlendMode::Additive : BlendMode::Alpha)
                               : BlendMode::Opaque);

    backend_->SetUniformMat4("uMVP", mvp);
    backend_->SetUniformVec2("uTiling", {material.uvRepeat, material.uvRepeat});
    backend_->BindTexture(0, material.albedo.Valid() ? material.albedo : white_);
    backend_->SetUniformInt("uAlbedo", 0);
    // G4 terrain splatmap: bind the grass texture + dirt/rock colors when the
    // terrain shader variant is active (other shaders ignore these uniforms).
    backend_->BindTexture(1, material.grassTex.Valid() ? material.grassTex : white_);
    backend_->SetUniformInt("uGrassTex", 1);
    backend_->SetUniformInt("uHasGrassTex", material.grassTex.Valid() ? 1 : 0);
    backend_->SetUniformVec4("uDirtColor",
                             {material.dirtColor.r, material.dirtColor.g,
                              material.dirtColor.b, material.dirtColor.a});
    backend_->SetUniformVec4("uRockColor",
                             {material.rockColor.r, material.rockColor.g,
                              material.rockColor.b, material.rockColor.a});
    backend_->SetUniformInt("uHasTexture", material.albedo.Valid() ? 1 : 0);
    // Alpha cutout: the lit shader discards albedo.a < uAlphaTest when > 0.
    // alphaTest survives only when there is an albedo to cut against and the
    // material requests it (glTF MASK foliage cards + procedurally-tufted grass).
    backend_->SetUniformFloat("uAlphaTest", material.alphaTest ? material.alphaCutoff : 0.0f);
    backend_->SetUniformInt("uHasMR", material.metallicRoughness.Valid() ? 1 : 0);
    backend_->SetUniformInt("uHasAO", material.occlusion.Valid() ? 1 : 0);
    backend_->SetUniformInt("uHasEmissive", material.emissive.Valid() ? 1 : 0);
    backend_->SetUniformFloat("uAOStrength", material.aoStrength);
    backend_->SetUniformFloat("uEmissiveIntensity", material.emissiveIntensity);
    backend_->SetUniformVec4("uTint", {material.tint.r, material.tint.g, material.tint.b, material.tint.a});
    backend_->SetUniformFloat("uMetallic", material.metallic);
    backend_->SetUniformFloat("uRoughness", material.roughness);
    backend_->BindTexture(2, material.metallicRoughness);
    backend_->SetUniformInt("uMR", 2);
    backend_->BindTexture(3, material.occlusion);
    backend_->SetUniformInt("uOcclusion", 3);
    backend_->BindTexture(4, material.emissive);
    backend_->SetUniformInt("uEmissive", 4);
    // A2 normal map: bound on texture unit 23 (20..22 are the IBL irradiance/
    // prefiltered/BRDF-LUT maps, 5..7 CSM shadows, 8..19 point shadows) and
    // disabled by default so the Lit shader samples it only when the material
    // carries one. Perturbation strength is authored per material.
    backend_->BindTexture(23, material.normalMap.Valid() ? material.normalMap : white_);
    backend_->SetUniformInt("uNormalMap", 23);
    backend_->SetUniformInt("uHasNormalMap", material.normalMap.Valid() ? 1 : 0);
    backend_->SetUniformFloat("uNormalScale", material.normalScale);
    backend_->SetUniformInt("uReceiveShadow", material.receiveShadow ? 1 : 0);
    // Diagnostic view (NEON_SHADOW_DEBUG=1: raw cascade shadow factor, 2:
    // cascade index colours) replaces the shaded colour. Resolved once per
    // frame: getenv in a per-draw path would show up in a profile.
    if (shadowDebug_ < 0) {
        const char* dbg = std::getenv("NEON_SHADOW_DEBUG");
        shadowDebug_ = dbg ? atoi(dbg) : 0;
    }
    backend_->SetUniformInt("uShadowDebug", shadowDebug_);

    if (material.lit) {
        backend_->SetUniformMat4("uModel", model);
        backend_->SetUniformMat4("uNormalMat", normalMat);
        backend_->SetUniformFloat("uShininess", material.shininess);
        // Selection / edge glow (no-op in shaders compiled without the uniforms,
        // e.g. the unlit variant; the lit + skinned-lit programs carry them).
        backend_->SetUniformVec3("uHighlightColor",
                                 {material.highlightColor.r, material.highlightColor.g,
                                  material.highlightColor.b});
        backend_->SetUniformFloat("uHighlightStrength", material.highlightStrength);
        // B1: the per-frame scene uniform block (sun/lights/fog/view/shadow/IBL)
        // is identical across every draw in a frame -- upload it once, and
        // re-upload whenever the shader changes (uniform locations are
        // per-program).
        if (sceneUniformStamp_ != sceneUniformAppliedStamp_ ||
            shader.id != lastSceneUniformShader_.id) {
            sceneUniformAppliedStamp_ = sceneUniformStamp_;
            ApplySceneUniforms(shader);
        }
    }
}

void Renderer::ApplySceneUniforms(ShaderHandle shader) {
    lastSceneUniformShader_ = shader;
    backend_->SetUniformVec3("uCamPos", sceneState_.CamPos());
    backend_->SetUniformVec3("uSunDir", sceneState_.SunDir());
    const Color& sunColor = sceneState_.SunColor();
    backend_->SetUniformVec3("uSunColor", {sunColor.r, sunColor.g, sunColor.b});
    backend_->SetUniformFloat("uAmbient", sceneState_.Ambient());
    const Color& ambientColor = sceneState_.AmbientColor();
    backend_->SetUniformVec3("uAmbientColor",
                             {ambientColor.r, ambientColor.g, ambientColor.b});
    const Color& groundColor = sceneState_.AmbientGroundColor();
    backend_->SetUniformVec3("uAmbientGroundColor",
                             {groundColor.r, groundColor.g, groundColor.b});
    backend_->SetUniformInt("uPointCount", sceneState_.PointCount());
    for (int i = 0; i < sceneState_.PointCount(); ++i) {
        std::string suffix = "[" + std::to_string(i) + "]";
        backend_->SetUniformVec3(("uPointPos" + suffix).c_str(), sceneState_.PointPos()[i]);
        const Color& pc = sceneState_.PointColor()[i];
        backend_->SetUniformVec3(("uPointColor" + suffix).c_str(), {pc.r, pc.g, pc.b});
        backend_->SetUniformFloat(("uPointRadius" + suffix).c_str(), sceneState_.PointRadius()[i]);
    }
    backend_->SetUniformVec3("uPlayerLightPos", sceneState_.PlayerLightPos());
    const Color& plc = sceneState_.PlayerLightColor();
    backend_->SetUniformVec3("uPlayerLightColor",
                             {plc.r, plc.g, plc.b});
    backend_->SetUniformFloat("uPlayerLightRadius", sceneState_.PlayerLightRadius());
    backend_->SetUniformInt("uPlayerLightEnabled", sceneState_.PlayerLightEnabled() ? 1 : 0);
    const Color& fogColor = sceneState_.FogColor();
    backend_->SetUniformVec3("uFogColor", {fogColor.r, fogColor.g, fogColor.b});
    backend_->SetUniformFloat("uFogStart", sceneState_.FogStart());
    backend_->SetUniformFloat("uFogEnd", sceneState_.FogEnd());
    backend_->SetUniformMat4("uViewMatrix", sceneState_.View());
    {
        float flatVP[3 * 16];
        for (int i = 0; i < ShadowSystem::kShadowCascades; ++i)
            std::memcpy(flatVP + i * 16, shadowSystem_.LightViewProj()[i].Data(),
                        16 * sizeof(float));
        backend_->SetUniformMat4Array("uLightVP", flatVP, ShadowSystem::kShadowCascades);
    }
    const float* splits = shadowSystem_.CascadeSplits();
    backend_->SetUniformVec4("uCascadeSplits",
                             {splits[1], splits[2], splits[3], splits[0]});
    backend_->SetUniformVec2("uShadowTexel",
                             {1.0f / static_cast<float>(shadowSystem_.ShadowSize()),
                              1.0f / static_cast<float>(shadowSystem_.ShadowSize())});
    {
        const float* texelWorld = shadowSystem_.CascadeTexelWorld();
        backend_->SetUniformVec3("uShadowTexelWorld",
                                 {texelWorld[0], texelWorld[1], texelWorld[2]});
    }
    backend_->SetUniformFloat("uShadowSoftness", shadowSoftness_);
    backend_->SetUniformFloat("uShadowNormalOffset", shadowNormalOffset_);
    backend_->SetUniformInt("uShadowEnabled", shadowSystem_.CsmActive() ? 1 : 0);
    const TextureHandle* shadowTex = shadowSystem_.ShadowDepthTex();
    backend_->BindTexture(5, shadowTex[0]);
    backend_->SetUniformInt("uShadowMap0", 5);
    backend_->BindTexture(6, shadowTex[1]);
    backend_->SetUniformInt("uShadowMap1", 6);
    backend_->BindTexture(7, shadowTex[2]);
    backend_->SetUniformInt("uShadowMap2", 7);

    // Point-light cubemap shadows: 2 lights x 6 faces on texture units
    // 8..19. When the pass is inactive the uniforms are set to valid units
    // anyway (harmless: the shader never samples them), so inactive lights
    // only leave their units unbound.
    const int psLightCount =
        shadowSystem_.PointShadowsActive()
            ? std::min(sceneState_.PointCount(), ShadowSystem::kShadowPointLights)
            : 0;
    backend_->SetUniformInt("uPointShadowEnabled", shadowSystem_.PointShadowsActive() ? 1 : 0);
    backend_->SetUniformInt("uPointShadowLightCount", psLightCount);
    backend_->SetUniformVec2("uPointShadowTexel",
                             {1.0f / static_cast<float>(ShadowSystem::kPointShadowSize),
                              1.0f / static_cast<float>(ShadowSystem::kPointShadowSize)});
    const TextureHandle* pointShadowTex = shadowSystem_.PointShadowDepthTex();
    for (int li = 0; li < ShadowSystem::kShadowPointLights; ++li) {
        for (int face = 0; face < 6; ++face) {
            const int slot = 8 + li * 6 + face;
            const std::string name = "uPointShadowMap" + std::to_string(li * 6 + face);
            if (li < psLightCount) backend_->BindTexture(slot, pointShadowTex[li * 6 + face]);
            backend_->SetUniformInt(name.c_str(), slot);
        }
    }

    // IBL environment maps (texture units 20..22): irradiance, prefiltered
    // specular, BRDF LUT. When no environment exists yet (IBL off, or
    // recompute pending) the uniforms stay at their GLSL defaults
    // (uIblStrength = 0) so the shader contributes no IBL term.
    if (sceneState_.IblValid()) {
        iblWasValid_ = true;
        backend_->SetUniformFloat("uIblStrength", sceneState_.IblStrength());
        backend_->SetUniformFloat("uRoughnessMin", ibl::kRoughnessMin);
        backend_->BindTexture(20, sceneState_.IblIrradianceTex());
        backend_->SetUniformInt("uIrradianceMap", 20);
        backend_->BindTexture(21, sceneState_.IblPrefilteredTex());
        backend_->SetUniformInt("uPrefilteredMap", 21);
        backend_->BindTexture(22, sceneState_.IblBrdfLutTex());
        backend_->SetUniformInt("uBrdfLUT", 22);
    } else if (iblWasValid_) {
        // The IBL set went invalid after having been uploaded (recompute
        // failure / SceneState::Shutdown): zero the strength so the shader
        // contributes no IBL term instead of sampling stale/destroyed handles.
        backend_->SetUniformFloat("uIblStrength", 0.0f);
        iblWasValid_ = false;
    }
    // A3 probe-field GI atlas (texture unit 24, after normalMap on 23 and IBL on
    // 20..22): sampled by world position for indirect diffuse, blended into the
    // IBL ambient. Disabled when no atlas is bound (invalid handle): the GLSL
    // defaults (uLightProbeEnabled = 0, empty texture) make the term a no-op.
    if (lightProbeAtlas_.Valid()) {
        backend_->BindTexture(24, lightProbeAtlas_);
        backend_->SetUniformInt("uLightProbeAtlas", 24);
        backend_->SetUniformInt("uLightProbeEnabled", 1);
        const math::Vec3 mn = lightProbeBounds_.min;
        const math::Vec3 ex = lightProbeBounds_.max - lightProbeBounds_.min;
        backend_->SetUniformVec3("uLightProbeMin", mn);
        backend_->SetUniformVec3("uLightProbeExtent", ex);
        backend_->SetUniformFloat("uLightProbeRes", static_cast<float>(lightProbeRes_));
        backend_->SetUniformFloat("uLightProbeInvMax",
                                  1.0f / (lightProbeMaxIrr_ > 0.0f ? lightProbeMaxIrr_ : 1.0f));
    } else {
        backend_->SetUniformInt("uLightProbeEnabled", 0);
        backend_->BindTexture(24, white_);
        backend_->SetUniformInt("uLightProbeAtlas", 24);
    }
}

void Renderer::DrawLines(const LineVertex* vertices, uint32_t count, const math::Mat4& model) {
    if (!vertices || count == 0) return;
    Flush2D();
    backend_->SetBlendMode(BlendMode::Alpha);
    backend_->SetDepthTest(sceneState_.DepthAvailable(), false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(linesShader_);
    backend_->SetUniformMat4("uMVP", sceneState_.ViewProjection() * model);
    backend_->DrawPrimitives(vertices, count, 28, nullptr, 0, PrimitiveTopology::Lines);
}

namespace {
// Appends one camera-facing ribbon (points oldest-first) to the shared vertex /
// index streams. `tailWidthScale` tapers the width towards the oldest point so
// a trail dissolves into a point instead of ending in a blunt rectangle.
void AppendRibbon(std::vector<Renderer::LineVertex>& verts, std::vector<uint16_t>& idx,
                  const math::Vec3* points, uint32_t count, float width,
                  float tailWidthScale, const Color& head, const Color& tail,
                  const math::Vec3& eye) {
    const uint16_t base = static_cast<uint16_t>(verts.size());
    for (uint32_t i = 0; i < count; ++i) {
        const math::Vec3 p = points[i];
        math::Vec3 dir = (i + 1 < count) ? (points[i + 1] - p) : (p - points[i - 1]);
        if (dir.LengthSq() < 1e-8f) dir = {0.0f, 0.0f, 1.0f};
        dir = dir.Normalized();
        // Perpendicular in the view plane: cross(segment, toEye).
        math::Vec3 side = math::Cross(dir, eye - p);
        if (side.LengthSq() < 1e-8f) side = math::Cross(dir, {0.0f, 1.0f, 0.0f});
        // i = 0 is the oldest point (tail); fade + taper tail -> head.
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        side = side.Normalized() * (width * 0.5f * math::Lerp(tailWidthScale, 1.0f, t));
        const Color c{tail.r + (head.r - tail.r) * t, tail.g + (head.g - tail.g) * t,
                      tail.b + (head.b - tail.b) * t, tail.a + (head.a - tail.a) * t};
        verts.push_back({p - side, c});
        verts.push_back({p + side, c});
    }
    for (uint32_t i = 0; i + 1 < count; ++i) {
        const uint16_t a = static_cast<uint16_t>(base + i * 2);
        const uint16_t b = static_cast<uint16_t>(base + i * 2 + 1);
        const uint16_t c = static_cast<uint16_t>(base + (i + 1) * 2);
        const uint16_t d = static_cast<uint16_t>(base + (i + 1) * 2 + 1);
        idx.push_back(a); idx.push_back(c); idx.push_back(b);
        idx.push_back(b); idx.push_back(c); idx.push_back(d);
    }
}
} // namespace

void Renderer::DrawTrail(const math::Vec3* points, uint32_t count, float width,
                         const Color& head, const Color& tail, float tailWidthScale) {
    TrailDraw d;
    d.points = points;
    d.count = count;
    d.width = width;
    d.tailWidthScale = tailWidthScale;
    d.head = head;
    d.tail = tail;
    DrawTrails(&d, 1);
}

void Renderer::DrawTrails(const TrailDraw* draws, uint32_t drawCount) {
    if (!backend_ || !draws || drawCount == 0) return;
    const math::Vec3 eye = sceneState_.ActiveCamera().position;
    trailVerts_.clear();
    trailIndices_.clear();
    bool any = false;
    auto flush = [&]() {
        if (trailIndices_.empty()) return;
        Flush2D();
        backend_->SetBlendMode(BlendMode::Additive);
        backend_->SetDepthTest(sceneState_.DepthAvailable(), false);
        backend_->SetCullMode(CullMode::None);
        backend_->UseShader(linesShader_);
        backend_->SetUniformMat4("uMVP", sceneState_.ViewProjection());
        backend_->DrawPrimitives(trailVerts_.data(), static_cast<uint32_t>(trailVerts_.size()),
                                 28, trailIndices_.data(),
                                 static_cast<uint32_t>(trailIndices_.size()),
                                 PrimitiveTopology::Triangles);
        ++stats_.drawCalls;
        stats_.triangles += static_cast<uint32_t>(trailIndices_.size() / 3);
        trailVerts_.clear();
        trailIndices_.clear();
    };
    for (uint32_t i = 0; i < drawCount; ++i) {
        const TrailDraw& d = draws[i];
        if (d.points == nullptr || d.count < 2 || d.width <= 0.0f) continue;
        // 16-bit indices: flush before the next ribbon would overflow.
        if (trailVerts_.size() + static_cast<size_t>(d.count) * 2 > 65000) flush();
        AppendRibbon(trailVerts_, trailIndices_, d.points, d.count, d.width, d.tailWidthScale,
                     d.head, d.tail, eye);
        any = true;
    }
    if (any) flush();
}

void Renderer::DrawBox(const math::AABB& box, const Color& color) {    math::Vec3 c[8] = {
        {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z},
        {box.max.x, box.max.y, box.min.z}, {box.min.x, box.max.y, box.min.z},
        {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
        {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z}};
    const uint8_t edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    LineVertex verts[24];
    for (int i = 0; i < 12; ++i) {
        verts[i * 2] = {c[edges[i][0]], color};
        verts[i * 2 + 1] = {c[edges[i][1]], color};
    }
    DrawLines(verts, 24, math::Mat4::Identity());
}

void Renderer::DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments) {
    std::vector<LineVertex> verts;
    auto ring = [&](const math::Vec3& axisA, const math::Vec3& axisB) {
        for (int i = 0; i < segments; ++i) {
            float a0 = static_cast<float>(i) / segments * math::kTwoPi;
            float a1 = static_cast<float>(i + 1) / segments * math::kTwoPi;
            math::Vec3 p0 = center + (axisA * std::cos(a0) + axisB * std::sin(a0)) * radius;
            math::Vec3 p1 = center + (axisA * std::cos(a1) + axisB * std::sin(a1)) * radius;
            verts.push_back({p0, color});
            verts.push_back({p1, color});
        }
    };
    ring({1, 0, 0}, {0, 1, 0});
    ring({1, 0, 0}, {0, 0, 1});
    ring({0, 1, 0}, {0, 0, 1});
    DrawLines(verts.data(), static_cast<uint32_t>(verts.size()), math::Mat4::Identity());
}

Texture Renderer::CreateTexture(const TextureDesc& desc) {
    TextureHandle handle = backend_->CreateTexture(desc);
    return Texture(handle, desc.width, desc.height);
}

void Renderer::UpdateTexture(const Texture& tex, int x, int y, int w, int h, const void* rgba) {
    if (!tex.Valid() || rgba == nullptr || w <= 0 || h <= 0) return;
    backend_->UpdateTextureRegion(tex.Handle(), x, y, w, h, rgba);
}

void Renderer::DestroyTexture(Texture& tex) {
    if (!tex.Valid()) return;
    backend_->DestroyTexture(tex.Handle());
    tex = Texture{};
}

Texture Renderer::CreateTextureCompressed(int width, int height, uint32_t format,
                                          const void* data, size_t size) {
    TextureHandle handle = backend_->CreateTextureCompressed(width, height, format, data, size);
    return Texture(handle, width, height);
}

Shader Renderer::CreateShader(const char* vertexSource, const char* fragmentSource, const char* name) {
    return Shader(backend_->CreateShader(vertexSource, fragmentSource, name), name);
}

Shader Renderer::CreateUnlitFragmentShader(const std::string& fragmentSource,
                                           const std::string& name) {
    if (fragmentSource.empty() || !backend_) return {};
    return CreateShader(kUnlitVertexShader, fragmentSource.c_str(), name.c_str());
}

void Renderer::DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                        TextureHandle texture, const math::Vec2& uv0, const math::Vec2& uv1,
                        BlendMode blend) {
    draw2d_.DrawQuad(pos, size, color, texture, uv0, uv1, blend);
}

void Renderer::DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color) {
    draw2d_.DrawRect(pos, size, color);
}

void Renderer::DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color) {
    draw2d_.DrawRectOutline(rect, thickness, color);
}

void Renderer::DrawTriangle2D(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                              const Color& color) {
    draw2d_.DrawTriangle2D(a, b, c, color);
}

void Renderer::DrawTriangle2DColored(const math::Vec2& a, const math::Vec2& b,
                                     const math::Vec2& c, const Color& ca, const Color& cb,
                                     const Color& cc) {
    draw2d_.DrawTriangle2DColored(a, b, c, ca, cb, cc);
}

void Renderer::DrawText(const Font& font, const std::string& text, const math::Vec2& pos,
                        float size, const Color& color, bool centerX, bool centerY) {
    draw2d_.DrawText(font, text, pos, size, color, centerX, centerY);
}

void Renderer::DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                             TextureHandle texture, BlendMode blend) {
    draw2d_.DrawBillboard(worldPos, size, color, texture, blend, sceneState_.ActiveCamera(),
                          sceneState_.ViewProjection());
}

math::Vec2 Renderer::ScreenToUI(const math::Vec2& screenPixels) const {
    return draw2d_.ScreenToUI(screenPixels);
}

bool Renderer::CaptureFrame(std::vector<uint8_t>& out) {
    if (!backend_) return false;
    // The scene lives in the HDR target at this point; composite it (bloom +
    // clamp) to the backbuffer first so the captured pixels are the FINAL
    // rendered image, then flush any pending 2D on top. EndFrame will see the
    // post graph already ran (CompositeRan) and just swap.
    CompositeFrame();
    out.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, out.data());
    return true;
}

math::Vec2 Renderer::ToScreen(const math::Vec2& design) const {
    return draw2d_.ToScreen(design);
}

void Renderer::Set2DViewport(float x, float y, float w, float h, float zoom,
                             const math::Vec2& pan, float aspect) {
    draw2d_.Set2DViewport(x, y, w, h, zoom, pan, aspect);
}

void Renderer::Reset2DViewport() {
    draw2d_.Reset2DViewport();
}

void Renderer::Set2DViewportPixels(float x, float y) {
    draw2d_.Set2DViewportPixels(x, y);
}

void Renderer::SetSceneViewport(float x, float y, float w, float h) {
    draw2d_.SetSceneViewport(x, y, w, h);
    sceneVpLast_ = {x, y, w, h};
    // The scene rasters into the HDR target, which render scaling may have made
    // smaller than the window; re-apply the rect in render-target pixels. The 2D
    // mapping above stays in window pixels (HUD/text layout is unaffected).
    const float s = EffectiveRenderScale();
    if (s != 1.0f && hdrEnabled_ && hdrRT_.Valid()) {
        backend_->SetViewport(static_cast<int>(x * s), static_cast<int>(y * s),
                              std::max(static_cast<int>(w * s), 1),
                              std::max(static_cast<int>(h * s), 1));
    }
}

void Renderer::ResetSceneViewport() {
    draw2d_.ResetSceneViewport();
    if (hdrEnabled_ && hdrRT_.Valid()) backend_->SetViewport(0, 0, hdrW_, hdrH_);
}

float Renderer::SceneAspect() const {
    return draw2d_.SceneAspect();
}

void Renderer::Flush2D() {
    draw2d_.Flush2D();
}

void Renderer::RebuildHdrTargets() {
    if (!hdrEnabled_) return;
    if (screenW_ <= 0 || screenH_ <= 0) return;
    // Step D render scaling: the scene + post chain run at (window * scale) and
    // the terminal composite (which binds the default target, viewport = window)
    // upscales with the HDR target's bilinear filter.
    const float scale = EffectiveRenderScale();
    const int sw = std::max(
        static_cast<int>(std::lround(static_cast<double>(screenW_) * scale)), 1);
    const int sh = std::max(
        static_cast<int>(std::lround(static_cast<double>(screenH_) * scale)), 1);
    const bool wantVelocity = velocityEnabled_ && velocityShader_.Valid();
    if (hdrRT_.Valid() && hdrW_ == sw && hdrH_ == sh &&
        velocityRT_.Valid() == wantVelocity)
        return;
    DestroyHdrTargets();
    const int hw = std::max(sw / 2, 1);
    const int hh = std::max(sh / 2, 1);
    const int qw = std::max(sw / 4, 1);
    const int qh = std::max(sh / 4, 1);
    hdrRT_ = backend_->CreateRenderTarget(sw, sh, true);
    if (!hdrRT_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "Renderer: HDR target %dx%d creation failed -> HDR/bloom disabled", sw, sh);
        hdrEnabled_ = false;
        return;
    }
    // MSAA request is re-evaluated here (not only in Init) so SetMsaaEnabled /
    // SetQuality take effect at runtime. A freshly requested sample count resets
    // msaaSamples_ to 0, which re-runs the capability probe.
    if (!msaaRequested_) {
        msaaEnabled_ = false;
    } else if (!msaaEnabled_ || msaaSamples_ == 0) {
        msaaEnabled_ = TestMsaaCapability();
    }
    if (msaaEnabled_) {
        // MSAA scene target: resolves into hdrRT_ (the post-chain source)
        // before the graph executes. Only the HDR main target is multisampled.
        hdrMsaaRT_ = backend_->CreateRenderTarget(sw, sh, true, msaaSamples_);
        if (!hdrMsaaRT_.Valid()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: MSAA target %dx%d (samples=%d) failed -> single-sample HDR",
                         sw, sh, msaaSamples_);
            msaaEnabled_ = false;
        } else {
            // B4: resolve the MSAA depth into a sampleable depth target so the
            // post chain can reuse the main pass depth (no caster redraw).
            hdrDepthRT_ = backend_->CreateDepthTarget(sw, sh);
        }
    }
    hdrW_ = sw;
    hdrH_ = sh;
    // Step E: the TAA history lives at the render resolution; a resize (or a
    // render-scale change) invalidates it so the first frame seeds cleanly.
    taaHistoryValid_ = false;
    prevViewProjValid_ = false;
    taaOutput_ = {};
    if (taaEnabled_) taa_.Init(*backend_, hdrW_, hdrH_);
    // Step E2: motion-vector target, same size as the render resolution. Only
    // allocated while temporal AA is on (it is the only consumer).
    if (velocityEnabled_ && velocityShader_.Valid())
        velocityRT_ = backend_->CreateRenderTarget(sw, sh, true);
    // Every post target (bloom pyramid + depth/AO/blur/vol/SSR) lives in the
    // unified post graph's transient pool: rebuild the graph at the new
    // resolution (Destroy first releases the old graph's GPU allocations).
    // Shaders/mesh were created in InitBuiltinResources. The depth pass draws
    // the scene's casters directly through DrawSsaoDepthCasters (its execute
    // lambda is the renderer's viewProj at draw time, so per-frame camera
    // changes are picked up without rebuilding).
    postGraph_.Destroy(*backend_);
    PostGraph::Shaders shaders;
    shaders.ssaoShader = ssaoShader_;
    shaders.depthEncodeShader = depthEncodeShader_;
    shaders.ssaoBlur = ssaoBlurShader_;
    shaders.volumetricShader = volumetricShader_;
    shaders.ssrShader = ssrShader_;
    shaders.brightPass = brightPassShader_;
    shaders.blur = blurShader_;
    shaders.downsample = downsampleShader_;
    shaders.upsampleAdd = upsampleAddShader_;
    shaders.luminanceShader = luminanceShader_;
    shaders.luminanceReduceShader = luminanceReduceShader_;
    shaders.exposureAdaptShader = exposureAdaptShader_;
    shaders.compositeShader = compositeShader_;
    shaders.white = white_;
    postGraph_.Build(shaders, postQuadMesh_, sw, sh,
                     [this] { DrawSsaoDepthCasters(sceneState_.ViewProjection()); });
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: HDR target %dx%d (RGBA16F%s, scale %.2f) + bloom %dx%d / %dx%d",
                 sw, sh, msaaEnabled_ ? ", MSAA" : "", scale, hw, hh, qw, qh);
}

void Renderer::DestroyHdrTargets() {
    auto destroy = [this](RenderTargetHandle& t) {
        if (t.Valid() && backend_) backend_->DestroyRenderTarget(t);
        t = {};
    };
    destroy(velocityRT_);
    destroy(hdrMsaaRT_);
    destroy(hdrDepthRT_);
    destroy(hdrRT_);
    // The post pyramid targets are owned by the FrameGraph pool; release every
    // allocation it still holds (also covers a pending result).
    if (backend_) postGraph_.Destroy(*backend_);
    hdrW_ = 0;
    hdrH_ = 0;
}

bool Renderer::TestFloatTargetCapability() {
    if (!backend_ || !unlitShader_.Valid() || !probeQuadMesh_.Valid()) return false;
    constexpr int kSize = 32;
    RenderTargetHandle rt = backend_->CreateRenderTarget(kSize, kSize, true);
    if (!rt.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: HDR FBO self-test: float target failed -> HDR/bloom disabled");
        return false;
    }
    backend_->BindRenderTarget(rt);
    backend_->Clear({0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    backend_->UseShader(unlitShader_);
    backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
    backend_->SetUniformInt("uHasTexture", 0);
    backend_->SetUniformVec4("uTint", {0.5f, 0.25f, 0.125f, 1.0f});
    backend_->SetCullMode(CullMode::None);
    backend_->SetDepthTest(false, false);
    backend_->SetBlendMode(BlendMode::Opaque);
    backend_->DrawMesh(probeQuadMesh_);
    unsigned char px[4] = {0, 0, 0, 0};
    backend_->ReadCurrentTargetPixel(kSize / 2, kSize / 2, px);
    backend_->DestroyRenderTarget(rt);
    backend_->BindDefaultTarget();
    // Drawn {0.5, 0.25, 0.125} must come back as ~{128, 64, 32} after the
    // float->byte readback; wide-but-specific ranges catch both a non-writing
    // FBO (zeros) and a clamped-to-1 target (255).
    const bool ok = px[0] >= 110 && px[0] <= 150 && px[1] >= 48 && px[1] <= 80 && px[2] >= 16 &&
                    px[2] <= 48;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: HDR FBO self-test: px=%u,%u,%u,%u -> %s", px[0], px[1], px[2], px[3],
                 ok ? "PASS" : "FAIL");
    if (!ok) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: HDR float render target unusable -> HDR/bloom disabled");
        return false;
    }
    return true;
}

bool Renderer::TestMsaaCapability() {
    if (!backend_ || !unlitShader_.Valid() || !probeQuadMesh_.Valid()) return false;
    constexpr int kSize = 32;
    // Try the requested sample count first (when a quality preset asked for a
    // specific one), then 4x/2x for drivers that only handle lower counts;
    // either way the resolved image must round-trip the drawn colour through the
    // same FBO + blit path the frame uses.
    int attempts[3] = {4, 2, 0};
    int attemptCount = 2;
    if (msaaSampleRequest_ > 0) {
        attempts[0] = msaaSampleRequest_;
        attempts[1] = 4;
        attempts[2] = 2;
        attemptCount = 3;
    }
    for (int ai = 0; ai < attemptCount; ++ai) {
        const int samples = attempts[ai];
        if (samples <= 0) break;
        RenderTargetHandle ms = backend_->CreateRenderTarget(kSize, kSize, true, samples);
        RenderTargetHandle ss = backend_->CreateRenderTarget(kSize, kSize, true);
        bool keep = false;
        if (ms.Valid() && ss.Valid()) {
            backend_->BindRenderTarget(ms);
            backend_->Clear({0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
            backend_->UseShader(unlitShader_);
            backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
            backend_->SetUniformInt("uHasTexture", 0);
            backend_->SetUniformVec4("uTint", {0.5f, 0.25f, 0.125f, 1.0f});
            backend_->SetCullMode(CullMode::None);
            backend_->SetDepthTest(false, false);
            backend_->SetBlendMode(BlendMode::Opaque);
            backend_->DrawMesh(probeQuadMesh_);
            backend_->ResolveRenderTarget(ms, ss);
            unsigned char px[4] = {0, 0, 0, 0};
            backend_->BindRenderTarget(ss);
            backend_->ReadCurrentTargetPixel(kSize / 2, kSize / 2, px);
            // {0.5, 0.25, 0.125} must survive draw -> multisample -> blit ->
            // byte readback as ~{128, 64, 32}; wide-but-specific ranges catch a
            // dead FBO (zeros) and a clamped-to-1 target (255).
            const bool ok = px[0] >= 110 && px[0] <= 150 && px[1] >= 48 && px[1] <= 80 &&
                            px[2] >= 16 && px[2] <= 48;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: MSAA %dx self-test: px=%u,%u,%u,%u -> %s", samples, px[0],
                         px[1], px[2], px[3], ok ? "PASS" : "FAIL");
            keep = ok;
        }
        backend_->DestroyRenderTarget(ss);
        backend_->DestroyRenderTarget(ms);
        backend_->BindDefaultTarget();
        if (keep) {
            msaaSamples_ = samples;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: MSAA %dx HDR target self-test PASS", samples);
            return true;
        }
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: MSAA %dx HDR target self-test FAIL", samples);
    }
    return false;
}

void Renderer::ResolveMainTarget() {
    depthResolved_ = false;
    if (msaaEnabled_ && hdrMsaaRT_.Valid() && hdrRT_.Valid()) {
        backend_->ResolveRenderTarget(hdrMsaaRT_, hdrRT_);
        // B4: also resolve the depth so the post chain reuses the main pass depth
        // (returns false on backends/drivers that cannot resolve depth, in which
        // case the depth pass falls back to redrawing the casters).
        if (hdrDepthRT_.Valid()) {
            depthResolved_ = backend_->ResolveDepth(hdrMsaaRT_, hdrDepthRT_);
            // A failing resolve flips the frame back to the caster fallback
            // (PostDepthReuseOk) instead of silently losing the AO chain.
            if (!depthResolved_) depthResolveSupported_ = false;
        }
    }
}

bool Renderer::EnsureSoftDepth() {
    if (softDepthReady_) return true;
    softDepthReady_ = true; // try once per frame regardless of outcome
    // Soft particles need a sampleable depth texture. Only the MSAA HDR path
    // keeps a depth target (hdrDepthRT_); the single-sample path has a bare
    // depth RBO, so soft particles are simply disabled there.
    if (!hdrEnabled_ || !hdrMsaaRT_.Valid() || !hdrDepthRT_.Valid()) return false;
    if (!backend_->ResolveDepth(hdrMsaaRT_, hdrDepthRT_)) return false; // restores currentFBO_
    if (!backend_->RenderTargetDepthTexture(hdrDepthRT_).Valid()) return false;
    return true;
}

void Renderer::RebindMainTarget() {
    if (hdrEnabled_ && hdrRT_.Valid()) {
        backend_->BindRenderTarget(msaaEnabled_ && hdrMsaaRT_.Valid() ? hdrMsaaRT_ : hdrRT_);
    } else {
        backend_->BindDefaultTarget();
    }
}

PostGraph::FrameParams Renderer::MakePostParams(bool chains) const {
    PostGraph::FrameParams p;
    // Step E: the post chain reads the temporally accumulated image when the
    // resolve produced one (the diagnostic comparisons force chains=false and
    // always see the raw scene colour).
    p.hdrScene = (chains && taaEnabled_ && taaOutput_.Valid()) ? taaOutput_ : hdrRT_;
    p.hdrW = hdrW_;
    p.hdrH = hdrH_;
    // Depth pre-pass is needed by SSAO/SSR and by the composite's volumetric
    // fog; the SSAO chain additionally requires scene casters (an empty scene
    // would produce an all-far depth whose AO is a no-op white). chains=false
    // forces every chain off (the old CaptureBloom/TonemapComparison path never
    // ran the post graph, so its composites had no AO/vol/SSR/fog terms).
    p.depthPass = chains && (ssaoEnabled_ || ssrEnabled_ || volumetricEnabled_ ||
                             sceneState_.VolumetricFogEnabled());
    // B4: when the main pass depth was resolved, feed it to the depth pass (which
    // then encodes it fullscreen instead of redrawing the casters).
    if (depthResolved_ && hdrDepthRT_.Valid())
        p.depthTexture = backend_->RenderTargetDepthTexture(hdrDepthRT_);
    p.ssaoPass = chains && ssaoEnabled_ &&
                 (p.depthTexture.Valid() || !ssaoCasters_.empty());
    p.volumetricPass = chains && volumetricEnabled_;
    p.ssrPass = chains && ssrEnabled_;
    p.bloomPass = bloomEnabled_;
    p.camPos = sceneState_.CamPos();
    p.sunDir = sceneState_.SunDir();
    const Color& sunC = sceneState_.SunColor();
    p.sunColor = {sunC.r, sunC.g, sunC.b};
    p.viewProj = sceneState_.ViewProjection();
    const math::Rect2& svp = SceneVpLastRect();
    if (svp.w > 0.0f && svp.h > 0.0f) {
        p.sceneVpRect = {svp.x, svp.y, svp.w, svp.h};
    } else {
        // Render-target pixels (not window pixels): the post graph normalises
        // this rect by the HDR target size, which render scaling shrinks.
        p.sceneVpRect = {0.0f, 0.0f, static_cast<float>(hdrW_ > 0 ? hdrW_ : screenW_),
                         static_cast<float>(hdrH_ > 0 ? hdrH_ : screenH_)};
    }
    p.camera = sceneState_.ActiveCamera();
    // Wall-clock dt for the auto-exposure adaptation rate.
    p.frameDt = lastFrameDtSec_;
    p.composite.ssaoIntensity = ssaoIntensity_;
    p.composite.volStrength = volumetricIntensity_;
    p.composite.ssrStrength = ssrIntensity_;
    p.composite.volumetricFog = chains && sceneState_.VolumetricFogEnabled();
    const Color& fogColor = sceneState_.FogColor();
    p.composite.fogColor = {fogColor.r, fogColor.g, fogColor.b};
    p.composite.fogDensity = sceneState_.VolumetricFogDensity();
    p.composite.exposure = exposure_;
    p.composite.tonemapEnabled = tonemapEnabled_;
    p.composite.bloomThreshold = bloomThreshold_;
    p.composite.bloomStrength = bloomStrength_;
    p.composite.bloomWidth = bloomWidth_;
    p.composite.colorGrade = colorGrade_;
    p.composite.autoExposure = autoExposure_;
    // Diagnostic captures (chains=false) re-run the composite with tweaked
    // switches; keep the exposure-measure chain out of them entirely so the
    // capture passes don't advance the adaptation state.
    p.composite.autoExposure.enabled = chains && autoExposure_.enabled;
    p.composite.vignette = vignette_;
    p.composite.white = white_;
    return p;
}

void Renderer::CompositeSceneToBackbuffer() {
    if (!hdrEnabled_ || !hdrRT_.Valid()) {
        Flush2D();
        return;
    }
    // The scene rendered into the (possibly multisample) HDR target; resolve
    // into the single-sample source before any pass samples it.
    // Every recorded motion vector is rasterized in ONE pass before anything
    // samples velocityRT_ (the TAA resolve below).
    FlushVelocityPass();
    ResolveMainTarget();
    // Step E: temporal resolve before the post chain (bloom/composite read the
    // accumulated image instead of the raw jittered one when this succeeds).
    ApplyTaa();
    // The SSAO/volumetric/SSR/depth/bloom chain + the terminal composite run as
    // one FrameGraph (postGraph_): each chain executes only when its enabled
    // flag is on, and the composite pass samples the finals in-graph and draws
    // the result to the backbuffer.
    postGraph_.Execute(*backend_, MakePostParams(true));
    Flush2D();
}

void Renderer::EndScene() {
    if (!hdrEnabled_ || !hdrRT_.Valid()) return; // legacy: 2D already to backbuffer
    if (!postGraph_.CompositeRan()) {
        // Any 2D still queued at this point is scene content (billboards,
        // particles, ground marker): flush it into the HDR target so it is
        // bloomed with the scene, then run the post chain (whose composite
        // draws to the backbuffer).
        Flush2D();
        CompositeSceneToBackbuffer();
    }
    // From here on every 2D flush goes straight to the backbuffer (unbloomed,
    // on top of the composite): HUD/nameplates/minimap/editor UI.
    backend_->BindDefaultTarget();
}

void Renderer::CompositeFrame() {
    if (!postGraph_.CompositeRan()) {
        CompositeSceneToBackbuffer();
    } else {
        // EndScene already composited this frame; just draw any 2D the app
        // pushed after EndScene (the HUD) onto the backbuffer.
        Flush2D();
    }
}

bool Renderer::CaptureBloomComparison(std::vector<uint8_t>& bloomOff,
                                      std::vector<uint8_t>& bloomOn) {
    if (!backend_ || !hdrEnabled_ || !hdrRT_.Valid()) return false;
    const bool savedBloom = bloomEnabled_;
    // Both captures composite the same (resolved) HDR target WITHOUT the 2D
    // overlay, so the two buffers differ only by the bloom term; the HUD is
    // flushed once at the end (it is drawn on top of the composite and is not
    // bloomed). The post chains are forced off (chains=false) exactly like the
    // old path, which never ran the post graph during the comparison.
    ResolveMainTarget();
    bloomEnabled_ = false;
    postGraph_.Execute(*backend_, MakePostParams(false));
    bloomOff.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, bloomOff.data());
    bloomEnabled_ = savedBloom;
    postGraph_.Execute(*backend_, MakePostParams(false));
    bloomOn.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, bloomOn.data());
    Flush2D();
    return true;
}

bool Renderer::CaptureTonemapComparison(std::vector<uint8_t>& clamped,
                                        std::vector<uint8_t>& tonemapped) {
    if (!backend_ || !hdrEnabled_ || !hdrRT_.Valid()) return false;
    const bool savedTonemap = tonemapEnabled_;
    // Same-frame diff of the tone-mapping operator: composite the SAME
    // resolved HDR target twice, once with ACES+exposure and once with the
    // T3.6 clamp reference. Bloom runs in both Executes on the same HDR input,
    // so its contribution is identical; the post chains are off (chains=false)
    // as in the old comparison path.
    ResolveMainTarget();
    tonemapEnabled_ = false;
    postGraph_.Execute(*backend_, MakePostParams(false));
    clamped.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, clamped.data());
    tonemapEnabled_ = savedTonemap;
    postGraph_.Execute(*backend_, MakePostParams(false));
    tonemapped.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, tonemapped.data());
    Flush2D();
    return true;
}

void Renderer::DrawSsaoDepthCasters(const math::Mat4& viewProj) {
    if (ssaoCasters_.empty()) return;
    // Painter's order far->near (the colour-encoded depth target has no depth
    // buffer), mirroring the shadow encoder.
    shadowSortKeys_.clear();
    shadowSortKeys_.reserve(ssaoCasters_.size());
    for (const ShadowSystem::ShadowDraw& draw : ssaoCasters_) {
        math::Vec3 center;
        if (!draw.models.empty()) {
            for (const math::Mat4& m : draw.models) center += m.TransformPoint(draw.bounds.Center());
            center = center * (1.0f / static_cast<float>(draw.models.size()));
        } else {
            center = draw.model.TransformPoint(draw.bounds.Center());
        }
        shadowSortKeys_.push_back({&draw, viewProj.TransformPoint(center).z});
    }
    std::sort(shadowSortKeys_.begin(), shadowSortKeys_.end(),
              [](const ShadowSystem::ShadowSortKey& a, const ShadowSystem::ShadowSortKey& b) {
                  return a.z > b.z;
              });
    for (const ShadowSystem::ShadowSortKey& k : shadowSortKeys_) {
        const ShadowSystem::ShadowDraw& draw = *k.draw;
        if (!draw.mesh.Valid()) continue;
        if (!draw.models.empty()) {
            backend_->UseShader(ssaoDepthShader_);
            backend_->SetUniformMat4("uMVP", viewProj);
            backend_->SetUniformFloat("uFar", sceneState_.ActiveCamera().farPlane);
            backend_->DrawMeshInstanced(draw.mesh, draw.models.data(),
                                        static_cast<uint32_t>(draw.models.size()));
        } else if (!draw.bones.empty() && ssaoDepthSkinnedShader_.Valid()) {
            // Linear-depth SKINNED variant (see kSsaoDepthSkinnedVertexShader):
            // the shadow skinned program writes window depth, which the post
            // chain would decode as ~990m at 10m distance.
            backend_->UseShader(ssaoDepthSkinnedShader_);
            boneUniformFlat_.resize(static_cast<size_t>(draw.boneCount) * 16);
            for (int i = 0; i < draw.boneCount; ++i)
                std::memcpy(boneUniformFlat_.data() + static_cast<size_t>(i) * 16,
                            draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
            backend_->SetUniformMat4Array("uBoneMatrices", boneUniformFlat_.data(), draw.boneCount);
            backend_->SetUniformMat4("uMVP", viewProj * draw.model);
            backend_->SetUniformFloat("uFar", sceneState_.ActiveCamera().farPlane);
            backend_->DrawMesh(draw.mesh);
        } else if (!draw.bones.empty()) {
            // No skinned linear-depth variant (shader build failed): fall back
            // to the static path so at least the model transform is right.
            backend_->UseShader(ssaoDepthMeshShader_);
            backend_->SetUniformMat4("uMVP", viewProj * draw.model);
            backend_->SetUniformFloat("uFar", sceneState_.ActiveCamera().farPlane);
            backend_->DrawMesh(draw.mesh);
        } else {
            backend_->UseShader(ssaoDepthMeshShader_);
            backend_->SetUniformMat4("uMVP", viewProj * draw.model);
            backend_->SetUniformFloat("uFar", sceneState_.ActiveCamera().farPlane);
            backend_->DrawMesh(draw.mesh);
        }
    }
}

} // namespace neon::gfx
