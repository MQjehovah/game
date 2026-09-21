// C1: HudSystem implementation. Migrated verbatim from GameRuntime's HUD
// subsystem (SpawnFloatText / SetEntityPlate / WorldToScreen / ScreenToWorld /
// DesignWidth + the Draw-time view snapshot, float-text aging and screen-anchor
// projection): the projection matrices and HUD collections are now owned here
// instead of by GameRuntime. Pure code movement, no semantic change.
#include "neon/scene/systems/hud_system.hpp"

#include <cmath>
#include <cstdint>
#include <utility>

#include "neon/gfx/renderer.hpp"

namespace neon::scene {
namespace {

// Stable 64-bit key for per-entity HUD scoping: id occupies the high half so an
// id reused across generations still keys uniquely (matches GameRuntime's
// EntityKey layout, so plates_ joins ScreenAnchors by the same key).
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

void HudSystem::CaptureView(const gfx::Camera& cam, float aspect, float vpW, float vpH) {
    lastCam_ = cam;
    lastCamValid_ = true;
    lastAspect_ = aspect;
    lastVpW_ = vpW;
    lastVpH_ = vpH;
    lastViewProj_ = cam.ViewProjection(aspect);
    lastViewProjValid_ = true;
}

void HudSystem::Tick(float dt) {
    // Floating combat texts age out.
    for (auto it = floatTexts_.begin(); it != floatTexts_.end();) {
        it->age += dt;
        if (it->age >= it->life) it = floatTexts_.erase(it);
        else ++it;
    }
}

void HudSystem::SpawnFloatText(const math::Vec3& world, const std::string& text, bool crit,
                               float life, float r, float g, float b) {
    FloatText ft;
    ft.world = world;
    ft.text = text;
    ft.crit = crit;
    ft.life = life > 0.05f ? life : 1.2f;
    ft.r = r;
    ft.g = g;
    ft.b = b;
    floatTexts_.push_back(ft);
    if (floatTexts_.size() > 64) floatTexts_.erase(floatTexts_.begin()); // cap
}

void HudSystem::SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac) {
    EntityPlate p;
    p.name = name;
    p.hpFrac = hpFrac;
    plates_[EntityKey(e)] = p;
}

bool HudSystem::WorldToScreen(const math::Vec3& world, float& outX, float& outY) const {
    if (!lastViewProjValid_) return false;
    math::Vec4 clip = lastViewProj_.TransformVec4(math::Vec4(world.x, world.y, world.z, 1.0f));
    if (clip.w <= 0.01f) return false;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    // Viewport PIXELS (top-left origin) - the same space the UI and the
    // script 2D canvas draw in.
    outX = (nx * 0.5f + 0.5f) * lastVpW_;
    outY = (0.5f - ny * 0.5f) * lastVpH_;
    return true;
}

bool HudSystem::ScreenToWorld(const math::Vec2& screen, float& outX, float& outY) const {
    // Inverse of WorldToScreen for the axis-aligned ortho camera shape the 2D
    // games use (camera on +Z looking -Z, up +Y, no roll). Input: pixels.
    if (!lastCamValid_ || !lastCam_.ortho) return false;
    const float nx = (screen.x / lastVpW_) * 2.0f - 1.0f;
    const float ny = 1.0f - (screen.y / lastVpH_) * 2.0f;
    const float halfH = lastCam_.orthoSize;
    const float halfW = halfH * lastAspect_;
    outX = lastCam_.target.x + nx * halfW;
    outY = lastCam_.target.y + ny * halfH;
    return true;
}

float HudSystem::DesignWidth() const {
    return lastVpW_;
}

float HudSystem::DesignHeight() const {
    return lastVpH_;
}

bool HudSystem::GroundPick(const math::Vec2& screen, math::Vec3& out) const {
    if (!lastViewProjValid_ || !lastCamValid_) return false;
    if (lastCam_.ortho) {
        float wx = 0.0f, wy = 0.0f;
        if (!ScreenToWorld(screen, wx, wy)) return false;
        out = {wx, wy, 0.0f};
        return true;
    }
    // Screen pixel -> normalized device coords (top-left origin, y down).
    const float vpW = lastVpW_ > 0.5f ? lastVpW_ : 1280.0f;
    const float vpH = lastVpH_ > 0.5f ? lastVpH_ : 720.0f;
    const float nx = (screen.x / vpW) * 2.0f - 1.0f;
    const float ny = 1.0f - (screen.y / vpH) * 2.0f;
    // Build the view ray from the captured camera basis (independent of near/far
    // plane, so a wide pitch or a shallow FOV both work).
    math::Vec3 f = lastCam_.target - lastCam_.position;
    const float fl = f.Length();
    if (fl < 1e-5f) return false;
    f = f * (1.0f / fl);
    math::Vec3 up = lastCam_.up;
    math::Vec3 s = math::Cross(f, up);
    const float sl = s.Length();
    if (sl < 1e-5f) return false;
    s = s * (1.0f / sl);
    const math::Vec3 u = math::Cross(s, f);
    const float aspect = lastAspect_ > 0.01f ? lastAspect_ : (vpW / vpH);
    const float tanY = std::tan(lastCam_.fovY * 0.5f);
    const float tanX = tanY * aspect;
    math::Vec3 dir = f + s * (nx * tanX) + u * (ny * tanY);
    const float dl = dir.Length();
    if (dl < 1e-5f) return false;
    dir = dir * (1.0f / dl);
    if (std::fabs(dir.y) < 1e-6f) return false;   // parallel to the ground
    const float t = -lastCam_.position.y / dir.y; // intersect the y = 0 plane
    if (t <= 0.0f) return false;                  // ground is behind the camera
    out = {lastCam_.position.x + dir.x * t, 0.0f, lastCam_.position.z + dir.z * t};
    return true;
}

// M1 HUD anchors: project each entity's world position into design units for
// on_render scripts. The caller (GameRuntime::Draw) computes the world
// positions (including per-plate head offsets); this projects them with the
// same view-projection WorldToScreen uses.
void HudSystem::UpdateAnchors(
    const std::vector<std::pair<ecs::Entity, math::Vec3>>& entities) {
    screenAnchors_.clear();
    for (const auto& e : entities) {
        const math::Vec3& wp = e.second;
        math::Vec4 clip = lastViewProj_.TransformVec4(math::Vec4(wp.x, wp.y, wp.z, 1.0f));
        ScreenAnchor a;
        a.entity = EntityKey(e.first);
        a.world = wp;
        if (clip.w > 0.01f) {
            const float nx = clip.x / clip.w, ny = clip.y / clip.w;
            // Design-space (1280x720) coordinates: the same mapping the
            // renderer's 2D overlay (and on_render) draws with.
            a.x = (nx * 0.5f + 0.5f) * gfx::Renderer::kDesignWidth;
            a.y = (0.5f - ny * 0.5f) * gfx::Renderer::kDesignHeight;
            a.onscreen = nx >= -1.2f && nx <= 1.2f && ny >= -1.2f && ny <= 1.2f;
        }
        screenAnchors_.push_back(a);
    }
}

} // namespace neon::scene
