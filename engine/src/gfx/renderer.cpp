#include "neon/gfx/renderer.hpp"

#include <algorithm>
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
    // Stop the background upload worker before destroying any shared GL
    // resources or the window's context (the worker holds the shared context).
    if (uploadThread_) {
        uploadThread_->Shutdown();
        uploadThread_.reset();
    }
    if (litShader_.Valid()) backend_->DestroyShader(litShader_);
    if (skinnedLitShader_.Valid()) backend_->DestroyShader(skinnedLitShader_);
    if (unlitShader_.Valid()) backend_->DestroyShader(unlitShader_);
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
    ssaoDepthMeshShader_ = backend_->CreateShader(kSsaoDepthMeshVertexShader,
                                                  kSsaoDepthFragmentShader, "ssao_depth_mesh");
    ssaoShader_ = backend_->CreateShader(kPostVertexShader, kSsaoFragmentShader, "ssao");
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
    screenW_ = window_ ? window_->Width() : screenW_;
    screenH_ = window_ ? window_->Height() : screenH_;
    draw2d_.Resize(screenW_, screenH_);
    RebuildHdrTargets();
    if (hdrEnabled_ && hdrRT_.Valid()) {
        // Scene + sky draw into the (multisample when MSAA is active) HDR
        // target; the final composite (bloom -> backbuffer) happens in
        // EndFrame / CaptureFrame after resolving the MSAA samples.
        RebindMainTarget();
        backend_->SetViewport(0, 0, screenW_, screenH_);
        backend_->Clear(clearColor, clearDepth);
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
    // Render the cascade shadow maps now: they are sampled by the main-pass
    // draws that follow this SetCamera. Uses the previous frame's recorded
    // casters (one frame of staleness, imperceptible) and the current camera.
    if (shadowSystem_.Enabled() && !shadowSystem_.ShadowPassRanThisFrame()) {
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
        if (draw2d_.SceneViewportActive()) {
            const math::Rect2& vp = draw2d_.SceneViewport();
            backend_->SetViewport(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                  static_cast<int>(vp.w), static_cast<int>(vp.h));
        }
    }
}

void Renderer::RefreshShadowPass() {
    // Re-run the cascade shadow pass for the current camera even when a shadow
    // pass already ran this frame (e.g. the editor pre-ran one with its free
    // orbit camera before play resolved the game camera). This keeps the light
    // frusta locked to the ACTUAL render view so orbiting the editor camera
    // slides the shadows incorrectly.
    if (!shadowSystem_.Enabled()) return;
    shadowSystem_.RunPass(sceneState_.ActiveCamera(), sceneState_.ViewAspect(),
                          sceneState_.SunDir(), sceneState_.PointPos(),
                          sceneState_.PointRadius(), sceneState_.PointCount());
    RebindMainTarget();
    if (draw2d_.SceneViewportActive()) {
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
    exposure_ = exposure;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: composite exposure = %.3f", exposure_);
}

void Renderer::SetTonemapEnabled(bool enabled) {
    tonemapEnabled_ = enabled;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: tonemap %s", enabled ? "enabled" : "disabled (legacy clamp)");
}

void Renderer::SetMsaaEnabled(bool enabled) {
    msaaRequested_ = enabled;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: MSAA %s", enabled ? "requested" : "disabled by flag");
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
    if (!mesh.Valid()) return;
    Flush2D();

    if (sceneState_.FrustumValid() &&
        !sceneState_.Frustum().Intersects(math::TransformAABB(mesh.Bounds(), model)))
        return;

    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent &&
        material.castShadow)
        shadowSystem_.RecordCaster({mesh.Handle(), model, {}, {}, 0, mesh.Bounds()});
    // SSAO/SSR have their own colour-encoded depth pre-pass and do NOT depend
    // on CSM being enabled: collect the caster whenever one is active.
    if ((ssaoEnabled_ || ssrEnabled_) && !material.transparent)
        ssaoCasters_.push_back({mesh.Handle(), model, {}, {}, 0, mesh.Bounds()});

    ShaderHandle shader = material.shader.Valid() ? material.shader
                                                  : (material.lit ? litShader_ : unlitShader_);
    ApplyMaterial(material, sceneState_.ViewProjection() * model, model, NormalMatrix(model),
                  shader);
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

void Renderer::DrawSkinnedMesh(const Mesh& mesh, const Material& material,
                               const math::Mat4& model,
                               const std::vector<math::Mat4>& boneMatrices, int boneCount) {
    if (!mesh.Valid()) return;
    Flush2D();

    if (sceneState_.FrustumValid() &&
        !sceneState_.Frustum().Intersects(math::TransformAABB(mesh.Bounds(), model)))
        return;

    // Upload up to 64 bone matrices as one contiguous row-major array.
    int count = boneCount >= 0 ? std::min(boneCount, static_cast<int>(boneMatrices.size()))
                               : static_cast<int>(boneMatrices.size());
    count = std::min(count, 64);

    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent)
        shadowSystem_.RecordCaster({mesh.Handle(), model, {}, boneMatrices, count, mesh.Bounds()});
    if ((ssaoEnabled_ || ssrEnabled_) && !material.transparent)
        ssaoCasters_.push_back({mesh.Handle(), model, {}, boneMatrices, count, mesh.Bounds()});

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

    if (shadowSystem_.Enabled() && shadowSystem_.Recording() && !material.transparent)
        shadowSystem_.RecordCaster(
            {mesh.Handle(), math::Mat4::Identity(), instancedVisible_, {}, 0, mesh.Bounds()});
    if ((ssaoEnabled_ || ssrEnabled_) && !material.transparent)
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
                              BlendMode blend, float intensity) {
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

    std::vector<math::Mat4> models(static_cast<size_t>(count));
    std::vector<math::Vec4> colc(static_cast<size_t>(count));
    for (uint32_t i = 0; i < count; ++i) {
        const float s = sizes[i];
        math::Mat4 m; // identity; fill the basis columns below
        // Local +X -> camera right, +Y -> camera up, +Z -> toward camera.
        m.m[0] = right.x * s;  m.m[4] = right.y * s;  m.m[8] = right.z * s;
        m.m[1] = up.x * s;     m.m[5] = up.y * s;     m.m[9] = up.z * s;
        m.m[2] = -fwd.x * s;   m.m[6] = -fwd.y * s;   m.m[10] = -fwd.z * s;
        m.m[3] = positions[i].x;
        m.m[7] = positions[i].y;
        m.m[11] = positions[i].z;
        models[i] = m;
        // Multiply RGB by `intensity` so additive glow particles emit HDR
        // values > 1.0 and the bloom pass picks them up (the "big game" glow).
        colc[i] = {colors[i].r * intensity, colors[i].g * intensity,
                   colors[i].b * intensity, colors[i].a};
    }

    Material mat = Material::Unlit(texture, Color::White);
    mat.transparent = true; // pick any; blend mode is overridden below
    mat.doubleSided = true;
    ApplyMaterial(mat, sceneState_.ViewProjection(), math::Mat4::Identity(),
                  math::Mat4::Identity(), unlitInstancedColoredShader_);
    // Particles blend appropriately and respect the scene depth (unlike the
    // screen-space DrawBillboard helper which is depth-unaware).
    backend_->SetBlendMode(blend);
    backend_->SetDepthTest(sceneState_.DepthAvailable(), false);
    backend_->SetCullMode(CullMode::None);
    backend_->DrawMeshInstancedColored(billboardQuad_.Handle(), models.data(), colc.data(),
                                       count);
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
    backend_->SetDepthTest(sceneState_.DepthAvailable(), !material.transparent);
    backend_->SetBlendMode(material.transparent ? BlendMode::Alpha : BlendMode::Opaque);

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

    if (material.lit) {
        backend_->SetUniformMat4("uModel", model);
        backend_->SetUniformMat4("uNormalMat", normalMat);
        backend_->SetUniformFloat("uShininess", material.shininess);
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
        backend_->SetUniformFloat("uIblStrength", sceneState_.IblStrength());
        backend_->SetUniformFloat("uRoughnessMin", ibl::kRoughnessMin);
        backend_->BindTexture(20, sceneState_.IblIrradianceTex());
        backend_->SetUniformInt("uIrradianceMap", 20);
        backend_->BindTexture(21, sceneState_.IblPrefilteredTex());
        backend_->SetUniformInt("uPrefilteredMap", 21);
        backend_->BindTexture(22, sceneState_.IblBrdfLutTex());
        backend_->SetUniformInt("uBrdfLUT", 22);
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

void Renderer::DrawBox(const math::AABB& box, const Color& color) {
    math::Vec3 c[8] = {
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
}

void Renderer::ResetSceneViewport() {
    draw2d_.ResetSceneViewport();
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
    if (hdrRT_.Valid() && hdrW_ == screenW_ && hdrH_ == screenH_) return;
    DestroyHdrTargets();
    const int hw = std::max(screenW_ / 2, 1);
    const int hh = std::max(screenH_ / 2, 1);
    const int qw = std::max(screenW_ / 4, 1);
    const int qh = std::max(screenH_ / 4, 1);
    hdrRT_ = backend_->CreateRenderTarget(screenW_, screenH_, true);
    if (!hdrRT_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "Renderer: HDR target %dx%d creation failed -> HDR/bloom disabled", screenW_,
                     screenH_);
        hdrEnabled_ = false;
        return;
    }
    if (msaaEnabled_) {
        // MSAA scene target: resolves into hdrRT_ (the post-chain source)
        // before the graph executes. Only the HDR main target is multisampled.
        hdrMsaaRT_ = backend_->CreateRenderTarget(screenW_, screenH_, true, msaaSamples_);
        if (!hdrMsaaRT_.Valid()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: MSAA target %dx%d (samples=%d) failed -> single-sample HDR",
                         screenW_, screenH_, msaaSamples_);
            msaaEnabled_ = false;
        }
    }
    hdrW_ = screenW_;
    hdrH_ = screenH_;
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
    shaders.ssaoBlur = ssaoBlurShader_;
    shaders.volumetricShader = volumetricShader_;
    shaders.ssrShader = ssrShader_;
    shaders.brightPass = brightPassShader_;
    shaders.blur = blurShader_;
    shaders.downsample = downsampleShader_;
    shaders.upsampleAdd = upsampleAddShader_;
    shaders.compositeShader = compositeShader_;
    shaders.white = white_;
    postGraph_.Build(shaders, postQuadMesh_, screenW_, screenH_,
                     [this] { DrawSsaoDepthCasters(sceneState_.ViewProjection()); });
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: HDR target %dx%d (RGBA16F%s) + bloom %dx%d / %dx%d", screenW_,
                 screenH_, msaaEnabled_ ? ", MSAA" : "", hw, hh, qw, qh);
}

void Renderer::DestroyHdrTargets() {
    auto destroy = [this](RenderTargetHandle& t) {
        if (t.Valid() && backend_) backend_->DestroyRenderTarget(t);
        t = {};
    };
    destroy(hdrMsaaRT_);
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
    // Try 4x first (the target sample count), then 2x for drivers that only
    // handle lower counts; either way the resolved image must round-trip the
    // drawn colour through the same FBO + blit path the frame uses.
    const int attempts[2] = {4, 2};
    for (int samples : attempts) {
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
    if (msaaEnabled_ && hdrMsaaRT_.Valid() && hdrRT_.Valid()) {
        backend_->ResolveRenderTarget(hdrMsaaRT_, hdrRT_);
    }
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
    p.hdrScene = hdrRT_;
    p.hdrW = hdrW_;
    p.hdrH = hdrH_;
    // Depth pre-pass is needed by SSAO/SSR and by the composite's volumetric
    // fog; the SSAO chain additionally requires scene casters (an empty scene
    // would produce an all-far depth whose AO is a no-op white). chains=false
    // forces every chain off (the old CaptureBloom/TonemapComparison path never
    // ran the post graph, so its composites had no AO/vol/SSR/fog terms).
    p.depthPass = chains && (ssaoEnabled_ || ssrEnabled_ || volumetricEnabled_ ||
                             sceneState_.VolumetricFogEnabled());
    p.ssaoPass = chains && ssaoEnabled_ && !ssaoCasters_.empty();
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
        p.sceneVpRect = {0.0f, 0.0f, static_cast<float>(screenW_),
                         static_cast<float>(screenH_)};
    }
    p.camera = sceneState_.ActiveCamera();
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
    p.composite.colorGrade = colorGrade_;
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
    ResolveMainTarget();
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
        } else if (!draw.bones.empty()) {
            backend_->UseShader(shadowSystem_.SkinnedDepthShader());
            boneUniformFlat_.resize(static_cast<size_t>(draw.boneCount) * 16);
            for (int i = 0; i < draw.boneCount; ++i)
                std::memcpy(boneUniformFlat_.data() + static_cast<size_t>(i) * 16,
                            draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
            backend_->SetUniformMat4Array("uBoneMatrices", boneUniformFlat_.data(), draw.boneCount);
            backend_->SetUniformMat4("uMVP", viewProj * draw.model);
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
