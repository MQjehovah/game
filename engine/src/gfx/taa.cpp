#include "neon/gfx/taa.hpp"

#include "neon/core/log.hpp"
#include "neon/gfx/backend.hpp"
#include "neon/gfx/bloom.hpp"

namespace neon::gfx {

bool Taa::Init(IRenderBackend& backend, int width, int height) {
    shader_ = backend.CreateShader(kPostVertexShader, kTaaFragmentShader, "taa");
    if (!shader_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: TAA shader failed -> temporal AA unavailable");
        return false;
    }
    // The sharpen pass is optional: when it fails to compile the resolve still
    // runs, just without the high-pass restore.
    sharpenShader_ = backend.CreateShader(kPostVertexShader, kTaaSharpenFragmentShader, "taa_sharpen");
    if (!sharpenShader_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: TAA sharpen shader failed -> resolve stays soft");
    }
    return Resize(backend, width, height);
}

bool Taa::Resize(IRenderBackend& backend, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (history_[0].Valid() && w_ == width && h_ == height) return true;
    Shutdown(backend, /*keepShader=*/true);
    history_[0] = backend.CreateRenderTarget(width, height, true);
    history_[1] = backend.CreateRenderTarget(width, height, true);
    scratch_ = backend.CreateRenderTarget(width, height, true);
    if (!history_[0].Valid() || !history_[1].Valid() || !scratch_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: TAA history target %dx%d failed -> temporal AA unavailable",
                     width, height);
        Shutdown(backend, /*keepShader=*/true);
        return false;
    }
    w_ = width;
    h_ = height;
    write_ = 0;
    return true;
}

void Taa::Shutdown(IRenderBackend& backend, bool keepShader) {
    for (RenderTargetHandle& rt : history_) {
        if (rt.Valid()) backend.DestroyRenderTarget(rt);
        rt = {};
    }
    if (scratch_.Valid()) backend.DestroyRenderTarget(scratch_);
    scratch_ = {};
    w_ = h_ = 0;
    write_ = 0;
    if (!keepShader) {
        if (shader_.Valid()) backend.DestroyShader(shader_);
        if (sharpenShader_.Valid()) backend.DestroyShader(sharpenShader_);
        shader_ = {};
        sharpenShader_ = {};
    }
}

void Taa::SetSharpen(float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.5f) amount = 1.5f;
    sharpen_ = amount;
}

RenderTargetHandle Taa::Resolve(IRenderBackend& backend, RenderTargetHandle current,
                               TextureHandle depth, const math::Mat4& invViewProj,
                               const math::Mat4& prevViewProj, MeshHandle postQuad, float blend,
                               bool validHistory, TextureHandle velocity) {
    if (!shader_.Valid() || !postQuad.Valid()) return {};
    if (!history_[0].Valid() || !history_[1].Valid()) return {};
    if (!current.Valid() || !depth.Valid()) return {};
    const bool doSharpen = sharpen_ > 0.001f && sharpenShader_.Valid() && scratch_.Valid();
    const int readIdx = write_ ^ 1;

    // Pass 1: blend the jittered frame into the history. The accumulator is what
    // gets stored, NOT the sharpened image: feeding the high-pass back into the
    // history would amplify it every frame (unstable ringing).
    backend.BindRenderTarget(history_[write_]);
    backend.SetBlendMode(BlendMode::Opaque);
    backend.SetDepthTest(false, false);
    backend.SetCullMode(CullMode::None);
    backend.UseShader(shader_);
    backend.SetUniformMat4("uMVP", math::Mat4::Identity());
    backend.BindTexture(0, backend.RenderTargetColorTexture(current));
    backend.SetUniformInt("uCurrent", 0);
    backend.BindTexture(1, backend.RenderTargetColorTexture(history_[readIdx]));
    backend.SetUniformInt("uHistory", 1);
    backend.BindTexture(2, depth);
    backend.SetUniformInt("uDepth", 2);
    // Motion vectors share the sampler with whichever target is bound; an
    // invalid handle means "no velocity this frame" and every pixel falls back
    // to the depth reprojection path in the shader.
    backend.BindTexture(3, velocity);
    backend.SetUniformInt("uVelocity", 3);
    backend.SetUniformInt("uHasVelocity", velocity.Valid() ? 1 : 0);
    backend.SetUniformVec2("uScreenSize",
                           {static_cast<float>(w_), static_cast<float>(h_)});
    backend.SetUniformMat4("uInvViewProj", invViewProj);
    backend.SetUniformMat4("uPrevViewProj", prevViewProj);
    backend.SetUniformFloat("uBlend", blend);
    backend.SetUniformInt("uValidHistory", validHistory ? 1 : 0);
    backend.DrawMesh(postQuad);

    // Pass 2: restore the high-frequency detail the accumulation averaged out.
    // Display-side only: the sharpened image is what the post chain consumes,
    // while history_[write_] stays the plain accumulation.
    if (doSharpen) {
        backend.BindRenderTarget(scratch_);
        backend.BindTexture(0, backend.RenderTargetColorTexture(history_[write_]));
        backend.SetBlendMode(BlendMode::Opaque);
        backend.SetDepthTest(false, false);
        backend.SetCullMode(CullMode::None);
        backend.UseShader(sharpenShader_);
        backend.SetUniformMat4("uMVP", math::Mat4::Identity());
        backend.SetUniformInt("uSource", 0);
        backend.SetUniformVec2("uScreenSize",
                               {static_cast<float>(w_), static_cast<float>(h_)});
        backend.SetUniformFloat("uSharpen", sharpen_);
        backend.DrawMesh(postQuad);
    }

    const RenderTargetHandle out = doSharpen ? scratch_ : history_[write_];
    write_ ^= 1;
    return out;
}

} // namespace neon::gfx