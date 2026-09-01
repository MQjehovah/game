#include "panels/viewport_panel.hpp"

// 视口面板实现 = 原 EditorApp::BuildViewportPanel（panels_debug.inc）方法体
// 逐行迁移：EditorApp 成员（viewportDockFallbackDone_/dockspaceId_/renderer_/
// viewportRect_/viewportScreenRect_/playActive_/play_/viewCam_/camTarget_/
// camDist_/entities_/showModelPreview_ + DrawTransformGizmo/OpenModelPreview/
// AddEntity）改本类 dockFallbackDone_ / ctx 指针 / ctx 回调。行为零变化。
// 需 imgui_internal.h（GetCurrentWindow/DockBuilder）。

#include <string>

#include "editor.hpp"
#include "imgui.h"
#include "imgui_internal.h"

namespace neon::editor {
namespace {

// 本地 basename 帮助函数（ScenePanel/AssetPanel 同款 TU 本地定义）。
std::string FileName(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

} // namespace

bool ViewportPanel::kAlwaysVisible = true;

void ViewportPanel::Draw(EditorContext& ctx) {
    ImGuiWindowFlags vpFlags = ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoBackground;
    // NOTE: deliberately NOT ImGuiWindowFlags_NoInputs. That flag disables
    // manual moving/resizing (imgui.cpp:7938), so the viewport could never be
    // undocked or re-docked elsewhere. The 3D camera reads the platform input
    // directly; ImGui just sees a normal (backgroundless) docked panel.
    if (ImGui::Begin("视口", nullptr, vpFlags)) {
        // The viewport is a normal dockable panel defaulting to the central
        // node. The user's saved layout (with a DockId) is always restored as
        //-is; only when the DockId was LOST (a previous session left it
        // floating) do we fall back to docking it in the center, on its first
        // frame - so an intentional mid-session undock is never yanked back.
        if (!dockFallbackDone_) {
            dockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (*ctx.dockspaceId && !win->DockId && !win->DockIsActive) {
                if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(*ctx.dockspaceId)) {
                    ImGui::DockBuilderDockWindow("视口", central->ID);
                    NEON_LOG_INFO("Editor: viewport DockId was lost; re-docked to the central node");
                }
            }
        }
        ImVec2 pos = ImGui::GetWindowPos();
        // GetWindowPos returns the OUTER frame (the dock tab bar included).
        // The design space must map into the VISIBLE content area instead, or
        // the game HUD's top rows end up hidden behind the tab bar.
        ImVec2 contentMin = ImGui::GetCursorScreenPos();
        ImVec2 contentMaxLocal = ImGui::GetContentRegionMax();
        const math::Rect2 contentRect{
            contentMin.x, contentMin.y,
            (pos.x + contentMaxLocal.x) - contentMin.x,
            (pos.y + contentMaxLocal.y) - contentMin.y};
        math::Vec2 uiPos = ctx.renderer->ScreenToUI({contentRect.x, contentRect.y});
        float scale = ctx.renderer->UIScale();
        *ctx.viewportRect = {uiPos.x, uiPos.y, contentRect.w / scale, contentRect.h / scale};
        *ctx.viewportScreenRect = contentRect;

        // Play: the game's own view fills the viewport; the editor hint rows
        // (camera help + scene stats) would draw on top of it - hide both.
        if (!(ctx.playActive && *ctx.playActive)) {
            ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                               "右键旋转 | 中键平移 | 滚轮缩放 | 左键拾取");
            const int vc = ctx.viewCam ? *ctx.viewCam : 0;
            const char* camLabel = vc == 1 ? "顶视 (正交)"
                                   : vc == 2 ? "前视 (正交)" : "透视";
            std::string physInfo;
            if (ctx.playBodyCount && ctx.playBodyCount() > 0)
                physInfo = " | 物理 " + std::to_string(ctx.playBodyCount());
            ImGui::TextDisabled("%s | 实体 %zu%s | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f", camLabel,
                                ctx.entities->size(), physInfo.c_str(),
                                ctx.camTarget ? ctx.camTarget->x : 0.0f,
                                ctx.camTarget ? ctx.camTarget->y : 0.0f,
                                ctx.camTarget ? ctx.camTarget->z : 0.0f,
                                ctx.camDist ? *ctx.camDist : 0.0f);
        }
        // Transform gizmo for the selected entity (drawn into this window's
        // draw list; interacts via ImGui's mouse state).
        ctx.drawTransformGizmo();
        // 模型拖到 3D 视口 → 打开模型查看器 (资产面板拖拽源的 ASSET_MODEL)。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Viewport: model dropped '%s'", path ? path : "");
                if (path && *path) {
                    if (ctx.showModelPreview) *ctx.showModelPreview = true;
                    ctx.openModelPreview(path);
                }
            }
            // 资产面板的预置体 (assets/prefabs/*.json) 拖到 3D 视口 → 生成实例。
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Viewport: prefab dropped '%s'", path ? path : "");
                if (path && *path) {
                    std::string nm = FileName(path);
                    const size_t dot = nm.find_last_of('.');
                    if (dot != std::string::npos) nm = nm.substr(0, dot);
                    if (!nm.empty()) ctx.addEntity("prefab:" + nm);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

} // namespace neon::editor
