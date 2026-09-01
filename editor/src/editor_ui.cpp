#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "imgui_internal.h"

namespace neon::editor {

namespace {
// Layout version persisted as a versioned marker window in the ImGui ini. When
// the ini is missing or predates the current layout version, the editor
// re-applies the Unity-style default docking layout once (the user's later
// customizations are still saved and respected).
// v3: the built-in script editor is docked into the bottom tab group instead
// of floating - its saved floating position (550,148) covered the left half
// of the Inspector (属性) and swallowed every click on component blocks.
constexpr int kNeonLayoutVersion = 3;
bool NeedsDefaultLayout() {
    static const bool needs = [] {
        const char* ini = ImGui::GetIO().IniFilename;
        if (!ini) return true; // no ini yet -> fresh default
        std::ifstream f(ini);
        if (!f) return true; // unreadable -> treat as fresh
    const std::string marker =
        std::string("[Window][##NeonLayoutVer") + std::to_string(kNeonLayoutVersion) + "]";
        std::string line;
        while (std::getline(f, line))
            if (line.find(marker) != std::string::npos) return false; // current layout saved
        return true; // ini exists but predates this layout version
    }();
    return needs;
}

} // namespace

void EditorApp::BuildImGuiUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存场景", "Ctrl+S")) SaveScene();
            if (ImGui::MenuItem("加载场景", "Ctrl+L"))
                LoadScene(std::string(kDefaultProjectDir) + "/" + kSandboxSceneRel);
            if (ImGui::MenuItem("另存为子场景")) SaveSceneAsChild();
            ImGui::Separator();
            if (ImGui::MenuItem("导出场景", "Ctrl+E")) ExportScene();
            ImGui::Separator();
            if (ImGui::MenuItem("退出")) {
                if (Window()) Window()->RequestClose();
            }
            ImGui::EndMenu();
        }
        // 编辑: scene-history undo/redo + selection batch operations (the
        // shortcuts also work while the viewport has focus; the menu adds
        // discoverability + enabled/disabled state).
        if (ImGui::BeginMenu("编辑")) {
            if (ImGui::MenuItem("撤销", "Ctrl+Z", false, history_.CanUndo())) {
                history_.Undo();
                ClampSelection();
            }
            if (ImGui::MenuItem("重做", "Ctrl+Y", false, history_.CanRedo())) {
                history_.Redo();
                ClampSelection();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("删除选中", "Del", false, !selection_.empty())) {
                history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                    &entities_, SelectedIndices()));
                ClampSelection();
            }
            if (ImGui::MenuItem("复制选中", "Ctrl+D", false, !selection_.empty())) {
                history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                    &entities_, SelectedIndices()));
                SetSelection(static_cast<int>(entities_.size()) - 1);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("聚焦选中", "F", false,
                                selected_ >= 0 &&
                                    selected_ < static_cast<int>(entities_.size()))) {
                camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            // Built-in toggleable panels come from the shared registry (C3):
            // one list drives both this menu and the ini persistence, so the
            // two can no longer drift apart.
            for (int i = 0; i < PanelCount(); ++i) {
                const PanelDef& p = Panels()[i];
                if (!p.inMenu) continue;
                ImGui::MenuItem(p.title, nullptr, &(this->*p.flag));
            }
            ImGui::MenuItem("以选中相机为视图", nullptr, &cameraFollowSelected_);
            // Plugin-contributed panels appear in the same menu (docked like
            // built-in panels; the manager owns their open state).
            if (pluginMgr_) {
                for (editor::PluginPanel& p : pluginMgr_->Panels())
                    ImGui::MenuItem(p.title.c_str(), nullptr, &p.opened);
            }
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showImGuiDemo_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("项目")) {
            if (projects_.empty()) ScanProjects();
            ImGui::TextDisabled("打开项目");
            for (size_t i = 0; i < projects_.size(); ++i) {
                const EditorProject& p = projects_[i];
                char label[256];
                std::snprintf(label, sizeof(label), "%s  [%s]###mproj%d", p.name.c_str(),
                              p.mode == "2d" ? "2D" : "3D", static_cast<int>(i));
                if (ImGui::MenuItem(label, nullptr, projectSel_ == static_cast<int>(i)))
                    SwitchProject(p.dir);
            }
            ImGui::Separator();
            ImGui::TextDisabled("当前项目场景");
            for (const std::string& s : projectScenes_) {
                if (ImGui::MenuItem(SceneDisplayName(s).c_str())) LoadProjectScene(s);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("项目目录");
            if (ImGui::InputText("##project_dir", projectDirBuf_, sizeof(projectDirBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                SwitchProject(projectDirBuf_);
            }
            ImGui::TextDisabled("导出场景写入 %s/assets/scenes/exported_scene.json",
                                projectDir_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("重新加载项目")) SwitchProject(projectDir_);
            if (ImGui::MenuItem("导出游戏场景")) ExportScene();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::MenuItem("关于", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // F3 toggles the unified debug-overlay panel (G8-3). Runs regardless of
    // selection so the layers can be switched while playing.
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) showDebugOverlay_ = !showDebugOverlay_;

    // Transform-gizmo shortcuts: W/E/R switch the operation while an entity is
    // selected (ignored while the user is typing text, e.g. the name field).
    if (selected_ >= 0 && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = ImGuizmo::SCALE;
        // P2-editor UX shortcuts: Delete = 删除选中, Ctrl+D = 复制选中,
        // F = 相机聚焦到选中实体.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ClampSelection();
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
            !selection_.empty()) {
            history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                &entities_, SelectedIndices()));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && selected_ < static_cast<int>(entities_.size())) {
            camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
        }
    }

    // Docking layout: full-workspace dock space below the menu bar.
    const float menuH = ImGui::GetFrameHeight();
    const float toolH = 36.0f;
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH + toolH), ImGuiCond_Always);
    // Use Size.y (the full window height), NOT WorkSize.y: BeginMainMenuBar
    // shrinks the main viewport's WorkSize by the menu bar height, so sizing
    // the DockSpace off WorkSize.y would end it ~menuH px above the window
    // bottom and let the full-screen 3D scene leak out below the panels.
    ImGui::SetNextWindowSize(
        ImVec2(mainVp->Size.x, mainVp->Size.y - menuH - toolH),
                             ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGuiWindowFlags dsFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##NeonDockSpace", nullptr, dsFlags);
    ImGui::PopStyleVar(3);
    ImGuiID dockId = ImGui::GetID("NeonDockSpace");
    dockspaceId_ = dockId;
    // NOTE: no ImGuiDockNodeFlags_PassthruCentralNode here. That flag makes the
    // DockSpace root paint an opaque ImGuiCol_WindowBg rectangle over the WHOLE
    // workspace when the central node is non-empty (and the 3D viewport window
    // IS docked into the central node, so the passthru "hole" is never
    // registered) - which would cover the full-screen 3D scene. Without the
    // flag the host window (NoBackground) + the 视口 window (NoBackground) stay
    // transparent, so the scene shows through the central viewport while the
    // opaque tool panels cover the rest.
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    // 全局兜底: 模型资产拖到编辑器任何空白处 (dock 分割条/标签栏/空闲区) 都
    // 打开模型查看器, 保证拖拽一定有反馈。只当鼠标落在其他窗口 (有各自目标)
    // 之外才命中, 不与场景/层级/预览面板的目标冲突。
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
            const char* path = static_cast<const char*>(p->Data);
            NEON_LOG_INFO("DockSpace: model dropped '%s'", path ? path : "");
            if (path && *path) {
                showModelPreview_ = true;
                OpenModelPreview(path);  // ModelPreviewPanel::Open（Task 7 转发器）
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();

    // Persist a layout-version marker in the ini (offscreen, invisible). Its
    // absence means "no saved layout yet" or "saved before the layout changed",
    // which triggers the Unity-style default below exactly once.
    {
        char verName[32];
        std::snprintf(verName, sizeof(verName), "##NeonLayoutVer%d", kNeonLayoutVersion);
        ImGui::SetNextWindowPos(ImVec2(-100000.0f, -100000.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
        ImGui::Begin(verName, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav);
        ImGui::End();
    }

    // First-run default docking layout (applied when there is no saved layout,
    // the saved ini predates this layout version, or the dock space is empty).
    static bool layoutAttempted = false;
    if (!layoutAttempted) {
        layoutAttempted = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (node == nullptr || !node->IsSplitNode() || NeedsDefaultLayout()) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId,
                                          ImVec2(mainVp->WorkSize.x,
                                                 mainVp->WorkSize.y - menuH - toolH));
            // Unity-style layout: Hierarchy (场景) left, Inspector (属性) right,
            // Scene view (视口) center, Project/tools (资产/资源/日志/行为树/脚本/
            // 脚本编辑器/打包/性能) docked across the bottom.
            ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.22f,
                                                        nullptr, &dockId);
            ImGuiID left = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.20f,
                                                       nullptr, &dockId);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.28f,
                                                         nullptr, &dockId);
            ImGui::DockBuilderDockWindow("场景", left);
            ImGui::DockBuilderDockWindow("属性", right);
            ImGui::DockBuilderDockWindow("资产", bottom);
            ImGui::DockBuilderDockWindow("资源", bottom);
            ImGui::DockBuilderDockWindow("日志", bottom);
            ImGui::DockBuilderDockWindow("行为树", bottom);
            ImGui::DockBuilderDockWindow("脚本", bottom);
            ImGui::DockBuilderDockWindow("脚本编辑器", bottom);
            ImGui::DockBuilderDockWindow("打包", bottom);
            ImGui::DockBuilderDockWindow("性能", bottom);
            ImGui::DockBuilderDockWindow("视口", dockId);
            ImGui::DockBuilderFinish(dockId);
        }
    }

    // Toolbar row below the menu bar.
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, toolH), ImGuiCond_Always);
    ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toolbar", nullptr, tbFlags)) {
        // Icon button helper: fixed-size glyph button with an active-state
        // highlight and a hover tooltip (toolbar icon-ization, UX item 6).
        auto ToolbarIcon = [](const char* label, const char* tip, bool active) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.36f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.42f, 0.62f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.47f, 0.68f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.29f, 0.33f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.38f, 0.43f, 1.0f));
            }
            const bool clicked = ImGui::Button(label, ImVec2(26.0f, 0.0f));
            if (active)
                ImGui::PopStyleColor(4);
            else
                ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        // --- 上下文: 项目 / 场景切换 ---
        ImGui::SetNextItemWidth(150.0f);
        const char* projPreview = projectName_.empty()
                                      ? (projectDir_ == "." ? "默认场景" : projectDir_.c_str())
                                      : projectName_.c_str();
        if (ImGui::BeginCombo("##project_picker", projPreview)) {
            if (ImGui::Selectable("默认场景", projectDir_ == ".")) SwitchProject(".");
            ImGui::Separator();
            if (projects_.empty()) ScanProjects();
            for (size_t i = 0; i < projects_.size(); ++i) {
                const EditorProject& p = projects_[i];
                char label[256];
                std::snprintf(label, sizeof(label), "%s  [%s]###proj%d", p.name.c_str(),
                              p.mode == "2d" ? "2D" : "3D", static_cast<int>(i));
                if (ImGui::Selectable(label, projectSel_ == static_cast<int>(i)))
                    SwitchProject(p.dir);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("重新扫描项目")) ScanProjects();
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##scene_picker", currentSceneName_.empty()
                                                    ? "选择场景…"
                                                    : SceneDisplayName(currentSceneName_).c_str())) {
            for (const std::string& s : projectScenes_) {
                if (ImGui::Selectable(SceneDisplayName(s).c_str(),
                                      currentSceneName_ == BaseName(s)))
                    LoadProjectScene(s);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 运行/视图: 播放、热重载、相机预设、2D/3D 切换 ---
        if (ToolbarIcon(playActive_ ? "■" : "▶",
                        playActive_ ? "停止播放 (F5)" : "播放 (F5)", playActive_))
            TogglePlay();
        ImGui::SameLine();
        // Hot reload toggle (T4.8): off by default. When on, script/asset mtime
        // changes restart the play / reload the cached assets (throttled).
        if (ToolbarIcon(hotReload_ ? "●" : "○", "热重载: 脚本/资源改动自动重载", hotReload_))
            hotReload_ = !hotReload_;
        ImGui::SameLine();
        // Multi-camera viewport preset (T4.8): 透视 / 顶视 / 前视 (also Tab).
        const char* camLabels[] = {"透视", "顶视", "前视"};
        int camSel = static_cast<int>(viewCam_);
        ImGui::SetNextItemWidth(88.0f);
        if (ImGui::Combo("##viewport_cam", &camSel, camLabels, 3))
            SetViewCam(static_cast<ViewCam>(camSel));
        ImGui::SameLine();
        const bool in2D = editMode_ == EditMode::Scene2D;
        if (ToolbarIcon(in2D ? "3D" : "2D", in2D ? "切换到 3D 透视" : "切换到 2D 视图",
                        false))
            Set2DMode(!in2D);
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 变换: 移动/旋转/缩放 + 本地/世界坐标 ---
        if (ToolbarIcon("✥", "移动 (W)", gizmoOp_ == ImGuizmo::TRANSLATE))
            gizmoOp_ = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ToolbarIcon("⟳", "旋转 (E)", gizmoOp_ == ImGuizmo::ROTATE))
            gizmoOp_ = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ToolbarIcon("⇲", "缩放 (R)", gizmoOp_ == ImGuizmo::SCALE))
            gizmoOp_ = ImGuizmo::SCALE;
        ImGui::SameLine();
        if (ToolbarIcon("◉", "本地坐标空间", gizmoMode_ == ImGuizmo::LOCAL))
            gizmoMode_ = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ToolbarIcon("◎", "世界坐标空间", gizmoMode_ == ImGuizmo::WORLD))
            gizmoMode_ = ImGuizmo::WORLD;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 视口辅助: 网格、聚焦 ---
        if (ToolbarIcon("▦", "视口网格", showViewportGrid_))
            showViewportGrid_ = !showViewportGrid_;
        ImGui::SameLine();
        if (ToolbarIcon("⌖", "聚焦选中 (F)", false) && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()))
            camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 状态: 选中/实体数量 ---
        ImGui::TextDisabled("选中 %zu / 实体 %zu", selection_.size(), entities_.size());
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // Plugin-contributed toolbar tools.
        if (pluginMgr_) {
            for (editor::PluginTool& t : pluginMgr_->Tools()) {
                ImGui::SameLine();
                if (ImGui::Button(t.label.c_str()) && t.host && t.fn != 0) {
                    const auto res = t.host->CallCaptured(t.fn, {});
                    if (!res.Ok()) {
                        NEON_LOG_ERROR("Editor plugin tool '%s' failed: %s", t.id.c_str(),
                                       t.host->LastError().message.c_str());
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                    ImGui::SetTooltip("插件工具: %s (%s)", t.label.c_str(), t.id.c_str());
            }
        }
    }
    ImGui::End();

    // The DockSpace's Begin/End (above) can overwrite the hover we resolved
    // after NewFrame, so re-resolve it right before the tool panels build.
    // IMPORTANT: never touch HoveredWindow while a popup/menu is open - the
    // 项目/场景 menus and any combo render as popups, and stealing the hover
    // would make their items unclickable.
    {
        ImGuiContext& ictx = *ImGui::GetCurrentContext();
        if (ictx.OpenPopupStack.Size == 0) {
            ImGuiWindow* best = nullptr;
            for (int wi = ictx.Windows.Size - 1; wi >= 0; --wi) {
                ImGuiWindow* w = ictx.Windows[wi];
                if (!w || w->Hidden) continue;
                if (!w->Active) continue; // closed panels must not win hover
                if (w->DockNodeAsHost != nullptr) continue;
                if (w->ParentWindow != nullptr) continue;
                if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue;
                if (std::strcmp(w->Name, "视口") == 0) continue;
                if (std::strncmp(w->Name, "##", 2) == 0) continue;
                if (w->Rect().Contains(ictx.IO.MousePos)) {
                    best = w;
                    break;
                }
            }
            if (best) ictx.HoveredWindow = best;
        }
    }
    panels_.DrawAll(ctx_); // 已迁移的独立面板（…/ScriptEditor/UIEditor）；其余仍走 BuildXxxPanel
    BuildBtPanel();
        BuildAnimEditorPanel();
        BuildStateMachineEditorPanel();
    BuildViewportPanel();
    BuildPluginPanels();
    DrawSceneGizmos();

    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
}

void EditorApp::LoadInputMapEdit() {
    inputMapState_.edit = script::InputMap::Defaults();
    std::ifstream in(projectDir_ + "/input.json", std::ios::binary);
    if (in.is_open()) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        if (!inputMapState_.edit.Load(text, &err))
            NEON_LOG_ERROR("Editor: input.json parse failed: %s", err.c_str());
    }
    inputMapState_.listenAction = "";
}

void EditorApp::SaveInputMapEdit() {
    std::ofstream out(projectDir_ + "/input.json", std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write '%s/input.json'", projectDir_.c_str());
        return;
    }
    out << inputMapState_.edit.ToJson();
    NEON_LOG_INFO("Editor: input.json saved (%zu actions)", inputMapState_.edit.Names().size());
}

// 输入映射面板（原 EditorApp::BuildInputMapPanel）已整体迁移为独立面板类
// editor/src/panels/input_map_panel.hpp/.cpp（InputMapPanel : IPanel，Task 13，
// 沿用 Task 2-12 样板）。InputMapState 提升为共享结构（editor_context.hpp）并
// 仍由 EditorApp 持有（OnEvent 监听按键写回 listenAction），经 ctx.inputMap 指针
// 访问；Load/Save 保留为本类方法（OnCreate 也调 Load），经 ctx 回调访问。
// EditorApp 在 OnCreate 注册它；BuildImGuiUI 经 panels_.DrawAll(ctx_) 分发。

} // namespace neon::editor

