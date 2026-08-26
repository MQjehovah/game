#include "editor.hpp"
#include "editor_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <windows.h>
#undef DrawText // windows.h maps DrawText -> DrawTextA; keep the renderer API
#else
#include <sys/stat.h>
#include <utime.h>
#endif

#include "editor_history.hpp"
#include "font_data.hpp"

#include "imgui_internal.h"
#include "neon/core/json.hpp"
#include "neon/gfx/imgui_neon.hpp"
#include "neon/gfx/scene_props.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace neon::editor {

namespace {

// ---------------------------------------------------------------------------
// Panel open/closed state persistence (neon_editor_imgui.ini).
// The ImGui ini saves each panel's dock node/size but NOT whether the panel is
// open, so a panel the user opened and docked vanished on the next launch (its
// show flag reset to the default while the layout data survived). A custom
// settings handler persists every panel's open state into the same ini: the
// next launch restores both the layout and which panels were visible.
// ---------------------------------------------------------------------------
struct PanelStateEntry {
    const char* title;        // ImGui window title / 视图 menu label
    bool EditorApp::*flag;    // member pointer to the panel's show flag
};

EditorApp* g_panelStateApp = nullptr;   // app owning the flags (set on register)
const PanelStateEntry* g_panelStateEntries = nullptr;
size_t g_panelStateCount = 0;

// Called when the loader enters "[NeonPanels][Panels]"; returning non-null
// marks the section as present so ReadLine applies the saved states.
void* NeonPanelsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) {
    return g_panelStateApp;
}

void NeonPanelsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry,
                        const char* line) {
    EditorApp* app = static_cast<EditorApp*>(entry);
    if (!app || !line) return;
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
        --len;
    if (len == 0 || line[0] == '#') return; // blank / comment
    const char* eq = static_cast<const char*>(std::memchr(line, '=', len));
    if (!eq) return;
    const bool open = std::atoi(eq + 1) != 0;
    const std::string title(line, static_cast<size_t>(eq - line));
    for (size_t i = 0; i < g_panelStateCount; ++i) {
        if (title == g_panelStateEntries[i].title) {
            app->*g_panelStateEntries[i].flag = open;
            return;
        }
    }
    // Plugin-contributed panels are matched by title as well (their open state
    // is owned by the plugin manager, not a member flag).
    if (editor::EditorPluginManager* mgr = app->PluginManager()) {
        for (editor::PluginPanel& p : mgr->Panels()) {
            if (title == p.title) {
                p.opened = open;
                return;
            }
        }
    }
}

void NeonPanelsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                        ImGuiTextBuffer* buf) {
    EditorApp* app = g_panelStateApp;
    if (!app || !buf) return;
    buf->appendf("[%s][Panels]\n", handler->TypeName);
    for (size_t i = 0; i < g_panelStateCount; ++i)
        buf->appendf("%s=%d\n", g_panelStateEntries[i].title,
                     app->*g_panelStateEntries[i].flag ? 1 : 0);
    if (editor::EditorPluginManager* mgr = app->PluginManager()) {
        for (const editor::PluginPanel& p : mgr->Panels())
            buf->appendf("%s=%d\n", p.title.c_str(), p.opened ? 1 : 0);
    }
}

} // namespace

// Single source of truth for the 3D scene framing. Everything that converts
// between screen pixels, the scene rect and the camera (picking, gizmo,
// world->screen overlays) routes through this instead of re-reading the dock
// with its own "rect not laid out yet" fallback.
math::Rect2 EditorApp::ValidSceneRect() const {
    const math::Rect2& vp = SceneRect();
    if (vp.w > 0.0f && vp.h > 0.0f) return vp;
    return {0.0f, 0.0f, static_cast<float>(renderer_.ScreenWidth()),
            static_cast<float>(renderer_.ScreenHeight())};
}

math::Ray EditorApp::PickRay() {
    const math::Rect2 vp = ValidSceneRect();
    const math::Vec2 mousePx = Input() ? Input()->MousePos() : math::Vec2{0.0f, 0.0f};
    const float ndcX = (mousePx.x - vp.x) / vp.w * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mousePx.y - vp.y) / vp.h * 2.0f;
    return RayFromNDC(ActiveCamera(), ViewportAspect(), ndcX, ndcY);
}


// Friend of EditorApp: builds the title -> show-flag table (naming the private
// members requires friendship) and registers the ini settings handler above.
void RegisterPanelStateHandler(EditorApp* app) {
    static const PanelStateEntry kPanels[] = {
        {"场景", &EditorApp::showHierarchy_},
        {"属性", &EditorApp::showInspector_},
        {"资产", &EditorApp::showAssets_},
        {"资源", &EditorApp::showResources_},
        {"日志", &EditorApp::showLog_},
        {"模型查看器", &EditorApp::showModelPreview_},
        {"行为树", &EditorApp::showBt_},
        {"脚本", &EditorApp::showScripts_},
        {"脚本编辑器", &EditorApp::showScriptEditor_},
        {"打包", &EditorApp::showPackage_},
        {"性能", &EditorApp::showProfiler_},
        {"导航", &EditorApp::showNav_},
        {"调试覆盖层", &EditorApp::showDebugOverlay_},
        {"动画时间线", &EditorApp::showAnimEditor_},
        {"地形编辑", &EditorApp::showTerrain_},
        {"2D 地图", &EditorApp::showTilemap_},
        {"本地化", &EditorApp::showLoc_},
        {"UI 编辑器", &EditorApp::showUIEditor_},
        {"输入映射", &EditorApp::showInputMap_},
        {"插件", &EditorApp::showPlugins_},
    };
    g_panelStateApp = app;
    g_panelStateEntries = kPanels;
    g_panelStateCount = sizeof(kPanels) / sizeof(kPanels[0]);

    ImGuiSettingsHandler panelHandler;
    panelHandler.TypeName = "NeonPanels";
    panelHandler.TypeHash = ImHashStr("NeonPanels");
    panelHandler.ReadOpenFn = &NeonPanelsReadOpen;
    panelHandler.ReadLineFn = &NeonPanelsReadLine;
    panelHandler.WriteAllFn = &NeonPanelsWriteAll;
    ImGui::AddSettingsHandler(&panelHandler);
}

bool EditorApp::OnCreate() {
    if (disableShadows_) renderer_.SetShadowsEnabled(false);
    renderer_.SetBackendName(backendName_);
    renderer_.SetBloomEnabled(bloomEnabled_);
    renderer_.SetMsaaEnabled(msaaEnabled_);
    renderer_.SetTonemapEnabled(tonemapEnabled_);
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Editor: renderer init failed");
        return false;
    }
    assetMgr_.Init(&renderer_);

    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
        // System CJK font with DYNAMIC glyphs: scene names, panels and
    // play HUD render any Chinese text without maintaining a list.
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24);
    if (!gfx::ImGuiNeon_Init(&renderer_, gfx::ImGuiNeon_SystemCJKPath())) {
        NEON_LOG_ERROR("Editor: Dear ImGui init failed");
        return false;
    }
    ApplyEditorTheme();
    // Panel open/close persistence: save which panels are visible into the
    // same ImGui ini that stores the docking layout (see NeonPanels* above).
    RegisterPanelStateHandler(this);
    // Toolbar icon glyph self-check: a missing glyph renders as '?' in the
    // toolbar. Log once at startup so icon regressions are caught immediately.
    {
        ImFontBaked* baked = nullptr;
        if (ImGui::GetIO().Fonts && !ImGui::GetIO().Fonts->Fonts.empty())
            baked = ImGui::GetIO().Fonts->Fonts[0]->GetFontBaked(18.0f);
        if (baked) {
            static const unsigned int kToolbarIcons[] = {
                0x2725, 0x27F3, 0x21F2, 0x25C9, 0x25CE, 0x25A6, 0x2316,
                0x25B6, 0x25A0, 0x25CF, 0x25CB, 0x2715, 0x2B06};
            std::string missing;
            for (unsigned int cp : kToolbarIcons) {
                if (!baked->IsGlyphLoaded(static_cast<ImWchar>(cp))) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "U+%04X ", cp);
                    missing += buf;
                }
            }
            if (missing.empty()) {
                NEON_LOG_INFO("Editor: toolbar icon glyphs all present in the font atlas");
            } else {
                NEON_LOG_WARN("Editor: toolbar glyphs missing from the font atlas: %s",
                              missing.c_str());
            }
        }
    }
    audioBackend_ = neon::audio::CreatePlatformAudioBackend();
    if (audioBackend_ && !audioBackend_->Init()) {
        audioBackend_->Shutdown();
        audioBackend_.reset();
        NEON_LOG_WARN("Editor: audio unavailable, play runs silent");
    }

    SetupScene();
    InitToolPanels();

    LoadEditorConfig();
    // Editor plugins: extend the editor itself (panels/tools/asset sources/
    // component schemas) from <projectDir>/plugins (type "editor").
    pluginMgr_ = std::make_unique<editor::EditorPluginManager>();
    pluginMgr_->Init(this);
    pluginMgr_->Load(projectDir_);
    NEON_LOG_INFO("NeonEditor ready (%zu entities), project dir '%s'", entities_.size(),
                  projectDir_.c_str());
    // The smoke test is the canonical 3D-editor flow: --2d/--2d-play/--project
    // only matter for interactive sessions. Normalize before any render so the
    // gizmo/camera assertions see the default scene from frame 0.
    if (smokeMode_) {
        editMode_ = EditMode::Scene3D;
        pvzPlayOnStart_ = false;
        loadProjectOnStart_ = false;
    }
    // Godot-style: restore the last-opened project from the editor config so
    // the editor reopens where the user left off. Skipped for --project
    // (explicit path wins) and smoke runs (the smoke needs the deterministic
    // default sandbox scene). The 2D/3D button is only a camera change, so it
    // never blocks restoring the user's project.
    if (!smokeMode_ && projectDirOnStart_.empty() && projectDir_ != ".") {
        std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
        if (in.is_open()) SwitchProject(projectDir_);
    }
    // --2d / --2d-play without a 2D project open defaults to the bundled PvZ
    // project so the demo canvas + play have plant/zombie content. The
    // toolbar view switcher never changes the project - 2D is just the camera.
    if (editMode_ == EditMode::Scene2D && projectMode_ != "2d" &&
        std::ifstream("projects/pvz/game.json").is_open()) {
        SwitchProject("projects/pvz");
    }
    if (!projectDirOnStart_.empty()) {
        projectDir_ = projectDirOnStart_;
        std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
        projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
        if (loadProjectOnStart_) LoadProjectScene();
    }
    // Start the play LAST: LoadProjectScene/SwitchProject above stop any
    // running play, so --2d-play + --project must start after both.
    if (pvzPlayOnStart_ || playOnStart_) StartPlay();
    // --ui-editor: open the panel and load the first ui/*.ui.json directly.
    // The panel's own auto-open only runs while its dock tab is visible, which
    // a headless/CI layout cannot guarantee.
    if (uiEditorOnStart_) {
        showUIEditor_ = true;
        if (!uiDocOpen_) {
            std::vector<AssetEntry> entries;
            if (ListDirectory(projectDir_ + "/ui", entries)) {
                std::sort(entries.begin(), entries.end(),
                          [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; });
                for (const AssetEntry& f : entries) {
                    if (f.isDir || f.name.size() < 9 ||
                        f.name.compare(f.name.size() - 8, 8, ".ui.json") != 0)
                        continue;
                    if (uiDoc_.Load(f.path)) {
                        uiDocPath_ = f.path;
                        uiDocOpen_ = true;
                        uiSelection_.clear();
                        UISelectNode(uiDoc_.Find("Start") ? uiDoc_.Find("Start") : &uiDoc_.root);
                        uiDirty_ = false;
                        NEON_LOG_INFO("UI: opened '%s'", f.path.c_str());
                    }
                    break;
                }
            }
        }
    }
    if (!previewOnStart_.empty()) {
        showModelPreview_ = true;
        OpenModelPreview(previewOnStart_);
    }
    LoadInputMapEdit(); // Godot-style input panel data
    return true;
}

// Godot-inspired dark editor theme (UX item 6). Applied once right after the
// ImGui context exists; the palette + compact metrics make docked panels read
// as one workspace instead of default-gray windows, and keep the toolbar and
// inspector dense enough that content, not chrome, fills the screen.
void EditorApp::ApplyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Compact metrics.
    s.WindowPadding = ImVec2(6.0f, 6.0f);
    s.FramePadding = ImVec2(5.0f, 3.0f);
    s.ItemSpacing = ImVec2(6.0f, 4.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    s.IndentSpacing = 16.0f;
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowRounding = 0.0f; // docked panels: flush edges
    s.ChildRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 3.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    // Palette.
    const ImVec4 kAccent(0.35f, 0.65f, 1.00f, 1.0f);
    const ImVec4 kWindowBg(0.125f, 0.133f, 0.157f, 1.0f);
    const ImVec4 kChildBg(0.102f, 0.110f, 0.133f, 1.0f);
    const ImVec4 kPopupBg(0.165f, 0.176f, 0.200f, 1.0f);
    const ImVec4 kFrameBg(0.176f, 0.188f, 0.216f, 1.0f);
    const ImVec4 kFrameHover(0.235f, 0.255f, 0.294f, 1.0f);
    const ImVec4 kFrameActive(0.310f, 0.340f, 0.390f, 1.0f);
    const ImVec4 kTitleBg(0.145f, 0.155f, 0.180f, 1.0f);
    const ImVec4 kTitleActive(0.220f, 0.240f, 0.280f, 1.0f);
    const ImVec4 kBorder(0.250f, 0.270f, 0.310f, 0.80f);
    const ImVec4 kText(0.88f, 0.89f, 0.92f, 1.0f);
    const ImVec4 kTextDisabled(0.42f, 0.45f, 0.51f, 1.0f);
    const ImVec4 kHeader(0.220f, 0.240f, 0.280f, 1.0f);
    const ImVec4 kHeaderHover(0.290f, 0.320f, 0.380f, 1.0f);
    const ImVec4 kHeaderActive(0.350f, 0.390f, 0.460f, 1.0f);
    const ImVec4 kTabHover(0.290f, 0.320f, 0.380f, 1.0f);
    const ImVec4 kTabSelected(0.240f, 0.290f, 0.360f, 1.0f);
    const ImVec4 kTabDimmed(0.170f, 0.185f, 0.220f, 1.0f);
    const ImVec4 kTabDimmedSelected(0.200f, 0.235f, 0.290f, 1.0f);
    const ImVec4 kSeparator(0.250f, 0.270f, 0.310f, 1.0f);
    const ImVec4 kScrollbarGrab(0.330f, 0.360f, 0.410f, 1.0f);
    const ImVec4 kScrollbarHover(0.420f, 0.460f, 0.520f, 1.0f);
    const ImVec4 kScrollbarActive(0.500f, 0.550f, 0.620f, 1.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDisabled;
    c[ImGuiCol_WindowBg] = kWindowBg;
    c[ImGuiCol_ChildBg] = kChildBg;
    c[ImGuiCol_PopupBg] = kPopupBg;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = kFrameBg;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kTitleBg;
    c[ImGuiCol_TitleBgActive] = kTitleActive;
    c[ImGuiCol_TitleBgCollapsed] = kTitleBg;
    c[ImGuiCol_MenuBarBg] = kTitleBg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.8f);
    c[ImGuiCol_ScrollbarGrab] = kScrollbarGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = kScrollbarHover;
    c[ImGuiCol_ScrollbarGrabActive] = kScrollbarActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kScrollbarGrab;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = kFrameBg;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = kHeader;
    c[ImGuiCol_HeaderHovered] = kHeaderHover;
    c[ImGuiCol_HeaderActive] = kHeaderActive;
    c[ImGuiCol_Separator] = kSeparator;
    c[ImGuiCol_SeparatorHovered] = kHeaderHover;
    c[ImGuiCol_SeparatorActive] = kHeaderActive;
    c[ImGuiCol_ResizeGrip] = kScrollbarGrab;
    c[ImGuiCol_ResizeGripHovered] = kScrollbarHover;
    c[ImGuiCol_ResizeGripActive] = kScrollbarActive;
    c[ImGuiCol_Tab] = kTabDimmed;
    c[ImGuiCol_TabHovered] = kTabHover;
    c[ImGuiCol_TabSelected] = kTabSelected;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kTabDimmed;
    c[ImGuiCol_TabDimmedSelected] = kTabDimmedSelected;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_DockingPreview] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.09f, 0.095f, 0.12f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = kHeader;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextLink] = kAccent;
    c[ImGuiCol_PlotLines] = kAccent;
    c[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavHighlight] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    if (pluginMgr_) pluginMgr_->Shutdown();
    if (audioBackend_) {
        audioBackend_->Shutdown();
        audioBackend_.reset();
    }
    // Release the offscreen thumbnail targets + their ImGui registrations
    // before the renderer shuts down.
    if (gfx::IRenderBackend* backend = renderer_.Backend()) {
        for (SceneEntity& e : entities_) {
            if (e.customShader.Valid()) backend->DestroyShader(e.customShader.Handle());
        }
        for (auto& kv : meshThumbs_) {
            if (kv.second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(kv.second.texHandle);
            if (kv.second.rt.Valid()) backend->DestroyRenderTarget(kv.second.rt);
        }
        for (auto& kv : materialThumbs_) {
            if (kv.second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(kv.second.texHandle);
            if (kv.second.rt.Valid()) backend->DestroyRenderTarget(kv.second.rt);
        }
    }
    meshThumbs_.clear();
    meshThumbQueue_.clear();
    materialThumbs_.clear();
    materialThumbQueue_.clear();
    if (benchMode_ && benchFrames_ > 0) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                     "BENCH-SUMMARY frames=%llu avgMs=%.2f maxMs=%.2f",
                     static_cast<unsigned long long>(benchFrames_),
                     benchFrameMsSum_ / static_cast<float>(benchFrames_), benchFrameMsMax_);
    }
    gfx::ImGuiNeon_Shutdown();
    renderer_.Shutdown();
}

void EditorApp::SetupScene() {
    auto add = [&](const std::string& key, const std::string& name, const math::Vec3& pos,
                   const math::Vec3& scale, const gfx::Color& tint) {
        SceneEntity e;
        e.name = name;
        e.meshKey = key;
        e.pos = pos;
        e.scale = scale;
        e.tint = tint;
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            entities_.push_back(std::move(e));
        }
    };

    // --- Ground: rolling hills with a village pond carved in the SW corner ---
    add("terrain", "地面", {0, 0, 0}, {1, 1, 1}, gfx::Color::White);
    add("water", "湖泊", {-18, -1.15f, -18}, {1.05f, 1, 1.05f}, gfx::Color{0.15f, 0.45f, 0.85f, 1});

    // --- Village (centre): roads, houses, villagers ---
    const float kRoadW = 2.8f;
    add("road", "主干道", {0, 0.03f, 0}, {kRoadW, 1, 17.0f}, gfx::Color{0.44f, 0.39f, 0.32f, 1});
    add("road", "横街", {0, 0.03f, 0}, {15.0f, 1, kRoadW}, gfx::Color{0.40f, 0.36f, 0.30f, 1});
    add("road", "小路_东", {5.5f, 0.03f, -2.5f}, {2.0f, 1, 8.0f}, gfx::Color{0.38f, 0.34f, 0.29f, 1});
    add("road", "小路_西", {-5.5f, 0.03f, -2.5f}, {2.0f, 1, 8.0f}, gfx::Color{0.38f, 0.34f, 0.29f, 1});

    add("house", "农舍_东", {4.6f, 0, 3.2f}, {1.15f, 1.15f, 1.15f}, gfx::Color::White);
    add("house", "旅店", {7.2f, 0, -2.2f}, {1.35f, 1.35f, 1.35f}, gfx::Color::White);
    add("house", "农舍_西", {-4.6f, 0, 3.2f}, {1.15f, 1.15f, 1.15f}, gfx::Color::White);
    add("house", "铁匠铺", {-7.2f, 0, -2.2f}, {1.2f, 1.2f, 1.2f}, gfx::Color::White);

    add("npc", "村民_商人", {1.8f, 0, 1.2f}, {1, 1, 1}, gfx::Color{0.78f, 0.28f, 0.18f, 1});
    add("npc", "村民_农夫", {-1.8f, 0, 1.4f}, {1, 1, 1}, gfx::Color{0.30f, 0.55f, 0.78f, 1});
    add("npc", "村民_猎人", {0.6f, 0, -1.6f}, {1, 1, 1}, gfx::Color{0.48f, 0.42f, 0.20f, 1});
    add("npc", "村民_法师", {3.0f, 0, -0.8f}, {1, 1, 1}, gfx::Color{0.60f, 0.36f, 0.72f, 1});
    add("npc", "村民_卫兵", {-3.0f, 0, -0.8f}, {1.05f, 1.05f, 1.05f}, gfx::Color{0.55f, 0.55f, 0.58f, 1});
    // The DamagedHelmet sits on a plinth as the village's trophy.
    add("helmet", "展示头盔", {0.0f, 0.95f, 2.6f}, {1, 1, 1}, gfx::Color::White);
    add("cube", "展示台", {0.0f, 0.45f, 2.6f}, {1.4f, 0.9f, 1.4f}, gfx::Color{0.55f, 0.42f, 0.30f, 1});

    // --- Wilderness: deterministic scatter of trees / rocks / bushes ---
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFFu) / 65535.0f;
    };
    auto within = [&](float lo, float hi) { return lo + rnd() * (hi - lo); };
    auto nearVillage = [](const math::Vec3& p) { return p.x * p.x + p.z * p.z < 8.5f * 8.5f; };
    auto inLake = [](const math::Vec3& p) {
        float dx = p.x + 18.0f, dz = p.z + 18.0f;
        return dx * dx + dz * dz < 11.5f * 11.5f;
    };
    int treeN = 0, rockN = 0, bushN = 0;
    for (int i = 0; i < 80; ++i) {
        math::Vec3 p{within(-26.0f, 26.0f), 0.0f, within(-26.0f, 26.0f)};
        if (nearVillage(p) || inLake(p)) continue;
        const float r = rnd();
        if (r < 0.52f) {
            add("tree", "松树_" + std::to_string(treeN++), p,
                {0.9f + within(0.0f, 0.7f), 1, 0.9f + within(0.0f, 0.7f)}, gfx::Color::White);
        } else if (r < 0.82f) {
            add("rock", "岩石_" + std::to_string(rockN++), {p.x, 0, p.z},
                {within(0.55f, 1.4f), within(0.45f, 0.9f), within(0.55f, 1.4f)},
                gfx::Color{0.58f, 0.58f, 0.58f, 1});
        } else {
            add("bush", "灌木_" + std::to_string(bushN++), {p.x, 0.15f, p.z}, {1, 1, 1},
                gfx::Color::White);
        }
    }
    // A few trees inside the village edge for shade.
    add("tree", "村口老树", {3.2f, 0, 4.6f}, {1.5f, 1, 1.5f}, gfx::Color::White);
    add("tree", "村口老树_2", {-3.2f, 0, 4.6f}, {1.3f, 1, 1.3f}, gfx::Color::White);
    add("tree", "村口老树_3", {3.4f, 0, -4.4f}, {1.2f, 1, 1.2f}, gfx::Color::White);

    // --- Playable hero: blue-armored figure bound to the hero controller
    // script (WASD/jump/melee/fireball/heal), parked at the south end of the
    // main road. Its health lives in the scene so combat can damage it.
    {
        SceneEntity h;
        h.name = "英雄";
        h.meshKey = "hero";
        h.pos = {0.0f, 0.0f, 5.5f};
        h.scale = {1, 1, 1};
        h.tint = gfx::Color::White;
        h.scripts.push_back({"lua", "scripts/hero.lua", {}});
        h.hp = 100.0f;
        h.maxHp = 100.0f;
        if (ResolveMesh(h)) {
            ApplyMaterialParams(h);
            entities_.push_back(std::move(h));
        }
    }

    // --- Hostile wolves in the wilderness: combat targets for the hero's
    // skills (static for now; the hero can melee/fireball them).
    uint32_t wolfSeed = 0x6D2B79F5u;
    auto wrnd = [&wolfSeed]() {
        wolfSeed ^= wolfSeed << 13;
        wolfSeed ^= wolfSeed >> 17;
        wolfSeed ^= wolfSeed << 5;
        return static_cast<float>(wolfSeed & 0xFFFFu) / 65535.0f;
    };
    for (int i = 0; i < 8; ++i) {
        math::Vec3 wp{10.0f + wrnd() * 14.0f, 0.0f, -12.0f - wrnd() * 12.0f};
        if (i % 2) wp.x = -wp.x;
        add("wolf", "野狼_" + std::to_string(i), wp,
            {1.0f + wrnd() * 0.3f, 1, 1.0f + wrnd() * 0.3f}, gfx::Color::White);
        if (!entities_.empty()) {
            entities_.back().hp = 40.0f;
            entities_.back().maxHp = 40.0f;
        }
    }
    LoadScene("editor_scene.json");
    SetSelection(entities_.empty() ? -1 : 0);
    NormalizeEntityIds(); // setup-created entities also need stable ids
    EnsureSceneDefaultObjects();
}

void EditorApp::OnUpdate(float dt) {
    // P2-6 benchmark: per-interval frame-time/draw logs + an exit summary.
    if (benchMode_) {
        const float ms = TimeRef().delta * 1000.0f;
        ++benchFrames_;
        benchFrameMsSum_ += ms;
        benchFrameMsMax_ = std::fmax(benchFrameMsMax_, ms);
        if (TimeRef().frameIndex - benchLastLogFrame_ >= 60) {
            benchLastLogFrame_ = TimeRef().frameIndex;
            const assets::AssetStats st = assetMgr_.Stats();
            NEON_LOG_CAT(
                core::LogCategory::Core, core::LogLevel::Info,
                "BENCH frame=%llu fps=%.1f avgMs=%.2f maxMs=%.2f ents=%zu draws=%u bodies=%zu "
                "tex=%zu mesh=%zu",
                static_cast<unsigned long long>(TimeRef().frameIndex), TimeRef().Fps(),
                benchFrameMsSum_ / static_cast<float>(benchFrames_), benchFrameMsMax_,
                entities_.size(), renderer_.Stats().drawCalls,
                play_ ? play_->PhysicsBodyCount() : 0, st.textures, st.meshes);
        }
    }
    // Drain completed async texture decodes (uploads + callbacks, main thread).
    assetMgr_.PumpAsync();
    // Advance animated skinned glTF entities (edit-mode pose; the play
    // advances its own animators inside GameRuntime::TickAnimations).
    for (SceneEntity& e : entities_) {
        if (e.skinned && e.skinned->Valid()) e.skinned->Update(dt);
    }
    // Advance the model-preview playhead.
    if (showModelPreview_ && previewModel_ && previewPlaying_) {
        float dur = previewModel_->clips.empty()
                        ? 1.0f
                        : previewModel_->clips[static_cast<size_t>(previewClip_)].duration;
        if (dur > 0.0f) previewTime_ = std::fmod(previewTime_ + dt, dur);
    }
    // The gizmo drag-sim (frame 30) needs the real mouse to hover the viewport
    // so ImGui's hover hit-test yields the dock host window (the window
    // SetAlternativeWindow points at); headless starts at (0,0) over the menu
    // bar, so park it on the viewport center for the smoke frame.
    if (smokeMode_ && TimeRef().frameIndex == 30) {
        platform::InputEvent e;
        e.type = platform::InputEvent::Type::MouseMove;
        e.x = renderer_.ScreenWidth() / 2;
        e.y = renderer_.ScreenHeight() / 2;
        Input()->HandleEvent(e);
    }
    if (showUIEditor_ && uiDocOpen_) {
        // UI editor: the viewport edits the UI document (select/move/resize).
        UpdateUIEditorViewport();
    } else {
        // 2D and 3D share one input path: 2D is just the front-ortho camera,
        // so middle-drag pans the camera target and the wheel zooms the ortho
        // size (the camera frame moves with it).
        UpdateViewport(dt);
        if (playActive_ && play_) {
            // P1-2 debugger: push edited breakpoints into the play host
            // (cheap, only when the set changed).
            if (scriptBreakpointsDirty_ && play_->ScriptHost()) {
                for (const auto& kv : scriptBreakpoints_) {
                    std::vector<int> lines(kv.second.begin(), kv.second.end());
                    play_->ScriptHost()->SetScriptBreakpoints(kv.first, lines);
                }
                scriptBreakpointsDirty_ = false;
            }
            play_->Tick(dt);
        }
    }
    // Hot reload (T4.8): throttled mtime poll for the play's scripts and
    // the scene's referenced assets. Off unless --hot / the toolbar toggle.
    if (hotReload_ && TimeRef().frameIndex - hotReloadFrame_ >= 30) {
        hotReloadFrame_ = TimeRef().frameIndex;
        PollHotReload();
    }
    gfx::ImGuiNeon_NewFrame(*Input(), pendingText_, dt);
    pendingText_.clear();
    ImGui::NewFrame();
    // ImGui's implicit fallback window ("Debug##Default") defaults to (60,60)
    // 400x400 - right on top of the viewport's upper-left corner, swallowing
    // the viewport dock tab and the camera input there. Park it off-screen.
    if (ImGuiWindow* fallback = ImGui::FindWindowByName("Debug##Default"))
        ImGui::SetWindowPos(fallback, ImVec2(-100000.0f, -100000.0f));
    // ImGui's hover resolution can report the DockSpace host instead of the
    // docked leaf window under the mouse, which makes every panel button
    // unclickable (ItemHoverable requires HoveredWindow == the item's window).
    // Re-resolve the hover to the topmost visible docked leaf under the mouse.
    {
        ImGuiContext& ictx = *ImGui::GetCurrentContext();
        ImGuiWindow* best = nullptr;
        for (int wi = ictx.Windows.Size - 1; wi >= 0; --wi) {
            ImGuiWindow* w = ictx.Windows[wi];
            if (!w || w->Hidden) continue;
            if (w->DockNodeAsHost != nullptr) continue; // dock host / tab bar
            if (w->ParentWindow != nullptr) continue;   // child windows
            if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue; // overlays
            // The transform gizmo binds to the dock host over the 视口, so
            // keep the host hover there; only re-resolve tool panels.
            if (std::strcmp(w->Name, "视口") == 0) continue;
            if (std::strncmp(w->Name, "##", 2) == 0) continue; // internal windows
            if (w->Rect().Contains(ictx.IO.MousePos)) {
                best = w;
                break;
            }
        }
        if (best && best != ictx.HoveredWindow) {
            ictx.HoveredWindow = best;
        }
    }
    BuildImGuiUI();
    ImGui::Render();

    // Smoke: panel open/close state must round-trip through the ini settings
    // handler (the user's report: a panel opened and docked vanished on the
    // next launch because only the layout was saved, not whether it was open).
    // Drive the handler's write path into a buffer, lose the states, then feed
    // the lines back through the read path exactly as LoadIniSettingsFromDisk
    // would, and verify the flags come back.
    if (smokeMode_ && TimeRef().frameIndex == 100) {
        static bool panelSmokeDone = false;
        if (!panelSmokeDone) {
            panelSmokeDone = true;
            bool ok = g_panelStateApp == this && g_panelStateEntries != nullptr;
            ImGuiSettingsHandler* h = ImGui::FindSettingsHandler("NeonPanels");
            ok = ok && h != nullptr;
            const bool savedAssets = showAssets_;
            const bool savedLoc = showLoc_;
            if (ok) {
                showAssets_ = true;  // open a panel (user docks it in real use)
                showLoc_ = false;    // close another
                ImGuiTextBuffer buf;
                h->WriteAllFn(ImGui::GetCurrentContext(), h, &buf);
                showAssets_ = false; // "lost" states; the read must restore
                showLoc_ = true;
                const char* p = buf.begin();
                while (p && *p) {
                    const char* nl = std::strchr(p, '\n');
                    const std::string line(
                        p, nl ? static_cast<size_t>(nl - p) : std::strlen(p));
                    if (!line.empty() && line[0] != '[')
                        h->ReadLineFn(ImGui::GetCurrentContext(), h,
                                      g_panelStateApp, line.c_str());
                    if (!nl) break;
                    p = nl + 1;
                }
                ok = showAssets_ && !showLoc_;
            }
            showAssets_ = savedAssets;
            showLoc_ = savedLoc;
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Info,
                         "EDITOR-PANELS-SMOKE: [%s] panel open/close state "
                         "round-trips through the ini handler",
                         ok ? "PASS" : "FAIL");
            if (!ok) smokeFailed_ = true;
        }
    }

    // Smoke: the 网格 移除 button's action (the command the button pushes)
    // works through the undo stack. Real mouse input cannot be synthesized
    // reliably here - the physical cursor overrides synthetic events every
    // frame - so the action is driven through the same command the button
    // handler pushes, then undone.
    if (smokeMode_ && !smokeRemoveActionDone_ && TimeRef().frameIndex >= 45) {
        smokeRemoveActionDone_ = true;
        for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
            if (!entities_[static_cast<size_t>(i)].meshKey.empty() &&
                entities_[static_cast<size_t>(i)].spriteTex.empty()) {
                SetSelection(i);
                break;
            }
        }
        const bool actionOk =
            selected_ >= 0 && selected_ < static_cast<int>(entities_.size());
        bool removed = false;
        bool healthOk = true;
        bool scriptOk = true;
        if (actionOk) {
            SceneEntity& e = entities_[static_cast<size_t>(selected_)];
            const std::string oldKey = entities_[static_cast<size_t>(selected_)].meshKey;
            history_.Push(std::make_unique<EditMeshKeyCommand>(
                this, &entities_, selected_, oldKey, ""));
            removed = entities_[static_cast<size_t>(selected_)].meshKey.empty();
            history_.Undo();
            // 生命 remove: the command the 移除##health button pushes.
            if (e.maxHp > 0.0f) {
                const HealthValue oldV{e.hp, e.maxHp};
                history_.Push(std::make_unique<EditPropertyCommand<HealthValue>>(
                    &entities_, selected_, ApplyHealth, oldV, HealthValue{},
                    /*mergeable=*/false));
                healthOk = e.maxHp == 0.0f && e.hp == 0.0f;
                history_.Undo();
                healthOk = healthOk && e.maxHp == oldV.maxHp;
            }
            // 脚本 remove: append one script, then erase it via the command
            // the 移除##script_N button pushes.
            std::vector<SceneScriptFields> withScript = e.scripts;
            withScript.push_back({"lua", "scripts/smoke_remove.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, selected_, ApplyScriptList, e.scripts, withScript,
                /*mergeable=*/false));
            if (e.scripts.size() == 1) {
                std::vector<SceneScriptFields> after = e.scripts;
                after.clear();
                history_.Push(std::make_unique<
                    EditPropertyCommand<std::vector<SceneScriptFields>>>(
                    &entities_, selected_, ApplyScriptList, e.scripts, after,
                    /*mergeable=*/false));
                scriptOk = e.scripts.empty();
                history_.Undo();
                history_.Undo();
                scriptOk = scriptOk && e.scripts.empty();
            }
        }
        NEON_LOG_INFO("EDITOR-REMOVE-BTN-SMOKE: [%s] remove actions work "
                      "(mesh=%d health=%d script=%d sel=%d)",
                      actionOk && removed && healthOk && scriptOk ? "PASS" : "FAIL",
                      removed ? 1 : 0, healthOk ? 1 : 0, scriptOk ? 1 : 0, selected_);
        if (!actionOk || !removed || !healthOk || !scriptOk) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 29) {
        showHierarchy_ = true;
        showInspector_ = true;
        showAssets_ = true;
        showResources_ = true;
        showLog_ = true;
        showBt_ = true;
        showScripts_ = true;
        showPackage_ = true;
        showProfiler_ = true;
        // Force the BT tab active for the upcoming render: when the persisted
        // dock layout tabs the panel away, ImGui::Begin returns false and the
        // canvas never emits vertices, so btCanvasDrawn_ (checked at frame 30)
        // would stay false. Focus is honored on the next frame's Begin.
        ImGui::SetWindowFocus("\u884c\u4e3a\u6811");
        // Seed a small tree so the BT canvas renders real nodes on the smoke
        // frame (frame 30) and the smoke can assert the canvas drew geometry.
        btGraph_ = btgraph::BtGraph{};
        const std::string r = btGraph_.AddNode("sequence", math::Vec2{20.f, 20.f});
        const std::string c = btGraph_.AddNode("in_range", math::Vec2{20.f, 180.f});
        const std::string a = btGraph_.AddNode("move_to", math::Vec2{240.f, 180.f});
        core::Json dist;
        dist.type_ = core::Json::Type::Number;
        dist.number_ = 8.0;
        core::Json speed;
        speed.type_ = core::Json::Type::Number;
        speed.number_ = 3.0;
        btGraph_.SetArg(c, "distance", dist);
        btGraph_.SetArg(a, "speed", speed);
        btGraph_.SetParent(c, r);
        btGraph_.SetParent(a, r);
        btSelected_ = r;
    }
    if (smokeMode_ && TimeRef().frameIndex == 30) RunUISmokeTest();

    // T4.8 smoke: the frame-30 OnRender generated the queued mesh thumbnail
    // (the asset panel selected the model during RunUISmokeTest); verify the
    // cache + profiler output here, then arm the ortho render check. The app
    // can run several fixed ticks between renders, so the checks key off
    // lastRenderTick_ (the tick the most recent OnRender processed) rather
    // than assuming a 1:1 tick/render correspondence.
    if (smokeMode_ && TimeRef().frameIndex == 31) {
        NEON_LOG_INFO("EDITOR-PROFILER-SMOKE: [%s] profiler panel populated",
                      profilerDrawn_ ? "PASS" : "FAIL");
        if (!profilerDrawn_) smokeFailed_ = true;
        viewCam_ = ViewCam::Top; // next OnRender renders the top ortho view
    }
    if (smokeMode_ && !thumbSmokeDone_ && !smokeThumbPath_.empty()) {
        // Once the queue has been processed by a render, the cache holds the
        // path (a valid texture = the mesh rendered; an invalid one = the
        // asset failed to load). Until then, keep waiting.
        if (lastRenderTick_ >= 32) {
            auto it = meshThumbs_.find(smokeThumbPath_);
            thumbSmokeDone_ = true;
            const bool ok = it != meshThumbs_.end() &&
                            it->second.texId != ImTextureID_Invalid && it->second.rt.Valid();
            NEON_LOG_INFO("EDITOR-THUMB-SMOKE: [%s] mesh thumbnail cached (%s)",
                          ok ? "PASS" : "FAIL", smokeThumbPath_.c_str());
            if (!ok) smokeFailed_ = true;
        }
    }
    if (smokeMode_ && editMode_ == EditMode::Scene3D && !camSmokeDone_ &&
        viewCam_ == ViewCam::Top) {
        // A render has now processed the Top arm (tick 31): lastRenderCamOrtho_
        // reflects that frame's camera and the scene draw-call count.
        if (lastRenderTick_ >= 32) {
            camSmokeDone_ = true;
            const bool ok = lastRenderCamOrtho_ && smokeDrawCalls_ > 0;
            NEON_LOG_INFO("EDITOR-CAM-SMOKE: [%s] top ortho camera rendered the viewport "
                          "(drawCalls=%u)",
                          ok ? "PASS" : "FAIL", smokeDrawCalls_);
            if (!ok) smokeFailed_ = true;
            viewCam_ = ViewCam::Perspective;
        }
    }

    // T4.8 smoke: hot reload. Frame 40 wires up a temp project with a script,
    // attaches it, starts the play and records the script mtime baseline.
    // Frame 41 bumps the file's mtime; frame 42 polls and asserts the play
    // was torn down and restarted.
    if (smokeMode_ && TimeRef().frameIndex == 40) {
        const std::string proj = GetTempDir() + "/hotreload_proj";
        EnsureDirs(proj + "/scripts");
        {
            std::ofstream out(proj + "/scripts/main.lua", std::ios::binary);
            out << "function on_start(ent)\nend\nfunction on_update(ent, dt)\nend\n";
        }
        hotReloadProj_ = proj;
        prevProjectDir_ = projectDir_;
        projectDir_ = proj;
        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            SceneEntity& sel = entities_[static_cast<size_t>(selected_)];
            core::Json vars;
            vars.type_ = core::Json::Type::Object;
            std::vector<SceneScriptFields> newList = sel.scripts;
            newList.push_back({"lua", "scripts/main.lua", vars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, selected_, ApplyScriptList, sel.scripts, newList,
                /*mergeable=*/false));
        }
        hotReload_ = true;
        StartPlay();
        PollHotReload(); // baseline: record the script's mtime (no restart)
        const bool active = playActive_ && play_ && play_->Running();
        NEON_LOG_INFO("EDITOR-HOTRELOAD-SMOKE: [%s] play running for hot reload",
                      active ? "PASS" : "FAIL");
        if (!active) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 41) {
        TouchFileMTime(hotReloadProj_ + "/scripts/main.lua", 2);
    }
    if (smokeMode_ && TimeRef().frameIndex == 42) {
        const int before = hotReloadCount_;
        PollHotReload();
        const bool restarted = hotReloadCount_ > before && playActive_ && play_;
        NEON_LOG_INFO("EDITOR-HOTRELOAD-SMOKE: [%s] script mtime change restarted the play",
                      restarted ? "PASS" : "FAIL");
        if (!restarted) smokeFailed_ = true;
        hotReload_ = false;
        projectDir_ = prevProjectDir_;
    }

    // Godot-style project switcher smoke: ScanProjects discovers both bundled
    // projects, SwitchProject enters the 2D project's canvas with its level
    // loaded, the 3D project loads its start scene, then we normalize back to
    // the canonical sandbox (editor_scene.json) so the play smoke at
    // frame 60 sees the deterministic 3D scene regardless of the saved config.
    if (smokeMode_ && TimeRef().frameIndex == 43) {
        ScanProjects();
        bool has2D = false, has3D = false;
        for (const EditorProject& p : projects_) {
            if (p.mode == "2d" && !p.scenes.empty()) has2D = true;
            if (p.mode == "3d" && !p.scenes.empty()) has3D = true;
        }
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] discovered 2D+3D projects (%zu)",
                      has2D && has3D ? "PASS" : "FAIL", projects_.size());
        if (!has2D || !has3D) smokeFailed_ = true;
        SwitchProject("projects/pvz");
        const bool pvzOk = editMode_ == EditMode::Scene2D && !entities_.empty() &&
                           pvzPlants_.size() > 0 && currentSceneName_ == "pvz.json";
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] 2D project switch -> 2D view (parsed %zu plants)",
                      pvzOk ? "PASS" : "FAIL", pvzPlants_.size());
        if (!pvzOk) smokeFailed_ = true;
        const bool assetOk = assetDir_ == "projects/pvz/assets";
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] asset panel follows the project assets dir",
                      assetOk ? "PASS" : "FAIL");
        if (!assetOk) smokeFailed_ = true;
        SwitchProject("projects/neon_realm");
        const bool realmOk = editMode_ == EditMode::Scene3D && !entities_.empty();
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] 3D project switch loaded its scene (%zu)",
                      realmOk ? "PASS" : "FAIL", entities_.size());
        if (!realmOk) smokeFailed_ = true;
        StopPlay();
        editMode_ = EditMode::Scene3D;
        LoadScene("editor_scene.json");
        // The sandbox scene is user data (SaveScene writes it), so only assert
        // the 3D scene tree is back and non-empty - not a fixed entity count.
        const bool backOk = editMode_ == EditMode::Scene3D && !entities_.empty();
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] normalized to the 3D sandbox (%zu)",
                      backOk ? "PASS" : "FAIL", entities_.size());
        if (!backOk) smokeFailed_ = true;
    }

    // Material-ball sphere preview: queued at frame 30, the offscreen render
    // runs in a later frame's OnRender; verify the cached texture landed.
    if (smokeMode_ && TimeRef().frameIndex == 44) {
        const std::string path = GetTempDir() + "/asset_proj/materials/smoke_mat.mat.json";
        const auto it = materialThumbs_.find(path);
        const bool ok = it != materialThumbs_.end() &&
                        it->second.texId != ImTextureID_Invalid;
        NEON_LOG_INFO("EDITOR-MATERIAL-SMOKE: [%s] material ball sphere preview generated",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
        const std::string zhPath =
            GetTempDir() + "/asset_proj/materials/\u6d4b\u8bd5\u7403.mat.json";
        const auto itZh = materialThumbs_.find(zhPath);
        const bool zhOk = itZh != materialThumbs_.end() &&
                          itZh->second.texId != ImTextureID_Invalid;
        NEON_LOG_INFO("EDITOR-MATERIAL-SMOKE-CJK: [%s] CJK-named material ball preview",
                      zhOk ? "PASS" : "FAIL");
        if (!zhOk) smokeFailed_ = true;
    }
    // P2-6 shader hot reload smoke: compile a temp fragment shader on the
    // first entity, rewrite the file, recompile (mtime gate), restore.
    if (smokeMode_ && TimeRef().frameIndex == 46) {
        bool ok = !entities_.empty();
        const bool glBackend = backendName_ != "vulkan";  // custom shaders: GL only
        SceneEntity* target = entities_.empty() ? nullptr : &entities_[0];
        const std::string shPath = GetTempDir() + "/smoke_tint.glsl";
        const std::string oldShader = target ? target->shaderPath : "";
        if (ok) {
            if (std::ofstream out(shPath, std::ios::binary); out.is_open()) {
                out << "#version 330 core\n"
                    << "in vec2 vUV;\nin vec4 vColor;\nout vec4 FragColor;\n"
                    << "uniform sampler2D uTex;\n"
                    << "void main() { FragColor = vColor * texture(uTex, vUV); }\n";
            }
            target->shaderPath = shPath;
            ReloadEntityShader(*target);
            ok = !glBackend || target->customShader.Valid();
            if (ok) {
                // Touch the file and recompile (simulates a hot reload).
                if (std::ofstream out2(shPath, std::ios::app); out2) out2 << "// touched\n";
                const uint64_t before = FileMTime(shPath);
                ReloadEntityShader(*target);
                ok = (!glBackend || target->customShader.Valid()) && FileMTime(shPath) >= before;
            }
        }
        if (target) {
            target->shaderPath = oldShader;
            ReloadEntityShader(*target);
        }
        NEON_LOG_INFO("EDITOR-SHADER-SMOKE: [%s] custom fragment shader compile + hot reload",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P1-1 terrain tool smoke: edit the heightmap canvas and rebuild the mesh.
    if (smokeMode_ && TimeRef().frameIndex == 47) {
        bool ok = false;
        for (SceneEntity& e : entities_) {
            if (e.meshKey != "terrain") continue;
            RebuildTerrainMesh(e);
            if (e.terrainHeights_.empty() || !e.mesh.Valid()) break;
            const float before = e.terrainHeights_[0];
            e.terrainHeights_[0] = before + 1.0f;
            RebuildTerrainMesh(e);
            ok = e.mesh.Valid() &&
                 std::fabs(e.terrainHeights_[0] - before - 1.0f) < 1e-4f;
            break;
        }
        NEON_LOG_INFO("EDITOR-TERRAIN-SMOKE: [%s] terrain heightmap edit + rebuild",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P1-1 tilemap smoke: a "tilemap" entity resolves without a mesh and
    // carries its cell grid.
    if (smokeMode_ && TimeRef().frameIndex == 48) {
        SceneEntity tm;
        tm.name = "smoke_tilemap";
        tm.meshKey = "tilemap";
        tm.tilemapCols_ = 2;
        tm.tilemapRows_ = 2;
        tm.tilemapTiles_ = {"a.png", "", "b.png", ""};
        const bool ok = ResolveMesh(tm) && !tm.mesh.Valid() &&
                        tm.tilemapTiles_.size() == 4u;
        NEON_LOG_INFO("EDITOR-TILEMAP-SMOKE: [%s] tilemap entity resolves (cells=%zu)",
                      ok ? "PASS" : "FAIL", tm.tilemapTiles_.size());
        if (!ok) smokeFailed_ = true;
    }
    // P2-1 decal smoke: exporting a scene with a decal writes the runtime
    // component.
    if (smokeMode_ && TimeRef().frameIndex == 49) {
        bool ok = false;
        if (!entities_.empty()) {
            SceneEntity& e = entities_[0];
            const std::string oldTex = e.decalTex;
            e.decalTex = "assets/textures/decal.png";
            e.decalSize = 3.0f;
            e.decalAlpha = 0.5f;
            auto rootRes = BuildPlaySceneJson();
            e.decalTex = oldTex;
            e.decalMesh = {};
            if (rootRes.Ok()) {
                const core::Json* ents = rootRes.Value().Get("entities");
                if (ents && ents->Size() > 0) {
                    const core::Json* comps = ents->At(0)->Get("components");
                    ok = comps && comps->Get("decal") != nullptr;
                }
            }
        }
        NEON_LOG_INFO("EDITOR-DECAL-SMOKE: [%s] decal exports as a runtime component",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P2-editor UX smoke: multi-selection + batch duplicate/delete + batch
    // gizmo transform command round-trip.
    if (smokeMode_ && TimeRef().frameIndex == 50) {
        bool ok = entities_.size() >= 2;
        if (ok) {
            SetSelection(0);
            ToggleSelection(1);
            ok = selection_.size() == 2 && IsSelected(0) && IsSelected(1);
            const size_t before = entities_.size();
            history_.Push(
                std::make_unique<MultiDuplicateEntityCommand>(&entities_, SelectedIndices()));
            ok = ok && entities_.size() == before + 2;
            ClampSelection();
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ok = ok && entities_.size() == before;
            ClampSelection();
            // Batch transform: move two entities by (1,0,0), then undo.
            const math::Vec3 p0 = entities_[0].pos;
            const math::Vec3 p1 = entities_[1].pos;
            std::vector<int> ids = {0, 1};
            std::vector<Transform3> from = {
                {entities_[0].pos, entities_[0].rot, entities_[0].scale},
                {entities_[1].pos, entities_[1].rot, entities_[1].scale}};
            std::vector<Transform3> to = from;
            to[0].pos.x += 1.0f;
            to[1].pos.x += 1.0f;
            history_.Push(std::make_unique<BatchTransformCommand>(&entities_, ids, from, to));
            ok = ok && std::fabs(entities_[0].pos.x - p0.x - 1.0f) < 1e-5f &&
                 std::fabs(entities_[1].pos.x - p1.x - 1.0f) < 1e-5f;
            history_.Undo();
            ok = ok && std::fabs(entities_[0].pos.x - p0.x) < 1e-5f &&
                 std::fabs(entities_[1].pos.x - p1.x) < 1e-5f;
            SetSelection(0);
        }
        NEON_LOG_INFO("EDITOR-MULTISELECT-SMOKE: [%s] multi-select + batch ops",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // Scene-tree smoke: per-parent recursive name sort (multi-level tree:
    // parent before children, each sibling group alphabetical) and the one
    // undo step that restores the previous order. The temporary names and
    // parent links are restored before the frame ends so the play smoke
    // at frame 60 sees the untouched scene.
    if (smokeMode_ && TimeRef().frameIndex == 51) {
        bool ok = entities_.size() >= 3;
        if (ok) {
            NormalizeEntityIds();
            const int id0 = entities_[0].id;
            const int id1 = entities_[1].id;
            const int id2 = entities_[2].id;
            const std::string n0 = entities_[0].name;
            const std::string n1 = entities_[1].name;
            const std::string n2 = entities_[2].name;
            const int p1 = entities_[1].parentId;
            const int p2 = entities_[2].parentId;
            entities_[0].name = "B_root";
            entities_[1].name = "A_child";
            entities_[2].name = "C_child";
            entities_[1].parentId = id0;
            entities_[2].parentId = id1; // grandchild: 3 levels deep
            const size_t undoDepthBefore = history_.UndoDepth();
            SortSceneTreeByName();
            size_t i0 = static_cast<size_t>(-1);
            size_t i1 = static_cast<size_t>(-1);
            size_t i2 = static_cast<size_t>(-1);
            for (size_t i = 0; i < entities_.size(); ++i) {
                if (entities_[i].id == id0) i0 = i;
                if (entities_[i].id == id1) i1 = i;
                if (entities_[i].id == id2) i2 = i;
            }
            ok = i0 != static_cast<size_t>(-1) && i1 != static_cast<size_t>(-1) &&
                 i2 != static_cast<size_t>(-1) && i0 < i1 && i1 < i2;
            if (history_.UndoDepth() > undoDepthBefore)
                history_.Undo(); // restore the pre-sort order
            for (SceneEntity& e : entities_) {
                if (e.id == id0) e.name = n0;
                if (e.id == id1) {
                    e.name = n1;
                    e.parentId = p1;
                }
                if (e.id == id2) {
                    e.name = n2;
                    e.parentId = p2;
                }
            }
        }
        NEON_LOG_INFO("EDITOR-SCENETREE-SMOKE: [%s] per-parent recursive sort + undo",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // Save/load round-trip: SaveScene must write the scene that is actually
    // loaded (currentScenePath_) in the runtime componentized format, so
    // hierarchy, ids, rotation and health survive a restart. Regression for
    // "save, restart, hierarchy lost" (SaveScene used to hardcode
    // editor_scene.json and never touched the loaded project scene).
    if (smokeMode_ && TimeRef().frameIndex == 52) {
        bool ok = entities_.size() >= 2;
        const std::string prevPath = currentScenePath_;
        if (ok) {
            NormalizeEntityIds();
            const int id0 = entities_[0].id;
            const int id1 = entities_[1].id;
            entities_[1].parentId = id0; // parent -> child
            entities_[1].rot = math::Quat{0.1f, 0.2f, 0.3f, 1.0f};
            const float hp = entities_[1].maxHp > 0.0f ? entities_[1].hp : 0.0f;
            const std::string tmpSave = GetTempDir() + "/smoke_save_roundtrip.json";
            currentScenePath_ = tmpSave;
            SaveScene();
            // Guard: the smoke must NEVER write a real scene (the user's
            // project scenes / sandbox are user data). Verify the write target
            // was actually the temp file; any deviation fails the smoke.
            if (currentScenePath_ != tmpSave) ok = false;
            currentScenePath_ = prevPath;
            LoadScene(tmpSave); // reload exactly what SaveScene wrote
            int n0 = -1;
            int n1 = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                if (entities_[i].id == id0) n0 = static_cast<int>(i);
                if (entities_[i].id == id1) n1 = static_cast<int>(i);
            }
            ok = n0 >= 0 && n1 >= 0 && entities_[static_cast<size_t>(n1)].parentId == id0 &&
                 entities_[static_cast<size_t>(n1)].rot.x == 0.1f &&
                 entities_[static_cast<size_t>(n1)].rot.y == 0.2f &&
                 entities_[static_cast<size_t>(n1)].rot.z == 0.3f;
            if (ok && hp > 0.0f)
                ok = entities_[static_cast<size_t>(n1)].hp == hp;
            if (!prevPath.empty()) LoadScene(prevPath); // restore the scene
        }
        NEON_LOG_INFO("EDITOR-SAVESCENE-SMOKE: [%s] save->load keeps hierarchy/id/rot/health",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // Play/Stop smoke: start a play at frame 60, verify it ticks, stop at
    // the last frame (119; OnUpdate never runs at 120). Kept at "Play/Stop
    // doesn't crash the editor" level; the real script/BT verification lives
    // in tests/test_game_runtime.cpp.
    if (smokeMode_ && TimeRef().frameIndex == 60) StartPlay();
    if (smokeMode_ && TimeRef().frameIndex == 90) {
        const bool ok = playActive_ && play_ && play_->Running();
        NEON_LOG_INFO("EDITOR-PLAY-SMOKE: [%s] play active (entities=%zu)",
                      ok ? "PASS" : "FAIL", ok ? play_->EntityCount() : 0u);
        if (!ok) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 119) { // last OnUpdate before exit
        const bool wasActive = playActive_ && play_;
        StopPlay();
        const bool clean = !play_ && !playActive_;
        NEON_LOG_INFO("EDITOR-PLAY-SMOKE: [%s] play stopped cleanly (was %s)",
                      clean ? "PASS" : "FAIL", wasActive ? "active" : "inactive");
        if (!wasActive || !clean) smokeFailed_ = true;
    }
}





void EditorApp::OnEvent(const platform::InputEvent& event) {
    // Input-map panel: while listening for a rebind, the next raw key wins
    // (checked before the F5/gizmo shortcuts so rebinding works while playing).
    if (!inputMapListenAction_.empty() &&
        event.type == platform::InputEvent::Type::KeyDown &&
        event.key != platform::Key::Unknown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (inputMapEdit_.SetPrimaryKey(inputMapListenAction_, event.key))
            NEON_LOG_INFO("Editor: input action '%s' -> %s", inputMapListenAction_.c_str(),
                          script::InputMap::KeyToName(event.key).c_str());
        inputMapListenAction_ = "";
        return;
    }
    // Ctrl+Z (undo) / Ctrl+Y or Ctrl+Shift+Z (redo) on the KeyDown edge only,
    // and never while ImGui owns the keyboard (e.g. typing in the name field)
    // -- same gating as the F5 play shortcut below. When the 行为树 panel
    // has focus AND its graph history has steps, undo/redo drive the BT graph;
    // otherwise they drive the scene history (an empty BT history never
    // swallows the scene shortcuts).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (Input()->IsDown(platform::Key::Control)) {
            if (event.key == platform::Key::Z) {
                if (Input()->IsDown(platform::Key::Shift)) {
                    if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                    else history_.Redo();
                } else {
                    if (btPanelFocused_ && btHistory_.CanUndo()) btHistory_.Undo();
                    else history_.Undo();
                }
                ClampSelection();
                return;
            }
            if (event.key == platform::Key::Y) {
                if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                else history_.Redo();
                ClampSelection();
                return;
            }
        }
    }
    // F5 toggles play on the KeyDown edge only (Win32 auto-repeats KeyDown
    // while held, which would otherwise oscillate Play/Stop), and never while
    // ImGui owns the keyboard (e.g. typing in a text field).
    if (event.key == platform::Key::F5) {
        if (event.type == platform::InputEvent::Type::KeyDown) {
            if (!f5Pressed_ && !gfx::ImGuiNeon_WantCaptureKeyboard()) {
                TogglePlay();
            }
            f5Pressed_ = true;
        } else if (event.type == platform::InputEvent::Type::KeyUp) {
            f5Pressed_ = false;
        }
    }
    // Delete removes the selected asset (armed here, opened inside the panel
    // on the next frame because ImGui popups need an active frame).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        event.key == platform::Key::Delete && selectedAsset_ >= 0 &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        NEON_LOG_INFO("Asset: Delete key pressed (selected=%d)", selectedAsset_);
        deleteAssetRequested_ = true;
    }
    // Tab cycles the viewport camera preset (透视 -> 顶视 -> 前视 -> ...), the
    // same list the toolbar combo exposes.
    if (event.type == platform::InputEvent::Type::KeyDown && event.key == platform::Key::Tab &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        SetViewCam(static_cast<ViewCam>((static_cast<int>(viewCam_) + 1) % 3));
    }
    if (event.type == platform::InputEvent::Type::TextInput) {
        pendingText_ += event.text;
    }
}

// --- P5-editor UX: UI-editor selection / align / snap helpers -------------

void EditorApp::UISelectNode(ui::UiNode* n) {
    uiSelection_.clear();
    if (n) uiSelection_.insert(n);
    uiSelected_ = n;
}

void EditorApp::UIToggleSelectNode(ui::UiNode* n) {
    if (!n) return;
    const auto it = uiSelection_.find(n);
    if (it != uiSelection_.end()) {
        uiSelection_.erase(it);
        if (uiSelected_ == n)
            uiSelected_ = uiSelection_.empty() ? nullptr : *uiSelection_.rbegin();
    } else {
        uiSelection_.insert(n);
        uiSelected_ = n;
    }
}

ui::UiNode* EditorApp::UICloneNode(const ui::UiNode& src) {
    auto clone = std::make_unique<ui::UiNode>();
    clone->type = src.type;
    clone->name = src.name + "_副本";
    clone->rect = src.rect;
    clone->rect.x += 8.0f;
    clone->rect.y += 8.0f;
    clone->color = src.color;
    clone->borderColor = src.borderColor;
    clone->text = src.text;
    clone->sprite = src.sprite;
    clone->fill = src.fill;
    clone->fontSize = src.fontSize;
    clone->visible = src.visible;
    clone->clipChildren = src.clipChildren;
    for (const auto& c : src.children)
        clone->children.push_back(std::unique_ptr<ui::UiNode>(UICloneNode(*c)));
    for (auto& c : clone->children) c->parent = clone.get();
    return clone.release();
}

void EditorApp::UIDuplicateSelectedNodes() {
    if (uiSelection_.empty()) return;
    std::vector<ui::UiNode*> sel(uiSelection_.begin(), uiSelection_.end());
    uiSelection_.clear();
    for (ui::UiNode* n : sel) {
        if (!n || !n->parent) continue;
        ui::UiNode* copy = UICloneNode(*n);
        n->parent->children.push_back(std::unique_ptr<ui::UiNode>(copy));
        copy->parent = n->parent;
        uiSelection_.insert(copy);
    }
    uiSelected_ = uiSelection_.empty() ? nullptr : *uiSelection_.rbegin();
    MarkUIDirty();
}

void EditorApp::UIDeleteSelectedNodes() {
    if (uiSelection_.empty()) return;
    std::vector<ui::UiNode*> sel(uiSelection_.begin(), uiSelection_.end());
    uiSelection_.clear();
    uiSelected_ = nullptr;
    for (ui::UiNode* n : sel) {
        if (!n || !n->parent) continue;
        std::vector<std::unique_ptr<ui::UiNode>>& kids = n->parent->children;
        for (auto it = kids.begin(); it != kids.end(); ++it) {
            if (it->get() == n) {
                kids.erase(it);
                break;
            }
        }
    }
    MarkUIDirty();
}

void EditorApp::UIAlignSelected(int mode) {
    if (uiSelection_.empty()) return;
    for (ui::UiNode* n : uiSelection_) {
        if (!n || !n->parent) continue;
        const math::Rect2& p = n->parent->rect;
        switch (mode) {
            case 0: n->rect.x = 0.0f; break;                     // left
            case 1: n->rect.x = (p.w - n->rect.w) * 0.5f; break; // h-center
            case 2: n->rect.x = p.w - n->rect.w; break;          // right
            case 3: n->rect.y = 0.0f; break;                     // top
            case 4: n->rect.y = (p.h - n->rect.h) * 0.5f; break; // v-center
            case 5: n->rect.y = p.h - n->rect.h; break;          // bottom
            default: break;
        }
        n->rect.x = UISnap(n->rect.x);
        n->rect.y = UISnap(n->rect.y);
    }
    MarkUIDirty();
}














std::vector<int> EditorApp::SelectedIndices() const {
    std::vector<int> out(selection_.begin(), selection_.end());
    return out;
}















void EditorApp::RequestMeshThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = meshThumbs_.find(path);
    if (it != meshThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(meshThumbQueue_.begin(), meshThumbQueue_.end(), path) ==
        meshThumbQueue_.end()) {
        meshThumbQueue_.push_back(path);
    }
}

void EditorApp::GenerateMeshThumbnails() {
    if (meshThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        meshThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    // A thumbnail is tooling, not scene geometry: never record its mesh as a
    // shadow caster for the main scene's next shadow pass.
    renderer_.SetShadowRecording(false);
    for (const std::string& path : meshThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = meshThumbs_.find(path);
        if (it != meshThumbs_.end() && it->second.mtime == m) continue; // already fresh

        // Resolve the asset's first mesh; a failed load caches a "miss" (same
        // mtime) so the panel only retries when the file actually changes.
        const std::string ext = ExtLower(path);
        gfx::Mesh mesh;
        gfx::Material mat = gfx::Material::Lit({}, gfx::Color{0.85f, 0.85f, 0.92f, 1.0f}, 16.0f);
        if (ext == ".obj") {
            mesh = assetMgr_.LoadMeshOBJ(path);
        } else if (ext == ".gltf") {
            assets::GltfAsset gltf = assetMgr_.LoadGLTF(path);
            if (!gltf.nodes.empty()) {
                mesh = gltf.nodes[0].mesh;
                mat = gltf.nodes[0].material;
            }
        }
        if (!mesh.Valid()) {
            if (it != meshThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                meshThumbs_.erase(it);
            }
            meshThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }

        // Orthographic front camera framing the mesh's bounds.
        const math::AABB& b = mesh.Bounds();
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const math::Vec3 extents = b.max - b.min;
        const float size = std::max({extents.x, extents.y, extents.z, 0.001f}) * 1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        // RGBA16F so the target carries a depth attachment (a plain RGBA8
        // target has none); the lit mesh then occludes correctly.
        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt); // sets the 96x96 viewport
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        renderer_.DrawMesh(mesh, mat, math::Mat4::Identity());
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);

        if (it != meshThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            meshThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        meshThumbs_[path] = nt;
    }
    meshThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    // Leave the backbuffer bound + the viewport at window size for the ImGui
    // pass (EndScene already composited to it).
    backend->BindDefaultTarget();
}

// Queues a material-ball sphere preview (mtime-gated, like mesh thumbnails).
void EditorApp::RequestMaterialThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = materialThumbs_.find(path);
    if (it != materialThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(materialThumbQueue_.begin(), materialThumbQueue_.end(), path) ==
        materialThumbQueue_.end()) {
        materialThumbQueue_.push_back(path);
    }
}

// Renders each queued material ball as a lit sphere (Unity/UE-style preview)
// into a small offscreen target; the ImGui pass samples it next frame.
void EditorApp::GenerateMaterialThumbnails() {
    if (materialThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        materialThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    renderer_.SetShadowRecording(false);
    for (const std::string& path : materialThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = materialThumbs_.find(path);
        if (it != materialThumbs_.end() && it->second.mtime == m) continue;

        // Expand the material asset into entity-style params, then build a
        // PBR material from them (texture slots resolve through the cache).
        SceneEntity params;
        if (!LoadMaterialParamsInto(params, path)) {
            // Cache a miss so the panel only retries when the file changes.
            if (it != materialThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                materialThumbs_.erase(it);
            }
            materialThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }
        gfx::Material mat = gfx::Material::Lit({}, params.tint, 8.0f);
        mat.metallic = params.metallic;
        mat.roughness = params.roughness;
        mat.aoStrength = params.ao;
        mat.emissiveIntensity = params.emissiveIntensity;
        if (!params.albedoTex.empty())
            mat.albedo = assetMgr_.LoadTexture(params.albedoTex).Handle();
        if (!params.mrTex.empty())
            mat.metallicRoughness = assetMgr_.LoadTexture(params.mrTex).Handle();
        if (!params.aoTex.empty())
            mat.occlusion = assetMgr_.LoadTexture(params.aoTex).Handle();
        if (!params.emissiveTex.empty())
            mat.emissive = assetMgr_.LoadTexture(params.emissiveTex).Handle();

        gfx::Mesh sphere = gfx::Mesh::CreateSphere(renderer_, 0.8f, 16, 12, "matball");
        const math::AABB& b = sphere.Bounds();
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const float size = std::max({b.max.x - b.min.x, b.max.y - b.min.y,
                                     b.max.z - b.min.z, 0.001f}) *
                           1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt);
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        renderer_.DrawMesh(sphere, mat, math::Mat4::Identity());
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);

        if (it != materialThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            materialThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        materialThumbs_[path] = nt;
    }
    materialThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    backend->BindDefaultTarget();
}

void EditorApp::PollHotReload() {
    const std::string base = projectDir_.empty() ? "." : projectDir_;

    // Scripts: only while a play runs (that is what executes scripts). A
    // changed *.lua under <projectDir>/scripts/ is applied as a play
    // restart (Stop + Start), which resets all script/entity/BT state - a safe,
    // deterministic reload for the editor. Shaders are compiled from strings
    // at init and are deliberately NOT hot-reloaded (YAGNI; see T4.8 notes).
    if (playActive_ && play_) {
        std::vector<std::string> files;
        ListScriptFiles(ScriptsDir(projectDir_), "scripts", files);
        bool scriptChanged = false;
        for (const std::string& rel : files) {
            const std::string full = base + "/" + rel;
            const uint64_t m = FileMTime(full);
            auto it = scriptMtimes_.find(full);
            if (it != scriptMtimes_.end() && it->second != m) {
                scriptChanged = true;
                break;
            }
            scriptMtimes_[full] = m;
        }
        if (scriptChanged) {
            ++hotReloadCount_;
            NEON_LOG_INFO(
                "Editor: hot reload: a script changed on disk -> restarting play "
                "(play state resets)");
            StopPlay();
            StartPlay();
        }
    }

    // Assets referenced by the editor scene: textures + file-backed meshes,
    // including the file-backed built-in mesh keys (helmet/tree resolve to
    // files via ResolveMesh). glTF is re-parsed by ResolveMesh on every call,
    // so a change only needs the mtime gate here; OBJ/textures drop through
    // the AssetManager cache.
    std::set<std::string> changedPaths;
    auto checkFile = [&](const std::string& path) {
        if (path.empty()) return;
        const uint64_t m = FileMTime(path);
        auto it = assetMtimes_.find(path);
        if (it != assetMtimes_.end() && it->second != m && m != 0) changedPaths.insert(path);
        assetMtimes_[path] = m;
    };
    for (const SceneEntity& e : entities_) {
        const std::string meshPath = MeshKeyAssetPath(e.meshKey, projectDir_);
        if (!meshPath.empty()) checkFile(meshPath);
        if (!e.shaderPath.empty()) checkFile(e.shaderPath);
        checkFile(e.albedoTex);
        checkFile(e.mrTex);
        checkFile(e.aoTex);
        checkFile(e.emissiveTex);
    }
    if (!changedPaths.empty()) {
        for (const std::string& p : changedPaths) {
            const std::string ext = ExtLower(p);
            if (ext == ".obj") {
                assetMgr_.ReloadMeshOBJ(p);
            } else if (ext == ".gltf") {
                // Drop the resolved-model cache so the next ResolveMesh
                // re-parses the updated file. (The old GPU meshes are not
                // explicitly destroyed; same as the pre-existing mesh path.)
                skinnedModelCache_.erase(p);
                gltfStaticMeshCache_.erase(p);
                gltfStaticMaterialCache_.erase(p);
            } else {
                assetMgr_.ReloadTexture(p);
            }
            NEON_LOG_INFO("Editor: hot reload: asset '%s' reloaded", p.c_str());
        }
        ++hotReloadCount_;
        // Re-resolve only the entities that reference a changed asset.
        for (SceneEntity& e : entities_) {
            const bool touches =
                changedPaths.count(MeshKeyAssetPath(e.meshKey, projectDir_)) ||
                changedPaths.count(e.shaderPath) ||
                changedPaths.count(e.albedoTex) || changedPaths.count(e.mrTex) ||
                changedPaths.count(e.aoTex) || changedPaths.count(e.emissiveTex);
            if (touches) {
                if (changedPaths.count(e.shaderPath)) ReloadEntityShader(e);
                ResolveMesh(e);
                ApplyMaterialParams(e);
            }
        }
    }

    // Asset directory watch (P1-1 import pipeline): new/changed files under
    // the project's assets dir show up in the asset panel automatically (the
    // panel still owns the actual import/copy actions).
    if (!assetDir_.empty()) {
        std::vector<AssetEntry> now;
        if (ListDirectory(assetDir_, now)) {
            std::string sig;
            for (const AssetEntry& a : now) {
                sig += a.name;
                sig += a.isDir ? "/" : "|";
            }
            if (sig != assetDirSignature_) {
                assetDirSignature_ = sig;
                RefreshAssetDir();
                NEON_LOG_INFO("Editor: asset dir watch: %zu entries", now.size());
            }
        }
    }
}

core::Status EditorApp::ExportScene() {
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Editor: export aborted: %s", rootRes.Error().c_str());
        return core::Status::Err(rootRes.Error());
    }
    core::Json root = rootRes.Value();

    std::string base = projectDir_.empty() ? "." : projectDir_;
    std::string scenesDir = base + "/scenes";
    if (!EnsureDirs(scenesDir)) {
        NEON_LOG_ERROR("Editor: cannot create export directory '%s'", scenesDir.c_str());
        return core::Status::Err("editor: cannot create export directory '" + scenesDir + "'");
    }
    std::string path = scenesDir + "/exported_scene.json";
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out(path); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: exported scene (%zu entities) -> %s", entities_.size(),
                      path.c_str());
        return core::Status::Ok(true);
    }
    NEON_LOG_ERROR("Editor: cannot write '%s'", path.c_str());
    return core::Status::Err("editor: cannot write '" + path + "'");
}

void EditorApp::LoadEditorConfig() {
    projectDir_ = ".";
    std::ifstream in("neon_editor_config.json");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        core::Json root = core::Json::Parse(ss.str(), &err);
        if (root.IsObject()) {
            if (const core::Json* p = root.Get("projectDir")) projectDir_ = p->GetString();
        }
    }
    if (projectDir_.empty()) projectDir_ = ".";
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
}

void EditorApp::SaveEditorConfig() {
    if (projectDir_.empty()) projectDir_ = ".";
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json p;
    p.type_ = core::Json::Type::String;
    p.string_ = projectDir_;
    root.object_["projectDir"] = p;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("neon_editor_config.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: config saved (project dir '%s')", projectDir_.c_str());
    } else {
        NEON_LOG_WARN("Editor: cannot write editor config");
    }
}



// Rebuilds a terrain entity's mesh from its heightmap canvas (P1-1). A blank
// canvas (no heights) becomes a flat field the brush can carve.

// P1-1 terrain brush: lowers/raises the heightmap around the ray's hit on the
// selected terrain's ground plane, then rebuilds the mesh.

// P2-6 shader hot reload: (re)compiles the entity's custom fragment shader
// against the built-in unlit vertex contract and re-binds it to the material.
// The GL backend supports custom fragments; other backends return an invalid
// handle and the material keeps its built-in shader.


// P1-1: writes a copy of the current scene as <dir>/<stem>_child.json with
// "extends" pointing at the current scene, then opens it. The child loads with
// the parent's entities underneath, so parent edits propagate and child
// same-name entities override.


// G1-3: assign a unique id to every entity missing one (id == 0), using
// max existing id + 1, so parentId references are always resolvable. Called
// after scene load/setup and before save/export.

// Stable per-parent sort of the scene tree by entity name. Root entities come
// first, then each group's children sorted case-insensitively by name,
// recursively (depth-first). Entities whose parentId points at a missing
// entity keep their relative order at the end. One undo step restores the
// whole previous order.



// Reads <dir>/game.json into `p` (title/mode/startScene) and lists the
// project's scenes/. Returns false when there is no game.json.

// Discovers every project under projects/ (a directory with a game.json) and
// keeps the active-project fields in sync with projectDir_.

// Loads every prefabs/*.json from the current project (Godot-style prefab
// templates referenced by scene entities).

// Saves the selected entity's components as prefabs/<name>.json (a component
// template other entities can instantiate).

// Expands a material-ball asset (materials/*.mat.json) into an entity's
// flattened material fields. False when the asset is missing or invalid.

// Saves the selected entity's material as a material-ball asset and links the
// entity to it (one undo step).

// Applies a material-ball asset to the selected entity (one undo step).

// Godot-style project switch: loads <dir>/game.json, enters the project's
// declared edit mode and loads its start scene (3D) or first level (2D).

// Loads the current project's start scene (3D) / level (2D): a "reload"
// entry point for the 项目 menu.

// Loads a specific scene from the current project into the 3D scene tree.

// Returns the throwaway syntax-check host matching a script file's extension
// (.js -> QuickJS, otherwise Lua), creating it lazily. Both hosts are
// validation-only: CheckSyntax never executes a chunk, so a failed check
// leaves the host fully reusable for the next file.
script::IScriptHost* EditorApp::ScriptCheckHostFor(const std::string& path) {
    const bool isJs = path.size() >= 3 &&
                      (path.compare(path.size() - 3, 3, ".js") == 0 ||
                       path.compare(path.size() - 3, 3, ".JS") == 0);
    if (isJs) {
        if (!scriptCheckHostJs_) {
            scriptCheckHostJs_ = script::CreateJsHost();
            if (scriptCheckHostJs_) scriptCheckHostJs_->Init();
        }
        return scriptCheckHostJs_.get();
    }
    if (!scriptCheckHost_) {
        scriptCheckHost_ = script::CreateLuaHost();
        if (scriptCheckHost_) scriptCheckHost_->Init();
    }
    return scriptCheckHost_.get();
}


// Saves the built-in editor's content, re-checks syntax and refreshes the
// script panel.

// Opens the file in the system's default editor (VS Code etc.).



// ---------------------------------------------------------------------------
// Editor plugin API (NeonEditor.* native bindings)
// ---------------------------------------------------------------------------







std::vector<std::string> EditorApp::PluginListDir(const std::string& dir) const {
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + "/" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (::stat((dir + "/" + name).c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) out.push_back(dir + "/" + name);
    }
    ::closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}





} // namespace neon::editor
