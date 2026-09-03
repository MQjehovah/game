#include "neon/gfx/post_graph.hpp"

#include <algorithm>

#include "neon/gfx/bloom.hpp"
#include "neon/gfx/ssao.hpp"
#include "neon/gfx/ssr.hpp"
#include "neon/gfx/volumetric.hpp"

namespace neon::gfx {

namespace {
// Non-zero format tag => backend's floatColor (RGBA16F) target, matching the
// renderer's `CreateRenderTarget(w, h, true)` for the AO/vol/SSR/bloom RTs.
constexpr uint32_t kFloatFormat = 1;
// Far code (1,0,0,0): sky / no-geometry pixels decode to depth 1.0 (matches
// the renderer's RunSceneDepthPass clear).
constexpr Color kFarDepth{1.0f, 0.0f, 0.0f, 0.0f};
} // namespace

void PostGraph::Build(const Shaders& shaders, MeshHandle postQuad, int w, int h,
                      std::function<void()> drawDepthCasters) {
    ssaoShader_ = shaders.ssaoShader;
    ssaoBlur_ = shaders.ssaoBlur;
    volumetricShader_ = shaders.volumetricShader;
    ssrShader_ = shaders.ssrShader;
    bright_ = shaders.brightPass;
    blur_ = shaders.blur;
    downsample_ = shaders.downsample;
    upsampleAdd_ = shaders.upsampleAdd;
    compositeShader_ = shaders.compositeShader;
    postQuad_ = postQuad;
    drawDepthCasters_ = std::move(drawDepthCasters);
    hdrW_ = w;
    hdrH_ = h;
    const int aw = std::max(w / 2, 1);
    const int ah = std::max(h / 2, 1);
    const int qw = std::max(w / 4, 1);
    const int qh = std::max(h / 4, 1);
    halfTexelX_ = 1.0f / static_cast<float>(aw);
    halfTexelY_ = 1.0f / static_cast<float>(ah);
    quarterTexelX_ = 1.0f / static_cast<float>(qw);
    quarterTexelY_ = 1.0f / static_cast<float>(qh);

    FrameGraph fresh;
    // hdrScene_ is declared (so reads validate) but never written by a pass:
    // Execute() injects the renderer's HDR target as an external input.
    hdrScene_ = fresh.AddResource({static_cast<uint32_t>(w), static_cast<uint32_t>(h), 0u, 1u});
    // Depth is colour-encoded (RGBA8), same as the renderer's ssaoDepthRT_.
    sceneDepth_ = fresh.AddResource({static_cast<uint32_t>(w), static_cast<uint32_t>(h), 0u, 1u});
    ao_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    aoBlurA_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    aoBlurB_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    vol_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    volBlurA_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    volBlurB_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    ssr_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    ssrBlurA_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    ssrBlurB_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    bloomHalfA_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    bloomHalfB_ = fresh.AddResource({static_cast<uint32_t>(aw), static_cast<uint32_t>(ah), kFloatFormat, 1u});
    bloomQuarterA_ = fresh.AddResource({static_cast<uint32_t>(qw), static_cast<uint32_t>(qh), kFloatFormat, 1u});
    bloomQuarterB_ = fresh.AddResource({static_cast<uint32_t>(qw), static_cast<uint32_t>(qh), kFloatFormat, 1u});

    size_t nextPass = 0;
    const auto add = [&](FramePass p) {
        const size_t idx = nextPass;
        const bool ok = fresh.AddPass(std::move(p));
        // All ids above are declared, so AddPass always succeeds.
        (void)ok;
        ++nextPass;
        return idx;
    };

    // 1. Depth pre-pass: clears the colour-encoded depth to far code and draws
    //    the scene's casters directly (NOT a fullscreen quad) via the injected
    //    callback, mirroring the old RunSceneDepthPass.
    FramePass depth;
    depth.name = "post.depth";
    depth.writes = {sceneDepth_};
    depth.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(sceneDepth_));
        backend.SetBlendMode(BlendMode::Opaque);
        backend.SetDepthTest(false, false);
        backend.SetCullMode(CullMode::None);
        backend.Clear(kFarDepth, 1.0f);
        if (drawDepthCasters_) drawDepthCasters_();
    };
    depthPassIndex_ = add(std::move(depth));

    // 2. AO compute: samples the colour-encoded scene depth, writes ao.
    FramePass ssao;
    ssao.name = "post.ssao";
    ssao.reads = {sceneDepth_};
    ssao.writes = {ao_};
    ssao.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(ao_));
        Fullscreen(backend, ssaoShader_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(sceneDepth_)));
        backend.SetUniformInt("uDepth", 0);
        backend.SetUniformVec2("uTexelSize", math::Vec2{1.0f / static_cast<float>(hdrW_),
                                                        1.0f / static_cast<float>(hdrH_)});
        backend.SetUniformFloat("uRadius", kSsaoRadius);
        backend.SetUniformFloat("uBias", kSsaoBias);
        backend.SetUniformFloat("uPower", kSsaoPower);
        backend.DrawMesh(postQuad_);
    };
    ssaoPassIndex_ = add(std::move(ssao));

    // 3/4. Separable AO blur (H then V), ping-ponging ao/aoBlurA/aoBlurB.
    auto blurPass = [this](const char* name, ShaderHandle shader, ResourceId src, ResourceId dst,
                           const math::Vec2& dir) {
        FramePass p;
        p.name = name;
        p.reads = {src};
        p.writes = {dst};
        p.execute = [this, shader, src, dst, dir](FrameGraphContext& ctx) {
            auto& backend = ctx.Backend();
            backend.BindRenderTarget(ctx.GetOutput(dst));
            Fullscreen(backend, shader);
            backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(src)));
            backend.SetUniformInt("uTex", 0);
            backend.SetUniformVec2("uTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
            backend.SetUniformVec2("uDirection", dir);
            backend.DrawMesh(postQuad_);
        };
        return p;
    };
    ssaoBlurHIndex_ = add(blurPass("post.ssaoBlurH", ssaoBlur_, ao_, aoBlurA_, math::Vec2{1.0f, 0.0f}));
    ssaoBlurVIndex_ = add(blurPass("post.ssaoBlurV", ssaoBlur_, aoBlurA_, aoBlurB_, math::Vec2{0.0f, 1.0f}));

    // 5. Volumetric: depth-aware volume ray-march toward the sun. Each pixel
    //    steps along its view ray, accumulating the sun's phase-scattered light
    //    attenuated by the scene depth (geometry in front of a step occludes it,
    //    so tree canopies leave dark shafts and open sky glows — the god-ray
    //    look). Sampled at full/vol res via the post quad's normalized UV.
    FramePass vol;
    vol.name = "post.volumetric";
    vol.reads = {hdrScene_, sceneDepth_};
    vol.writes = {vol_};
    vol.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(vol_));
        Fullscreen(backend, volumetricShader_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_)));
        backend.SetUniformInt("uScene", 0);
        backend.BindTexture(1, backend.RenderTargetColorTexture(ctx.GetInput(sceneDepth_)));
        backend.SetUniformInt("uDepth", 1);
        backend.SetUniformVec2("uTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
        backend.SetUniformVec2("uSunScreen", sunUV_);
        backend.SetUniformVec3("uCamPos", camPos_);
        backend.SetUniformVec3("uSunDir", sunDir_);
        backend.SetUniformVec3("uSunColor", sunColor_);
        backend.SetUniformMat4("uViewProj", viewProj_);
        backend.SetUniformFloat("uNear", nearPlane_);
        backend.SetUniformFloat("uFar", farPlane_);
        backend.SetUniformFloat("uDensity", kVolumetricDensity);
        backend.SetUniformFloat("uWeight", kVolumetricWeight);
        backend.SetUniformFloat("uDecay", kVolumetricDecay);
        backend.SetUniformFloat("uThreshold", kVolumetricThreshold);
        backend.SetUniformInt("uSteps", kVolumetricSteps);
        backend.DrawMesh(postQuad_);
    };
    volPassIndex_ = add(std::move(vol));

    // 6/7. Volumetric blur (H then V), ping-ponging vol/volBlurA/volBlurB.
    volBlurHIndex_ = add(blurPass("post.volBlurH", ssaoBlur_, vol_, volBlurA_, math::Vec2{1.0f, 0.0f}));
    volBlurVIndex_ = add(blurPass("post.volBlurV", ssaoBlur_, volBlurA_, volBlurB_, math::Vec2{0.0f, 1.0f}));

    // 8. SSR: ray-marches the reflected view ray in screen space against the
    //    scene depth, pulling the HDR colour.
    FramePass ssr;
    ssr.name = "post.ssr";
    ssr.reads = {hdrScene_, sceneDepth_};
    ssr.writes = {ssr_};
    ssr.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(ssr_));
        Fullscreen(backend, ssrShader_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_)));
        backend.SetUniformInt("uScene", 0);
        backend.BindTexture(1, backend.RenderTargetColorTexture(ctx.GetInput(sceneDepth_)));
        backend.SetUniformInt("uDepth", 1);
        backend.SetUniformVec2("uTexelSize", math::Vec2{1.0f / static_cast<float>(hdrW_),
                                                        1.0f / static_cast<float>(hdrH_)});
        backend.SetUniformFloat("uNear", nearPlane_);
        backend.SetUniformFloat("uFar", farPlane_);
        backend.SetUniformFloat("uSteps", static_cast<float>(kSsrSteps));
        backend.SetUniformFloat("uThickness", kSsrThickness);
        backend.SetUniformFloat("uMaxDist", kSsrMaxDist);
        backend.DrawMesh(postQuad_);
    };
    ssrPassIndex_ = add(std::move(ssr));

    // 9/10. SSR blur (H then V), ping-ponging ssr/ssrBlurA/ssrBlurB.
    ssrBlurHIndex_ = add(blurPass("post.ssrBlurH", ssaoBlur_, ssr_, ssrBlurA_, math::Vec2{1.0f, 0.0f}));
    ssrBlurVIndex_ = add(blurPass("post.ssrBlurV", ssaoBlur_, ssrBlurA_, ssrBlurB_, math::Vec2{0.0f, 1.0f}));

    // 11. Bloom bright pass: HDR -> bloomHalfA (thresholded, only pixels above
    //     1.0).
    FramePass bright;
    bright.name = "bloom.bright";
    bright.reads = {hdrScene_};
    bright.writes = {bloomHalfA_};
    bright.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(bloomHalfA_));
        Fullscreen(backend, bright_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_)));
        backend.SetUniformInt("uTex", 0);
        backend.SetUniformFloat("uThreshold", comp_.bloomThreshold);
        backend.DrawMesh(postQuad_);
    };
    brightPassIndex_ = add(std::move(bright));

    // 12/13. Blur the half-res bright (H then V), ping-ponging bloomHalfA/B.
    auto bloomBlur = [this](const char* name, ResourceId src, ResourceId dst, bool half,
                            float dx, float dy) {
        FramePass p;
        p.name = name;
        p.reads = {src};
        p.writes = {dst};
        p.execute = [this, src, dst, half, dx, dy](FrameGraphContext& ctx) {
            auto& backend = ctx.Backend();
            backend.BindRenderTarget(ctx.GetOutput(dst));
            Fullscreen(backend, blur_);
            backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(src)));
            backend.SetUniformInt("uTex", 0);
            const math::Vec2 texel = half ? math::Vec2{halfTexelX_, halfTexelY_}
                                          : math::Vec2{quarterTexelX_, quarterTexelY_};
            backend.SetUniformVec2("uTexelSize", texel);
            backend.SetUniformVec2("uDirection", math::Vec2{dx, dy});
            backend.DrawMesh(postQuad_);
        };
        return p;
    };
    blurHalfHIndex_ = add(bloomBlur("bloom.blurHalfH", bloomHalfA_, bloomHalfB_, true, 1.0f, 0.0f));
    blurHalfVIndex_ = add(bloomBlur("bloom.blurHalfV", bloomHalfB_, bloomHalfA_, true, 0.0f, 1.0f));

    // 14. Downsample: bloomHalfA -> bloomQuarterA (2x2 box).
    FramePass downsamplePass;
    downsamplePass.name = "bloom.downsample";
    downsamplePass.reads = {bloomHalfA_};
    downsamplePass.writes = {bloomQuarterA_};
    downsamplePass.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(bloomQuarterA_));
        Fullscreen(backend, downsample_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(bloomHalfA_)));
        backend.SetUniformInt("uTex", 0);
        backend.SetUniformVec2("uSrcTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
        backend.DrawMesh(postQuad_);
    };
    downsamplePassIndex_ = add(std::move(downsamplePass));

    // 15/16. Blur the quarter-res level (H then V), ping-ponging quarterA/B.
    blurQuarterHIndex_ =
        add(bloomBlur("bloom.blurQuarterH", bloomQuarterA_, bloomQuarterB_, false, 1.0f, 0.0f));
    blurQuarterVIndex_ =
        add(bloomBlur("bloom.blurQuarterV", bloomQuarterB_, bloomQuarterA_, false, 0.0f, 1.0f));

    // 17. Upsample-add (progressive bloom): bloomHalfB = bloomHalfA +
    //     up(bloomQuarterA). The output (bloomHalfB) is the accumulated bloom
    //     the composite pass samples.
    FramePass upsample;
    upsample.name = "bloom.upsampleAdd";
    upsample.reads = {bloomHalfA_, bloomQuarterA_};
    upsample.writes = {bloomHalfB_};
    upsample.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(bloomHalfB_));
        Fullscreen(backend, upsampleAdd_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(bloomHalfA_)));
        backend.SetUniformInt("uHalf", 0);
        backend.BindTexture(1, backend.RenderTargetColorTexture(ctx.GetInput(bloomQuarterA_)));
        backend.SetUniformInt("uQuarter", 1);
        backend.DrawMesh(postQuad_);
    };
    upsampleAddIndex_ = add(std::move(upsample));

    // 18. Composite: draws the final image to the DEFAULT target (backbuffer,
    //     an out-of-graph target) sampling the scene HDR plus the exported
    //     finals (bloom accumulation, raw AO, blurred volumetric / SSR, scene
    //     depth for volumetric fog). Each term binds its texture only when the
    //     corresponding chain produced a live target this frame (invalid
    //     otherwise), so a disabled chain blends white + uXEnabled=0 exactly
    //     like the old hand-written composite.
    FramePass composite;
    composite.name = "post.composite";
    composite.reads = {hdrScene_, sceneDepth_, ao_, volBlurB_, ssrBlurB_, bloomHalfB_};
    composite.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        if (!compositeShader_.Valid() || !postQuad_.Valid()) return;
        backend.BindDefaultTarget();
        backend.SetBlendMode(BlendMode::Opaque);
        backend.SetDepthTest(false, false);
        backend.SetCullMode(CullMode::None);
        backend.UseShader(compositeShader_);
        backend.SetUniformMat4("uMVP", math::Mat4::Identity());
        const TextureHandle hdr = backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_));
        if (!hdr.Valid()) return; // no live scene -> nothing to composite
        backend.BindTexture(0, hdr);
        backend.SetUniformInt("uHdr", 0);
        // The bloom term samples the graph's bloomHalfB_ (upsample-add output);
        // when bloom is off / unavailable, bind the HDR texture on the bloom
        // slot too (the program still references the sampler) and skip the
        // term, so the `--no-bloom` image differs only by the bloom term.
        const RenderTargetHandle bloomRt = ctx.GetInput(bloomHalfB_);
        const bool bloomActive = bloomRt.Valid();
        if (bloomActive) {
            backend.BindTexture(1, backend.RenderTargetColorTexture(bloomRt));
        } else {
            backend.BindTexture(1, hdr);
        }
        backend.SetUniformFloat("uStrength", comp_.bloomStrength);
        backend.SetUniformInt("uBloomEnabled", bloomActive ? 1 : 0);
        backend.SetUniformInt("uBloom", 1);
        const RenderTargetHandle aoRt = ctx.GetInput(ao_);
        const bool aoActive = aoRt.Valid();
        if (aoActive) {
            backend.BindTexture(2, backend.RenderTargetColorTexture(aoRt));
        } else {
            backend.BindTexture(2, comp_.white);
        }
        backend.SetUniformInt("uAoEnabled", aoActive ? 1 : 0);
        backend.SetUniformInt("uAo", 2);
        backend.SetUniformFloat("uAoIntensity", comp_.ssaoIntensity);
        const RenderTargetHandle volRt = ctx.GetInput(volBlurB_);
        const bool volActive = volRt.Valid();
        if (volActive) {
            backend.BindTexture(3, backend.RenderTargetColorTexture(volRt));
        } else {
            backend.BindTexture(3, comp_.white);
        }
        backend.SetUniformInt("uVolEnabled", volActive ? 1 : 0);
        backend.SetUniformInt("uVol", 3);
        backend.SetUniformFloat("uVolStrength", comp_.volStrength);
        const RenderTargetHandle ssrRt = ctx.GetInput(ssrBlurB_);
        const bool ssrActive = ssrRt.Valid();
        if (ssrActive) {
            backend.BindTexture(4, backend.RenderTargetColorTexture(ssrRt));
        } else {
            backend.BindTexture(4, comp_.white);
        }
        backend.SetUniformInt("uSsrEnabled", ssrActive ? 1 : 0);
        backend.SetUniformInt("uSsr", 4);
        backend.SetUniformFloat("uSsrStrength", comp_.ssrStrength);
        const RenderTargetHandle depthRt = ctx.GetInput(sceneDepth_);
        const bool fogDepthActive = comp_.volumetricFog && depthRt.Valid();
        if (fogDepthActive) {
            backend.BindTexture(5, backend.RenderTargetColorTexture(depthRt));
        } else {
            backend.BindTexture(5, comp_.white);
        }
        backend.SetUniformInt("uFogEnabled", fogDepthActive ? 1 : 0);
        backend.SetUniformInt("uFogDepth", 5);
        backend.SetUniformVec3("uFogColor", comp_.fogColor);
        backend.SetUniformFloat("uFogDensity", comp_.fogDensity);
        backend.SetUniformFloat("uNear", nearPlane_);
        backend.SetUniformFloat("uFar", farPlane_);
        backend.SetUniformFloat("uExposure", comp_.exposure);
        backend.SetUniformInt("uTonemapEnabled", comp_.tonemapEnabled ? 1 : 0);
        backend.DrawMesh(postQuad_);
    };
    compositePassIndex_ = add(std::move(composite));

    graph_ = std::move(fresh);
    built_ = true;
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
    bloomRan_ = false;
    compositeRan_ = false;
}

void PostGraph::Destroy(IRenderBackend& backend) {
    graph_.DestroyResources(backend);
    built_ = false;
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
    bloomRan_ = false;
    compositeRan_ = false;
}

bool PostGraph::Execute(IRenderBackend& backend, const FrameParams& params) {
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
    bloomRan_ = false;
    compositeRan_ = false;
    if (!built_ || !postQuad_.Valid()) return false;
    if (params.hdrW <= 0 || params.hdrH <= 0) return false;

    // Per-chain enable: a chain runs only when requested AND its shaders are
    // valid AND (for the chains sampling the scene) the HDR target is live.
    const bool ssao = params.ssaoPass && ssaoShader_.Valid() && ssaoBlur_.Valid();
    const bool vol = params.volumetricPass && volumetricShader_.Valid() && ssaoBlur_.Valid() &&
                     params.hdrScene.Valid();
    const bool ssr = params.ssrPass && ssrShader_.Valid() && ssaoBlur_.Valid() &&
                     params.hdrScene.Valid();
    const bool bloom = params.bloomPass && bright_.Valid() && blur_.Valid() &&
                       downsample_.Valid() && upsampleAdd_.Valid() && params.hdrScene.Valid();
    // Depth must run whenever any chain samples the scene depth (belt and
    // braces on top of the renderer's own depthPass flag).
    const bool depth = params.depthPass || ssao || ssr || params.composite.volumetricFog;

    graph_.SetPassEnabled(depthPassIndex_, depth);
    graph_.SetPassEnabled(ssaoPassIndex_, ssao);
    graph_.SetPassEnabled(ssaoBlurHIndex_, ssao);
    graph_.SetPassEnabled(ssaoBlurVIndex_, ssao);
    graph_.SetPassEnabled(volPassIndex_, vol);
    graph_.SetPassEnabled(volBlurHIndex_, vol);
    graph_.SetPassEnabled(volBlurVIndex_, vol);
    graph_.SetPassEnabled(ssrPassIndex_, ssr);
    graph_.SetPassEnabled(ssrBlurHIndex_, ssr);
    graph_.SetPassEnabled(ssrBlurVIndex_, ssr);
    graph_.SetPassEnabled(brightPassIndex_, bloom);
    graph_.SetPassEnabled(blurHalfHIndex_, bloom);
    graph_.SetPassEnabled(blurHalfVIndex_, bloom);
    graph_.SetPassEnabled(downsamplePassIndex_, bloom);
    graph_.SetPassEnabled(blurQuarterHIndex_, bloom);
    graph_.SetPassEnabled(blurQuarterVIndex_, bloom);
    graph_.SetPassEnabled(upsampleAddIndex_, bloom);
    // The composite is the chain's terminal pass: it always runs so the HDR
    // scene reaches the backbuffer (its execute guards on shader/input validity
    // and draws nothing when the composite program is missing).
    graph_.SetPassEnabled(compositePassIndex_, true);

    comp_ = params.composite;
    if (vol) {
        // Project the sun (far along sunDir) to screen UV for the ray origin,
        // mirroring the renderer's RunVolumetricPass.
        const math::Vec3 sunWorld = params.camPos + params.sunDir * 5000.0f;
        const math::Vec4 clip =
            params.viewProj.TransformVec4({sunWorld.x, sunWorld.y, sunWorld.z, 1.0f});
        sunUV_ = math::Vec2{0.5f, 0.5f};
        if (clip.w > 0.0f) {
            const math::Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
            sunUV_ = {ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f};
            sunUV_.y = 1.0f - sunUV_.y; // texture V is top-down in the post sampler
        }
    }
    nearPlane_ = params.camera.nearPlane;
    farPlane_ = params.camera.farPlane;
    viewProj_ = params.viewProj;
    camPos_ = params.camPos;
    sunDir_ = params.sunDir;
    sunColor_ = params.sunColor;

    graph_.SetExternalInput(hdrScene_, params.hdrScene);
    const bool ok = graph_.Execute(backend);
    const bool any = depth || ssao || vol || ssr;
    ran_ = ok && any;
    depthRan_ = ok && depth;
    ssaoRan_ = ok && ssao;
    volRan_ = ok && vol;
    ssrRan_ = ok && ssr;
    bloomRan_ = ok && bloom;
    compositeRan_ = ok;
    return ok;
}

void PostGraph::Fullscreen(IRenderBackend& backend, ShaderHandle shader) {
    backend.SetBlendMode(BlendMode::Opaque);
    backend.SetDepthTest(false, false);
    backend.SetCullMode(CullMode::None);
    backend.UseShader(shader);
    backend.SetUniformMat4("uMVP", math::Mat4::Identity());
}

} // namespace neon::gfx
