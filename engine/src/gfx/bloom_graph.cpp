#include "neon/gfx/bloom_graph.hpp"

#include <algorithm>

#include "neon/gfx/bloom.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

namespace {
// Non-zero format tag => backend's floatColor (RGBA16F) target, matching the
// renderer's `CreateRenderTarget(w, h, true)` for the HDR bloom pyramid.
constexpr uint32_t kFloatFormat = 1;
} // namespace

void BloomGraph::Build(ShaderHandle brightPass, ShaderHandle blur, ShaderHandle downsample,
                       ShaderHandle upsampleAdd, MeshHandle postQuad, int hdrW, int hdrH) {
    bright_ = brightPass;
    blur_ = blur;
    downsample_ = downsample;
    upsampleAdd_ = upsampleAdd;
    postQuad_ = postQuad;

    const int hw = std::max(hdrW / 2, 1);
    const int hh = std::max(hdrH / 2, 1);
    const int qw = std::max(hdrW / 4, 1);
    const int qh = std::max(hdrH / 4, 1);
    halfTexelX_ = 1.0f / static_cast<float>(hw);
    halfTexelY_ = 1.0f / static_cast<float>(hh);
    quarterTexelX_ = 1.0f / static_cast<float>(qw);
    quarterTexelY_ = 1.0f / static_cast<float>(qh);

    FrameGraph fresh;
    // hdrScene_ is declared (so reads validate) but never written by a pass:
    // Execute() injects the renderer's HDR target as an external input.
    hdrScene_ = fresh.AddResource({static_cast<uint32_t>(hdrW), static_cast<uint32_t>(hdrH),
                                   0u, 1u});
    halfA_ = fresh.AddResource({static_cast<uint32_t>(hw), static_cast<uint32_t>(hh),
                                kFloatFormat, 1u});
    halfB_ = fresh.AddResource({static_cast<uint32_t>(hw), static_cast<uint32_t>(hh),
                                kFloatFormat, 1u});
    quarterA_ = fresh.AddResource({static_cast<uint32_t>(qw), static_cast<uint32_t>(qh),
                                   kFloatFormat, 1u});
    quarterB_ = fresh.AddResource({static_cast<uint32_t>(qw), static_cast<uint32_t>(qh),
                                   kFloatFormat, 1u});
    bloomAcc_ = halfB_; // upsample-add output IS the accumulated bloom
    fresh.ExportResource(bloomAcc_);

    // 1. Bright pass: HDR -> halfA (thresholded, only pixels above 1.0).
    FramePass bright;
    bright.name = "bloom.bright";
    bright.reads = {hdrScene_};
    bright.writes = {halfA_};
    bright.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(halfA_));
        Fullscreen(backend, bright_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(hdrScene_)));
        backend.SetUniformInt("uTex", 0);
        backend.SetUniformFloat("uThreshold", kBloomThreshold);
        backend.DrawMesh(postQuad_);
    };
    fresh.AddPass(std::move(bright));

    // 2/3. Blur the half-res bright (H then V), ping-ponging halfA/halfB.
    auto blurPass = [this](const char* name, ResourceId src, ResourceId dst, bool half,
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
    fresh.AddPass(blurPass("bloom.blurHalfH", halfA_, halfB_, true, 1.0f, 0.0f));
    fresh.AddPass(blurPass("bloom.blurHalfV", halfB_, halfA_, true, 0.0f, 1.0f));

    // 4. Downsample: halfA -> quarterA (2x2 box).
    FramePass downsamplePass;
    downsamplePass.name = "bloom.downsample";
    downsamplePass.reads = {halfA_};
    downsamplePass.writes = {quarterA_};
    downsamplePass.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(quarterA_));
        Fullscreen(backend, downsample_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(halfA_)));
        backend.SetUniformInt("uTex", 0);
        backend.SetUniformVec2("uSrcTexelSize", math::Vec2{halfTexelX_, halfTexelY_});
        backend.DrawMesh(postQuad_);
    };
    fresh.AddPass(std::move(downsamplePass));

    // 5/6. Blur the quarter-res level (H then V), ping-ponging quarterA/quarterB.
    fresh.AddPass(blurPass("bloom.blurQuarterH", quarterA_, quarterB_, false, 1.0f, 0.0f));
    fresh.AddPass(blurPass("bloom.blurQuarterV", quarterB_, quarterA_, false, 0.0f, 1.0f));

    // 7. Upsample-add (progressive bloom): halfB = halfA + up(quarterA). The
    //    output (halfB) is exported as bloomAcc_ for the composite pass.
    FramePass upsample;
    upsample.name = "bloom.upsampleAdd";
    upsample.reads = {halfA_, quarterA_};
    upsample.writes = {halfB_};
    upsample.execute = [this](FrameGraphContext& ctx) {
        auto& backend = ctx.Backend();
        backend.BindRenderTarget(ctx.GetOutput(halfB_));
        Fullscreen(backend, upsampleAdd_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(ctx.GetInput(halfA_)));
        backend.SetUniformInt("uHalf", 0);
        backend.BindTexture(1, backend.RenderTargetColorTexture(ctx.GetInput(quarterA_)));
        backend.SetUniformInt("uQuarter", 1);
        backend.DrawMesh(postQuad_);
    };
    fresh.AddPass(std::move(upsample));

    graph_ = std::move(fresh);
    built_ = true;
    ran_ = false;
}

void BloomGraph::Destroy(IRenderBackend& backend) {
    graph_.DestroyResources(backend);
    built_ = false;
    ran_ = false;
}

bool BloomGraph::Execute(IRenderBackend& backend, RenderTargetHandle hdrScene, int hdrW,
                         int hdrH, bool enabled) {
    ran_ = false;
    if (!enabled) return false;
    if (!built_ || !postQuad_.Valid() || !bright_.Valid() || !blur_.Valid() ||
        !downsample_.Valid() || !upsampleAdd_.Valid() || !hdrScene.Valid()) {
        return false;
    }
    // Texel sizes derive from the current HDR resolution (normally identical to
    // the resolution Build() declared the resources at).
    const int hw = std::max(hdrW / 2, 1);
    const int hh = std::max(hdrH / 2, 1);
    const int qw = std::max(hdrW / 4, 1);
    const int qh = std::max(hdrH / 4, 1);
    halfTexelX_ = 1.0f / static_cast<float>(hw);
    halfTexelY_ = 1.0f / static_cast<float>(hh);
    quarterTexelX_ = 1.0f / static_cast<float>(qw);
    quarterTexelY_ = 1.0f / static_cast<float>(qh);
    graph_.SetExternalInput(hdrScene_, hdrScene);
    const bool ok = graph_.Execute(backend);
    ran_ = ok;
    return ok;
}

TextureHandle BloomGraph::BloomColorTexture(IRenderBackend& backend) const {
    const RenderTargetHandle rt = graph_.GetResourceTarget(bloomAcc_);
    return rt.Valid() ? backend.RenderTargetColorTexture(rt) : TextureHandle{};
}

void BloomGraph::Fullscreen(IRenderBackend& backend, ShaderHandle shader) {
    backend.SetBlendMode(BlendMode::Opaque);
    backend.SetDepthTest(false, false);
    backend.SetCullMode(CullMode::None);
    backend.UseShader(shader);
    backend.SetUniformMat4("uMVP", math::Mat4::Identity());
}

} // namespace neon::gfx
