#include "neon/gfx/scene_state.hpp"

#include <algorithm>
#include <chrono>

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
    if (!iblBrdfLutReady_) {
        const std::vector<uint8_t> lut = ibl::BuildBrdfLut();
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

    // Sky-dependent maps: irradiance (diffuse) + prefiltered specular. Both are
    // RGBA8: the sky gradient is an LDR environment (all texels <= 1) so the
    // 8-bit upload loses nothing; a future HDR environment would need a
    // float-texture path in the backend.
    const std::vector<uint8_t> irr = ibl::BuildIrradianceMap(top, horizon, kIblGradientPower);
    const std::vector<uint8_t> pf = ibl::BuildPrefilteredMap(top, horizon, kIblGradientPower);
    if (iblIrradianceTex_.Valid()) backend_->DestroyTexture(iblIrradianceTex_);
    if (iblPrefilteredTex_.Valid()) backend_->DestroyTexture(iblPrefilteredTex_);
    TextureDesc irrDesc;
    irrDesc.width = 1;
    irrDesc.height = ibl::kEnvRows;
    irrDesc.rgba = irr.data();
    irrDesc.filter = Filter::Linear;
    iblIrradianceTex_ = backend_->CreateTexture(irrDesc);
    TextureDesc pfDesc;
    pfDesc.width = ibl::kRoughnessCols;
    pfDesc.height = ibl::kEnvRows;
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
