// 日志面板（原 EditorApp::BuildLogPanel）已整体迁移为独立面板类
// editor/src/panels/log_panel.hpp/.cpp（LogPanel : IPanel，Task 5，沿用
// Task 2-4 迁移样板）。EditorApp 在 OnCreate 注册它；editor_ui.cpp 的
// BuildImGuiUI 经 panels_.DrawAll(ctx_) 分发。此占位注释保留迁移痕迹，
// 待性能/视口等其余面板迁移完成后整体删除本 inc。

// 性能面板（原 EditorApp::BuildProfilerPanel）已整体迁移为独立面板类
// editor/src/panels/profiler_panel.hpp/.cpp（ProfilerPanel : IPanel，Task 12，
// 沿用 Task 2-11 样板）。帧时间环形缓冲 ProfilerState 提升为共享结构（editor_
// context.hpp）并仍由 EditorApp 持有（冒烟测试直接读 profiler_.ms），经
// EditorContext 指针访问。EditorApp 在 OnCreate 注册它；editor_ui.cpp 的
// BuildImGuiUI 经 panels_.DrawAll(ctx_) 分发。

void EditorApp::BuildViewportPanel() {
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
        if (!viewportDockFallbackDone_) {
            viewportDockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (dockspaceId_ && !win->DockId && !win->DockIsActive) {
                if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId_)) {
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
        math::Vec2 uiPos = renderer_.ScreenToUI({contentRect.x, contentRect.y});
        float scale = renderer_.UIScale();
        viewportRect_ = {uiPos.x, uiPos.y, contentRect.w / scale, contentRect.h / scale};
        viewportScreenRect_ = contentRect;

        // Play: the game's own view fills the viewport; the editor hint rows
        // (camera help + scene stats) would draw on top of it - hide both.
        if (!playActive_) {
            ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                               "右键旋转 | 中键平移 | 滚轮缩放 | 左键拾取");
            const char* camLabel = viewCam_ == ViewCam::Top
                                   ? "顶视 (正交)"
                                   : viewCam_ == ViewCam::Front ? "前视 (正交)" : "透视";
            std::string physInfo;
            if (play_ && play_->PhysicsBodyCount() > 0)
                physInfo = " | 物理 " + std::to_string(play_->PhysicsBodyCount());
            ImGui::TextDisabled("%s | 实体 %zu%s | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f", camLabel,
                                entities_.size(), physInfo.c_str(), camTarget_.x, camTarget_.y,
                                camTarget_.z, camDist_);
        }
        // Transform gizmo for the selected entity (drawn into this window's
        // draw list; interacts via ImGui's mouse state).
        DrawTransformGizmo();
        // 模型拖到 3D 视口 → 打开模型查看器 (资产面板拖拽源的 ASSET_MODEL)。
        // 注意: 层级面板已用 ASSET_MODEL 添加实体, 这里只做预览兜底, 避免
        // 用户拖到视口没反应的困惑。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Viewport: model dropped '%s'", path ? path : "");
                if (path && *path) {
                    showModelPreview_ = true;
                    OpenModelPreview(path);  // ModelPreviewPanel::Open (Task 7 转发器)
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
                    if (!nm.empty()) AddEntity("prefab:" + nm);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

// 导航工具（原 EditorApp::BuildNavPanel）已整体迁移为独立面板类
// editor/src/panels/nav_panel.hpp/.cpp（NavPanel : IPanel，Task 9，沿用 Task 2-8
// 样板）。导航状态 NavState 提升为 neon::editor 共享结构（editor_context.hpp），
// EditorApp 持有（nav_），经 EditorContext::nav 指针与调试覆盖层共用。
// EditorApp 在 OnCreate 注册它；editor_ui.cpp 的 BuildImGuiUI 经
// panels_.DrawAll(ctx_) 分发。此占位注释保留迁移痕迹，待其余面板迁移完成后
// 整体删除本 inc。

// Data-driven UI editor (Godot/Unity-style): browse ui/*.ui.json documents,
// edit the node tree + node properties, and preview the result in the main
// viewport (1:1 design pixels). Saved JSON is consumed at runtime by the
// UIShow/UIClicked Lua bindings.
