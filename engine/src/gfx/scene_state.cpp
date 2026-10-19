#include "neon/gfx/scene_state.hpp"

#include <algorithm>
#include <chrono>
#include <future>

#include "neon/core/log.hpp"
#include "neon/gfx/ibl.hpp"

namespace neon::gfx {

void SceneState::Shutdown(IRenderBackend& backend) {
    if (iblIrradianceTex_.Valid()) backend.DestroyTexture(iblIrradianceTex_);
    if (iblPrefilteredTex_.Valid()) backend.DestroyTexture(iblPrefilteredTex_);
    if (iblBrdfLutTex_.Valid()) backend.DestroyTexture(iblBrdfLutTex_);
    iblIrradianceTex_ = {};
    iblPrefilteredTex_ = {};
    iblBrdfLutTex_ = {};
    iblValid_ = false;
}

void SceneState::SetCamera(const Camera& camera, float aspect) {
    camera_ = camera;
    viewAspect_ = aspect > 0.01f ? aspect : viewAspect_;
    viewProj_ = camera.ViewProjection(aspect);
    view_ = camera.View();
    camPos_ = camera.position;
    frustum_ = math::Frustum::FromViewProjection(viewProj_);
    frustumValid_ = true;
}

void SceneState::SetProjectionJitter(float jitterXPixels, float jitterYPixels, int width,
                                     int height) {
    // Rebuild from the last camera so repeated calls in a frame are idempotent
    // (the jitter is not compounded).
    viewProj_ = camera_.ViewProjection(viewAspect_);
    if (width <= 0 || height <= 0) return;
    // clip.x += jx * clip.w shifts the NDC x by a constant jx, so the x row
    // absorbs jx times the w row (and likewise y). Exact for any projection.
    const float jx = 2.0f * jitterXPixels / static_cast<float>(width);
    const float jy = 2.0f * jitterYPixels / static_cast<float>(height);
    for (int c = 0; c < 4; ++c) {
        const float wRow = viewProj_.m[12 + c];
        viewProj_.m[c] += jx * wRow;
        viewProj_.m[4 + c] += jy * wRow;
    }
}

void SceneState::SetSky(const Color& top, const Color& horizon) {
    skyTop_ = top;
    skyHorizon_ = horizon;
    // Lazy IBL recompute. The demo animates the sky every frame (a day/night
    // cycle), so the environment is rebuilt only when the sky has actually
    // moved by a cumulative epsilon AND enough SetSky calls have elapsed since
    // the last rebuild - an animated sky then re-precomputes at most once every
    // kIblRecomputeInterval frames (~20ms, logged) and a static sky never does.
    // Re-enabling IBL (strength 0 -> >0) forces a rebuild via iblValid_.
    if (iblStrength_ <= 0.0f) return;
    ++iblFrameCounter_;
    if (!iblValid_) {
        RecomputeIbl(top, horizon);
        return;
    }
    const float delta = std::max({
        std::fabs(top.r - iblLastTop_.r),     std::fabs(top.g - iblLastTop_.g),
        std::fabs(top.b - iblLastTop_.b),     std::fabs(horizon.r - iblLastHorizon_.r),
        std::fabs(horizon.g - iblLastHorizon_.g), std::fabs(horizon.b - iblLastHorizon_.b),
    });
    iblAccumDelta_ += delta;
    if (iblAccumDelta_ >= kIblSkyEpsilon &&
        iblFrameCounter_ - iblLastRecomputeFrame_ >= kIblRecomputeInterval) {
        RecomputeIbl(top, horizon);
    }
}

void SceneState::SetSkySource(const uint8_t* rgba, int width, int height) {
    skyEquirect_.clear();
    skyEquirectW_ = 0;
    skyEquirectH_ = 0;
    if (!rgba || width <= 0 || height <= 0) {
        // Procedural sky: force a rebuild so the gradient maps come back.
        iblValid_ = false;
        return;
    }
    // Box-downsample to at most 1024 wide: the prefilter's Monte-Carlo loop
    // samples this buffer randomly, so a full 8K HDRI would be cache-hostile
    // for no visual gain (the prefiltered map is 64 texels).
    constexpr int kMaxW = 1024;
    const int dstW = std::min(width, kMaxW);
    const int dstH = std::max(1, height * dstW / std::max(width, 1));
    const int sx = std::max(1, width / dstW);
    const int sy = std::max(1, height / dstH);
    skyEquirect_.resize(static_cast<size_t>(dstW) * dstH * 4);
    for (int y = 0; y < dstH; ++y) {
        for (int x = 0; x < dstW; ++x) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int oy = 0; oy < sy; ++oy) {
                const int yy = std::min(height - 1, y * sy + oy);
                for (int ox = 0; ox < sx; ++ox) {
                    const int xx = std::min(width - 1, x * sx + ox);
                    const uint8_t* p = rgba + (static_cast<size_t>(yy) * width + xx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            }
            uint8_t* d = skyEquirect_.data() + (static_cast<size_t>(y) * dstW + x) * 4;
            d[0] = static_cast<uint8_t>(r / std::max(n, 1));
            d[1] = static_cast<uint8_t>(g / std::max(n, 1));
            d[2] = static_cast<uint8_t>(b / std::max(n, 1));
            d[3] = 255;
        }
    }
    skyEquirectW_ = dstW;
    skyEquirectH_ = dstH;
    iblValid_ = false; // rebuild with the new source
}

void SceneState::SetIblStrength(float strength) {
    strength = std::max(0.0f, std::min(1.0f, strength));
    const bool wasZero = iblStrength_ <= 0.0f;
    iblStrength_ = strength;
    if (wasZero && strength > 0.0f) iblValid_ = false; // rebuild on next SetSky
}

void SceneState::RecomputeIbl(const Color& top, const Color& horizon) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1: IBL uniforms changed
    if (!backend_) return;
    ++iblBuildCount_;
    const auto start = std::chrono::steady_clock::now();

    // BRDF LUT is a pure material term (roughness x NoV), independent of the
    // sky - build and upload it once.
    std::future<std::vector<uint8_t>> brdfFuture;
    if (!iblBrdfLutReady_) {
        brdfFuture = std::async(std::launch::async, []() { return ibl::BuildBrdfLut(); });
    }

    // Sky-dependent maps: irradiance (diffuse) + prefiltered specular. Both are
    // RGBA8: the sky gradient is an LDR environment (all texels <= 1) so the
    // 8-bit upload loses nothing; a future HDR environment would need a
    // float-texture path in the backend.
    // Octahedral (azimuth-aware) maps. When the scene supplies a decoded HDRI
    // the bake samples it directly, so reflections show the photographed sky's
    // actual layout; otherwise the procedural vertical gradient is used.
    const bool haveSource =
        !skyEquirect_.empty() && skyEquirectW_ > 0 && skyEquirectH_ > 0;
    // The three bakes are independent pure functions of the sky/material data.
    // Run them on separate cores to cut the visible startup hitch; the outputs
    // are byte-identical to the serial path.
    auto irrFuture = std::async(std::launch::async, [&]() {
        return haveSource ? ibl::BuildOctaIrradianceFromEquirect(
                                skyEquirect_.data(), skyEquirectW_, skyEquirectH_)
                          : ibl::BuildOctaIrradiance(top, horizon, kIblGradientPower);
    });
    const std::vector<uint8_t> pf =
        haveSource ? ibl::BuildOctaPrefilteredFromEquirect(skyEquirect_.data(), skyEquirectW_,
                                                           skyEquirectH_)
                   : ibl::BuildOctaPrefiltered(top, horizon, kIblGradientPower);
    const std::vector<uint8_t> irr = irrFuture.get();

    if (brdfFuture.valid()) {
        const std::vector<uint8_t> lut = brdfFuture.get();
        if (!lut.empty()) {
            if (iblBrdfLutTex_.Valid()) backend_->DestroyTexture(iblBrdfLutTex_);
            TextureDesc desc;
            desc.width = ibl::kBrdfLutSize;
            desc.height = ibl::kBrdfLutSize;
            desc.rgba = lut.data();
            desc.filter = Filter::Linear;
            iblBrdfLutTex_ = backend_->CreateTexture(desc);
            iblBrdfLutReady_ = iblBrdfLutTex_.Valid();
        }
    }
    if (iblIrradianceTex_.Valid()) backend_->DestroyTexture(iblIrradianceTex_);
    if (iblPrefilteredTex_.Valid()) backend_->DestroyTexture(iblPrefilteredTex_);
    TextureDesc irrDesc;
    irrDesc.width = ibl::kOctaIrradianceSize;
    irrDesc.height = ibl::kOctaIrradianceSize;
    irrDesc.rgba = irr.data();
    irrDesc.filter = Filter::Linear;
    iblIrradianceTex_ = backend_->CreateTexture(irrDesc);
    TextureDesc pfDesc;
    pfDesc.width = ibl::kOctaPrefilterSize;
    pfDesc.height = ibl::kOctaPrefilterSize * ibl::kOctaRoughnessLevels;
    pfDesc.rgba = pf.data();
    pfDesc.filter = Filter::Linear;
    iblPrefilteredTex_ = backend_->CreateTexture(pfDesc);

    iblValid_ = iblIrradianceTex_.Valid() && iblPrefilteredTex_.Valid() && iblBrdfLutTex_.Valid();
    iblLastTop_ = top;
    iblLastHorizon_ = horizon;
    iblAccumDelta_ = 0.0f;
    iblLastRecomputeFrame_ = iblFrameCounter_;

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: IBL environment recomputed (sky %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f) in "
                 "%.1f ms (%s)",
                 top.r, top.g, top.b, horizon.r, horizon.g, horizon.b, ms,
                 iblValid_ ? "ok" : "FAILED");
}

void SceneState::SetFog(const Color& color, float start, float end) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    fogColor_ = color;
    fogStart_ = start;
    fogEnd_ = end;
}

void SceneState::SetDirectionalLight(const math::Vec3& direction, const Color& color,
                                     float ambientStrength) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    sunDir_ = direction.Normalized();
    sunColor_ = color;
    ambient_ = ambientStrength;
}

void SceneState::SetAmbientLight(const Color& color, float strength) {
    ambientColor_ = color;
    ambient_ = strength;
    // B1: like every other light setter - uAmbient/uAmbientColor are cached in
    // the scene uniform block and only re-upload when the stamp changes.
    if (sceneUniformStamp_) ++*sceneUniformStamp_;
}

void SceneState::SetAmbientGroundColor(const Color& color) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    ambientGroundColor_ = color;
}

void SceneState::SetPointLight(int index, const math::Vec3& position, const Color& color,
                               float radius) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    if (index < 0 || index >= kMaxPointLights) return;
    pointPos_[index] = position;
    pointColor_[index] = color;
    pointRadius_[index] = radius;
    pointCount_ = std::max(pointCount_, index + 1);
}

void SceneState::SetSpotLight(int index, const math::Vec3& position, const math::Vec3& direction,
                              const Color& color, float radius, float cosInner, float cosOuter) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    if (index < 0 || index >= kMaxSpotLights) return;
    spotPos_[index] = position;
    spotDir_[index] = direction;
    spotColor_[index] = color;
    spotRadius_[index] = radius;
    spotCosInner_[index] = cosInner;
    spotCosOuter_[index] = cosOuter;
    spotCount_ = std::max(spotCount_, index + 1);
}

void SceneState::SetPlayerLight(const math::Vec3& position, const Color& color, float radius) {
    if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1
    playerLightPos_ = position;
    playerLightColor_ = color;
    playerLightRadius_ = radius;
    playerLightEnabled_ = true;
}

void SceneState::DrawSky(DrawBatch2D& overlay) {
    // 写实天空贴图（HDRI tonemapped JPG）：全屏纹理 quad 替代纯色渐变。
    if (skyTexture_.Valid()) {
        overlay.DrawQuad({0.0f, 0.0f}, {1280.0f, 720.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                         skyTexture_, {0.0f, 0.0f}, {1.0f, 1.0f});
        return;
    }
    overlay.DrawFullscreenGradient(skyTop_, skyHorizon_);
}

} // namespace neon::gfx
