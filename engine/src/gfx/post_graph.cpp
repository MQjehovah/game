#include "neon/gfx/post_graph.hpp"

#include <algorithm>

#include "neon/gfx/ssao.hpp"
#include "neon/gfx/ssr.hpp"
#include "neon/gfx/volumetric.hpp"

namespace neon::gfx {

namespace {
// Non-zero format tag => backend's floatColor (RGBA16F) target, matching the
// renderer's `CreateRenderTarget(w, h, true)` for the AO/vol/SSR half-res RTs.
constexpr uint32_t kFloatFormat = 1;
// Far code (1,0,0,0): sky / no-geometry pixels decode to depth 1.0 (matches
// the renderer's RunSceneDepthPass clear).
constexpr Color kFarDepth{1.0f, 0.0f, 0.0f, 0.0f};
} // namespace

void PostGraph::Build(ShaderHandle ssaoShader, ShaderHandle ssaoBlur,
                      ShaderHandle volumetricShader, ShaderHandle ssrShader, MeshHandle postQuad,
                      int w, int h, std::function<void()> drawDepthCasters) {
    ssaoShader_ = ssaoShader;
    ssaoBlur_ = ssaoBlur;
    volumetricShader_ = volumetricShader;
    ssrShader_ = ssrShader;
    postQuad_ = postQuad;
    drawDepthCasters_ = std::move(drawDepthCasters);
    hdrW_ = w;
    hdrH_ = h;
    const int aw = std::max(w / 2, 1);
    const int ah = std::max(h / 2, 1);
    halfTexelX_ = 1.0f / static_cast<float>(aw);
    halfTexelY_ = 1.0f / static_cast<float>(ah);

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

    // Exports: the composite samples the scene depth (volumetric fog), the raw
    // AO (not the blurred one), and the final blurred volumetric / SSR results.
    fresh.ExportResource(sceneDepth_);
    fresh.ExportResource(ao_);
    fresh.ExportResource(volBlurB_);
    fresh.ExportResource(ssrBlurB_);

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
    auto blurPass = [this](const char* name, ResourceId src, ResourceId dst,
                           const math::Vec2& dir) {
        FramePass p;
        p.name = name;
        p.reads = {src};
        p.writes = {dst};
        p.execute = [this, src, dst, dir](FrameGraphContext& ctx) {
            auto& backend = ctx.Backend();
            backend.BindRenderTarget(ctx.GetOutput(dst));
            Fullscreen(backend, ssaoBlur_);
            backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(src)));
            backend.SetUniformInt("uTex", 0);
            backend.SetUniformVec2("uTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
            backend.SetUniformVec2("uDirection", dir);
            backend.DrawMesh(postQuad_);
        };
        return p;
    };
    ssaoBlurHIndex_ = add(blurPass("post.ssaoBlurH", ao_, aoBlurA_, math::Vec2{1.0f, 0.0f}));
    ssaoBlurVIndex_ = add(blurPass("post.ssaoBlurV", aoBlurA_, aoBlurB_, math::Vec2{0.0f, 1.0f}));

    // 5. Volumetric: samples the HDR scene radially toward the sun (sunUV
    //    recomputed in Execute from camPos/sunDir/viewProj), writes vol.
    FramePass vol;
    vol.name = "post.volumetric";
    vol.reads = {hdrScene_};
    vol.writes = {vol_};
    vol.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(vol_));
        Fullscreen(backend, volumetricShader_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_)));
        backend.SetUniformInt("uScene", 0);
        backend.SetUniformVec2("uSunScreen", sunUV_);
        backend.SetUniformVec2("uTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
        backend.SetUniformFloat("uDensity", kVolumetricDensity);
        backend.SetUniformFloat("uWeight", kVolumetricWeight);
        backend.SetUniformFloat("uDecay", kVolumetricDecay);
        backend.SetUniformFloat("uThreshold", kVolumetricThreshold);
        backend.SetUniformInt("uSteps", kVolumetricSteps);
        backend.DrawMesh(postQuad_);
    };
    volPassIndex_ = add(std::move(vol));

    // 6/7. Volumetric blur (H then V), ping-ponging vol/volBlurA/volBlurB.
    volBlurHIndex_ = add(blurPass("post.volBlurH", vol_, volBlurA_, math::Vec2{1.0f, 0.0f}));
    volBlurVIndex_ = add(blurPass("post.volBlurV", volBlurA_, volBlurB_, math::Vec2{0.0f, 1.0f}));

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
    ssrBlurHIndex_ = add(blurPass("post.ssrBlurH", ssr_, ssrBlurA_, math::Vec2{1.0f, 0.0f}));
    ssrBlurVIndex_ = add(blurPass("post.ssrBlurV", ssrBlurA_, ssrBlurB_, math::Vec2{0.0f, 1.0f}));

    graph_ = std::move(fresh);
    built_ = true;
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
}

void PostGraph::Destroy(IRenderBackend& backend) {
    graph_.DestroyResources(backend);
    built_ = false;
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
}

bool PostGraph::Execute(IRenderBackend& backend, const FrameParams& params) {
    ran_ = false;
    depthRan_ = false;
    ssaoRan_ = false;
    volRan_ = false;
    ssrRan_ = false;
    if (!built_ || !postQuad_.Valid()) return false;
    if (params.hdrW <= 0 || params.hdrH <= 0) return false;

    // Per-chain enable: a chain runs only when requested AND its shaders are
    // valid AND (for the chains sampling the scene) the HDR target is live.
    const bool ssao = params.ssaoPass && ssaoShader_.Valid() && ssaoBlur_.Valid();
    const bool vol = params.volumetricPass && volumetricShader_.Valid() && ssaoBlur_.Valid() &&
                     params.hdrScene.Valid();
    const bool ssr = params.ssrPass && ssrShader_.Valid() && ssaoBlur_.Valid() &&
                     params.hdrScene.Valid();
    // Depth must run whenever any chain samples the scene depth (belt and
    // braces on top of the renderer's own depthPass flag).
    const bool depth = params.depthPass || ssao || ssr;

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

    graph_.SetExternalInput(hdrScene_, params.hdrScene);
    const bool ok = graph_.Execute(backend);
    const bool any = depth || ssao || vol || ssr;
    ran_ = ok && any;
    depthRan_ = ok && depth;
    ssaoRan_ = ok && ssao;
    volRan_ = ok && vol;
    ssrRan_ = ok && ssr;
    return ok;
}

TextureHandle PostGraph::SceneDepthTexture(IRenderBackend& backend) const {
    const RenderTargetHandle rt = graph_.GetResourceTarget(sceneDepth_);
    return rt.Valid() ? backend.RenderTargetColorTexture(rt) : TextureHandle{};
}

TextureHandle PostGraph::AoTex(IRenderBackend& backend) const {
    const RenderTargetHandle rt = graph_.GetResourceTarget(ao_);
    return rt.Valid() ? backend.RenderTargetColorTexture(rt) : TextureHandle{};
}

TextureHandle PostGraph::VolTex(IRenderBackend& backend) const {
    const RenderTargetHandle rt = graph_.GetResourceTarget(volBlurB_);
    return rt.Valid() ? backend.RenderTargetColorTexture(rt) : TextureHandle{};
}

TextureHandle PostGraph::SsrTex(IRenderBackend& backend) const {
    const RenderTargetHandle rt = graph_.GetResourceTarget(ssrBlurB_);
    return rt.Valid() ? backend.RenderTargetColorTexture(rt) : TextureHandle{};
}

void PostGraph::Fullscreen(IRenderBackend& backend, ShaderHandle shader) {
    backend.SetBlendMode(BlendMode::Opaque);
    backend.SetDepthTest(false, false);
    backend.SetCullMode(CullMode::None);
    backend.UseShader(shader);
    backend.SetUniformMat4("uMVP", math::Mat4::Identity());
}

} // namespace neon::gfx
