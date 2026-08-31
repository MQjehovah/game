#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "neon/ecs/world.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {

// HUD 覆盖层：世界↔屏幕投影 + 飘字 + 头顶板 + 屏幕锚点。
class HudSystem {
public:
    struct FloatText { math::Vec3 world; std::string text; bool crit = false; float life = 1.0f, age = 0.0f; };
    struct ScreenAnchor { uint64_t entity = 0; float x = 0, y = 0; bool onscreen = false; math::Vec3 world; };
    struct EntityPlate { std::string name; float hpFrac = -1.0f; };

    // Draw 每帧调用：记录相机/视口投影快照（WorldToScreen/ScreenToWorld 用）。
    void CaptureView(const gfx::Camera& cam, float aspect, float vpW, float vpH);
    void Tick(float dt); // 推进飘字 age
    void SpawnFloatText(const math::Vec3& w, const std::string& t, bool crit, float life);
    void SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac);
    bool WorldToScreen(const math::Vec3& w, float& x, float& y) const;
    bool ScreenToWorld(const math::Vec2& s, float& x, float& y) const;
    float DesignWidth() const;
    float DesignHeight() const;
    // 每帧由 Draw 更新：把 world 投影成屏幕锚点（供脚本 ScreenAnchors() 读）。
    void UpdateAnchors(const std::vector<std::pair<ecs::Entity, math::Vec3>>& entities);
    const std::vector<FloatText>& FloatTexts() const { return floatTexts_; }
    const std::vector<ScreenAnchor>& ScreenAnchors() const { return screenAnchors_; }
    const std::map<uint64_t, EntityPlate>& EntityPlates() const { return plates_; }

private:
    std::vector<FloatText> floatTexts_;
    std::vector<ScreenAnchor> screenAnchors_;
    std::map<uint64_t, EntityPlate> plates_;
    math::Mat4 lastViewProj_;
    bool lastViewProjValid_ = false;
    gfx::Camera lastCam_;
    bool lastCamValid_ = false;
    float lastAspect_ = 16.0f / 9.0f;
    float lastVpW_ = 1280.0f;
    float lastVpH_ = 720.0f;
};

} // namespace neon::scene
