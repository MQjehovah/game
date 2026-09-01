#include "panels/debug_overlay_panel.hpp"

// 调试覆盖层实现 = 原 EditorApp::BuildDebugOverlayPanel / DrawDebugOverlay
// （editor_plugins.cpp:125-228）方法体逐行迁移：EditorApp 成员改 ctx 指针 /
// 本类成员。行为零变化。SceneEntity 完整类型经 editor.hpp（与 ScenePanel 同模式）。

#include <algorithm>

#include "editor.hpp"
#include "imgui.h"
#include "neon/core/json.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::editor {

void DebugOverlayPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("调试覆盖层", visible_)) {
        ImGui::TextDisabled("F3 开关本面板; 图层实时作用在视口");
        ImGui::Checkbox("碰撞线框", ctx.debugColliders);
        ImGui::Checkbox("导航可行走区域", ctx.debugNavMesh);
        ImGui::Checkbox("光照探针", ctx.debugProbes);
        ImGui::Checkbox("音频源", ctx.debugAudio);
        ImGui::TextDisabled("提示: 碰撞/导航/音频在编辑与播放视口即时生效");
    }
    ImGui::End();
}

void DebugOverlayPanel::DrawOverlay(EditorContext& ctx, const gfx::Camera& cam) {
    (void)cam;
    if (!ctx.renderer->Backend()) return;

    // G8-3 audio sources: translucent blue attenuation spheres at every entity
    // carrying an "audio" component. Radius = the component's attenuation
    // distance, so designers see how far a source is audible.
    if (ctx.debugAudio && *ctx.debugAudio) {
        for (const SceneEntity& e : *ctx.entities) {
            auto it = e.extraComponents.find("audio");
            if (it == e.extraComponents.end() || !it->second.IsObject()) continue;
            float radius = 10.0f;
            if (const core::Json* r = it->second.Get("radius")) {
                if (r->IsNumber()) radius = static_cast<float>(r->GetNumber());
            }
            ctx.renderer->DrawSphere(e.pos, radius,
                                     gfx::Color{0.25f, 0.5f, 1.0f, 0.18f});
        }
    }

    // Navigation walkable area: translucent green (walkable) / red (blocked)
    // ground cells so the field is visible in the viewport, not just the panel.
    if (ctx.debugNavMesh && *ctx.debugNavMesh && ctx.nav && ctx.nav->grid.Valid()) {
        static gfx::Mesh cell = gfx::Mesh::CreatePlane(*ctx.renderer, 1.0f, 1.0f, 1, 1, "navcell");
        if (!cell.Valid()) return;
        gfx::Material walk = gfx::Material::Unlit({}, gfx::Color{0.3f, 0.85f, 0.4f, 0.28f});
        walk.transparent = true;
        gfx::Material block = gfx::Material::Unlit({}, gfx::Color{0.9f, 0.3f, 0.3f, 0.28f});
        block.transparent = true;
        for (int y = 0; y < ctx.nav->grid.Height(); ++y) {
            for (int x = 0; x < ctx.nav->grid.Width(); ++x) {
                const math::Vec2 c = ctx.nav->grid.CellToWorld(x, y);
                const math::Mat4 m = math::Mat4::Translation({c.x, 0.02f, c.y}) *
                                     math::Mat4::Scale({ctx.nav->grid.CellSize(), 1.0f,
                                                        ctx.nav->grid.CellSize()});
                ctx.renderer->DrawMesh(cell, ctx.nav->grid.Walkable(x, y) ? walk : block, m);
            }
        }
    }

    // Light probes: wireframe sphere markers tinted by probe irradiance, over a
    // field rebuilt lazily from the scene's 3D meshes' combined AABB. A single
    // representative light near the scene centre makes the 3D gradient visible;
    // real scene point lights would feed this list (G2-4).
    if (ctx.debugProbes && *ctx.debugProbes) {
        math::AABB bounds;
        bool haveBounds = false;
        for (const SceneEntity& e : *ctx.entities) {
            if (!e.mesh.Valid()) continue;
            const math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                     math::Mat4::Scale(e.scale);
            const math::AABB wb = math::TransformAABB(e.mesh.Bounds(), model);
            if (!haveBounds) {
                bounds = wb;
                haveBounds = true;
            } else {
                bounds.min.x = std::min(bounds.min.x, wb.min.x);
                bounds.min.y = std::min(bounds.min.y, wb.min.y);
                bounds.min.z = std::min(bounds.min.z, wb.min.z);
                bounds.max.x = std::max(bounds.max.x, wb.max.x);
                bounds.max.y = std::max(bounds.max.y, wb.max.y);
                bounds.max.z = std::max(bounds.max.z, wb.max.z);
            }
        }
        if (!haveBounds) {
            bounds = math::AABB{{-15, 0, -15}, {15, 5, 15}};
        }
        const bool same =
            probeBounds_.min.x == bounds.min.x && probeBounds_.max.x == bounds.max.x &&
            probeBounds_.min.y == bounds.min.y && probeBounds_.max.y == bounds.max.y &&
            probeBounds_.min.z == bounds.min.z && probeBounds_.max.z == bounds.max.z;
        if (probeDirty_ || !same) {
            gfx::ProbeLightInput in;
            in.pointLights.push_back({(bounds.min + bounds.max) * 0.5f,
                                      gfx::Color{1.0f, 0.9f, 0.75f, 1.0f}, 6.0f,
                                      std::max(bounds.max.x - bounds.min.x, 8.0f)});
            gfx::BuildProbeField(bounds, probeRes_, in, probeField_);
            probeBounds_ = bounds;
            probeDirty_ = false;
        }
        const float markerR =
            std::max(0.08f, (bounds.max.x - bounds.min.x) / (2.0f * static_cast<float>(probeRes_)));
        for (const gfx::IrradianceProbe& p : probeField_) {
            const float y = math::Clamp(p.irradiance.y, 0.0f, 1.0f);
            ctx.renderer->DrawSphere(p.pos, markerR,
                                     gfx::Color{math::Clamp(p.irradiance.x, 0.0f, 1.0f),
                                                y,
                                                math::Clamp(p.irradiance.z, 0.0f, 1.0f), 0.9f});
        }
    }
}

} // namespace neon::editor
