#include "editor.hpp"
#include "editor_util.hpp"
#include "panels/scene_panel.hpp"
#include "panels/asset_panel.hpp"
#include "panels/inspector_panel.hpp"
#include "panels/log_panel.hpp"
#include "panels/resource_panel.hpp"
#include "panels/model_preview_panel.hpp"
#include "panels/plugins_panel.hpp"
#include "panels/nav_panel.hpp"
#include "panels/debug_overlay_panel.hpp"
#include "panels/loc_panel.hpp"
#include "panels/profiler_panel.hpp"
#include "panels/input_map_panel.hpp"
#include "panels/terrain_panel.hpp"
#include "panels/tilemap_panel.hpp"
#include "panels/package_panel.hpp"
#include "panels/script_editor_panel.hpp"
#include "panels/ui_editor_panel.hpp"
#include "panels/anim_editor_panel.hpp"
#include "panels/asm_editor_panel.hpp"
#include "panels/viewport_panel.hpp"
#include "panels/bt_panel.hpp"

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
// The shared panel registry entry (title + show-flag member pointer + menu
// visibility). Lives in EditorApp so the 视图 menu and the ini persistence both
// iterate the same list instead of drifting apart (C3).
using PanelStateEntry = EditorApp::PanelDef;

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
    // Ordered to match the 视图 menu (the settings handler is order-agnostic).
    static const PanelStateEntry kPanels[] = {
        {"场景", &EditorApp::showHierarchy_},
        {"属性", &EditorApp::showInspector_},
        {"动画时间线", &EditorApp::showAnimEditor_},
        {"动画状态机", &EditorApp::showStateMachineEditor_},
        {"地形编辑", &EditorApp::showTerrain_},
        {"2D 地图", &EditorApp::showTilemap_},
        {"资产", &EditorApp::showAssets_},
        {"资源", &EditorApp::showResources_},
        {"日志", &EditorApp::showLog_},
        {"模型查看器", &EditorApp::showModelPreview_},
        {"行为树", &EditorApp::showBt_},
        {"脚本编辑器", &EditorApp::showScriptEditor_},
        {"打包", &EditorApp::showPackage_},
        {"性能", &EditorApp::showProfiler_},
        {"输入映射", &EditorApp::showInputMap_},
        {"导航", &EditorApp::showNav_},
        {"UI 编辑器", &EditorApp::showUIEditor_},
        {"本地化", &EditorApp::showLoc_},
        {"插件", &EditorApp::showPlugins_},
        {"调试覆盖层", &EditorApp::showDebugOverlay_, false},
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

// C3: the panel registry accessor shared by the 视图 menu (editor_ui.cpp) and
// this persistence handler. Points at the static array registered above.
const EditorApp::PanelDef* EditorApp::Panels() { return g_panelStateEntries; }
int EditorApp::PanelCount() { return static_cast<int>(g_panelStateCount); }

bool EditorApp::OnCreate() {
    if (disableShadows_) renderer_.SetShadowsEnabled(false);
    // G5-4: keep the runtime sceneWorld_ authoritative — every committed editor
    // mutation (Push/Undo/Redo) rebuilds it from the working model, so the World
    // always reflects the current edit state (and the play/save output is
    // generated from it via FromWorld).
    // B11: a gizmo drag pushes a mergeable transform command EVERY frame; the
    // full World rebuild (JSON build + parse + Instantiate) runs once per drag
    // instead of once per frame. The mirror stays fresh for the final pose.
    history_.onChanged = [this] {
        if (!gizmoDragActive_) SyncWorldFromEntities();
    };
    renderer_.SetBackendName(backendName_);
    renderer_.SetRenderThreadEnabled(renderThreadEnabled_);
    renderer_.SetBloomEnabled(bloomEnabled_);
    renderer_.SetMsaaEnabled(msaaEnabled_);
    renderer_.SetTonemapEnabled(tonemapEnabled_);
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Editor: renderer init failed");
        return false;
    }
    assetMgr_.Init(&renderer_);
    MountAssetVfs();

    pixelFont_ = renderer_.CreateFontFromMemory(neon::embedded::kEmbeddedFontData,
                                                neon::embedded::kEmbeddedFontSize, 24);
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
    // 面板插件化（阶段 1）：构建面板共享上下文（指针 + 回调全部指向/调用本类
    // 成员，行为与原 EditorApp 方法一致），再注册已迁移的独立面板。可见标志
    // 注入 showHierarchy_（窗口菜单勾选 + ini 持久化 + 冒烟强制开启都读写它）。
    ctx_.sceneWorld = &sceneWorld_;
    ctx_.entities = &entities_;
    ctx_.selected = &selected_;
    ctx_.selection = &selection_;
    ctx_.renderer = &renderer_;
    ctx_.assetMgr = &assetMgr_;
    ctx_.compReg = &sceneCompReg_;
    ctx_.projectDir = &projectDir_;
    ctx_.history = &history_;
    ctx_.postSsao = &postSsao_;
    ctx_.postSsaoIntensity = &postSsaoIntensity_;
    ctx_.postVolumetric = &postVolumetric_;
    ctx_.postVolumetricIntensity = &postVolumetricIntensity_;
    ctx_.postSsr = &postSsr_;
    ctx_.postSsrIntensity = &postSsrIntensity_;
    ctx_.refreshAssetDir = [this]() { RefreshAssetDir(); };
    ctx_.setSelection = [this](int index) { SetSelection(index); };
    ctx_.toggleSelection = [this](int index) { ToggleSelection(index); };
    ctx_.selectRangeTo = [this](int index) { SelectRangeTo(index); };
    ctx_.isSelected = [this](int index) { return IsSelected(index); };
    ctx_.selectedIndices = [this]() { return SelectedIndices(); };
    ctx_.clampSelection = [this]() { ClampSelection(); };
    ctx_.addEntity = [this](const std::string& meshKey) { AddEntity(meshKey); };
    ctx_.addSpriteEntity = [this](const std::string& path) { AddSpriteEntity(path); };
    ctx_.sortSceneTreeByName = [this]() { SortSceneTreeByName(); };
    ctx_.normalizeEntityIds = [this]() { NormalizeEntityIds(); };
    ctx_.savePrefab = [this](const std::string& name) { SavePrefab(name); };
    // 资产面板（Task 3）：浏览状态指针 + 资产操作回调（全部指向本类成员/方法）。
    ctx_.assetDir = &assetDir_;
    ctx_.assetEntries = &assetEntries_;
    ctx_.assetFilter = &assetFilter_;
    ctx_.assetGridView = &assetGridView_;
    ctx_.selectedAsset = &selectedAsset_;
    ctx_.deleteAssetRequested = &deleteAssetRequested_;
    ctx_.importAssetFile = [this](const std::string& src) { ImportAssetFile(src); };
    ctx_.createAssetFile = [this](const std::string& name, int kind) {
        CreateAssetFile(name, kind);
    };
    ctx_.deleteSelectedAsset = [this]() { DeleteSelectedAsset(); };
    ctx_.importAssetPath = [this](const std::string& path) { ImportAssetPath(path); };
    ctx_.inPrefabsDir = [this]() { return InPrefabsDir(); };
    ctx_.openScriptEditor = [this](const std::string& path) { OpenScriptEditor(path); };
    ctx_.openInExternalEditor = [this](const std::string& path) {
        OpenInExternalEditor(path);
    };
    // 缩略图查询 = 登记/刷新请求（幂等）+ 读缓存条目的纹理 id（0 = 未就绪）。
    ctx_.meshThumbnail = [this](const std::string& path) -> std::uint64_t {
        RequestMeshThumbnail(path);
        const auto it = meshThumbs_.find(path);
        return it != meshThumbs_.end() ? static_cast<std::uint64_t>(it->second.texId) : 0;
    };
    ctx_.materialThumbnail = [this](const std::string& path) -> std::uint64_t {
        RequestMaterialThumbnail(path);
        const auto it = materialThumbs_.find(path);
        return it != materialThumbs_.end() ? static_cast<std::uint64_t>(it->second.texId) : 0;
    };
    ctx_.nativeWindowHandle = [this]() -> void* {
        return Window() ? Window()->NativeHandle() : nullptr;
    };
    panels_.Register(std::make_unique<ScenePanel>(&showHierarchy_));
    // 可见标志过渡期注入 showAssets_（窗口菜单勾选 + ini 持久化 + 冒烟强制开启）。
    panels_.Register(std::make_unique<AssetPanel>(&showAssets_));
    // 属性面板（Task 4）：场景脏标志 + 预置体库指针 + 实体重指令回调（原面板
    // 直接调用的 EditorApp 方法——被视口/播放/加载/冒烟多处共用，故保留在本类，
    // 经回调访问，行为一致）。
    ctx_.sceneDirty = &sceneDirty_;
    ctx_.prefabLib = &prefabLib_;
    ctx_.sceneEnvironment = &sceneEnvironment_;
    ctx_.hasSceneEnvironment = &hasSceneEnvironment_;
    ctx_.sceneRenderStack = &sceneRenderStack_;
    ctx_.hasSceneRenderStack = &hasSceneRenderStack_;
    ctx_.resolveMesh = [this](SceneEntity& e) { return ResolveMesh(e); };
    ctx_.applyMaterialParams = [this](SceneEntity& e) { ApplyMaterialParams(e); };
    ctx_.materializePrefab = [this](const std::string& pfName, const math::Vec3& pos) {
        return MaterializePrefabEntity(pfName, pos);
    };
    ctx_.reloadEntityShader = [this](SceneEntity& e) { ReloadEntityShader(e); };
    ctx_.saveMaterialAsset = [this](const std::string& name) { SaveMaterialAsset(name); };
    ctx_.applyMaterialAsset = [this](const std::string& path) { ApplyMaterialAsset(path); };
    // 可见标志过渡期注入 showInspector_（窗口菜单勾选 + ini 持久化 + 冒烟强制开启）。
    panels_.Register(std::make_unique<InspectorPanel>(&showInspector_));
    // 日志面板（Task 5）：日志数据源是 core::Log 全局环形缓冲，面板私有状态
    // 已全部迁入 LogPanel，只需注入可见标志 showLog_。
    panels_.Register(std::make_unique<LogPanel>(&showLog_));
    // 资源面板（Task 6）：数据源是 AssetManager 统计与缓存枚举（经 ctx.assetMgr），
    // 面板无自有状态，只需注入可见标志 showResources_。
    panels_.Register(std::make_unique<ResourcePanel>(&showResources_));
    // 模型查看器（Task 7）：有渲染 + 外部交互（Open/Render/Tick/HandleViewportMouse），
    // 注入可见标志 + renderer + assetMgr；EditorApp 保留转发器（见下）。
    {
        auto panel = std::make_unique<ModelPreviewPanel>(&showModelPreview_, &renderer_, &assetMgr_);
        modelPreviewPanel_ = panel.get();
        panels_.Register(std::move(panel));
    }
    // 插件管理面板（Task 8）：经 ctx.pluginMgr 访问 EditorPluginManager；
    // 原生插件列表（nativePlugins_/nativePluginsDir_）已迁入面板私有状态。
    panels_.Register(std::make_unique<PluginsPanel>(&showPlugins_));
    // 导航面板（Task 9）：导航状态 NavState 被导航面板 + 调试覆盖层共用，
    // 仍由 EditorApp 持有（nav_），注入指针经 ctx.nav 共享。
    ctx_.nav = &nav_;
    panels_.Register(std::make_unique<NavPanel>(&showNav_));
    // 调试覆盖层（Task 10）：图层开关状态仍由 EditorApp 持有（视口画物理线框
    // 直接读 debugColliders_），注入指针；探针字段缓存迁入面板私有状态。
    ctx_.debugColliders = &debugColliders_;
    ctx_.debugNavMesh = &debugNavMesh_;
    ctx_.debugProbes = &debugProbes_;
    ctx_.debugAudio = &debugAudio_;
    {
        auto panel = std::make_unique<DebugOverlayPanel>(&showDebugOverlay_);
        debugOverlayPanel_ = panel.get();
        panels_.Register(std::move(panel));
    }
    // 本地化面板（Task 11）：LocState 全部迁入面板私有状态，仅注入可见标志 showLoc_。
    panels_.Register(std::make_unique<LocPanel>(&showLoc_));
    // 性能面板（Task 12）：帧时间环形缓冲 + profilerDrawn_ 仍由 EditorApp 持有
    // （冒烟测试直接读 profiler_.ms），注入指针；模拟时钟 + 播放统计经 ctx。
    ctx_.time = &TimeRef();
    ctx_.profiler = &profiler_;
    ctx_.profilerDrawn = &profilerDrawn_;
    ctx_.playActive = &playActive_;
    ctx_.playEntityCount = [this]() { return play_ ? play_->EntityCount() : 0; };
    ctx_.playBodyCount = [this]() { return play_ ? play_->PhysicsBodyCount() : 0; };
    ctx_.playBtCount = [this]() { return play_ ? play_->BehaviorTreeCount() : 0; };
    ctx_.playScriptCount = [this]() { return play_ ? play_->ScriptCount() : 0; };
    panels_.Register(std::make_unique<ProfilerPanel>(&showProfiler_));
    // 输入映射面板（Task 13）：InputMapState 仍由 EditorApp 持有（OnEvent 监听
    // 按键写回 listenAction），注入指针；Load/Save 经回调（OnCreate:481 已调 Load）。
    ctx_.inputMap = &inputMapState_;
    ctx_.loadInputMapEdit = [this]() { LoadInputMapEdit(); };
    ctx_.saveInputMapEdit = [this]() { SaveInputMapEdit(); };
    panels_.Register(std::make_unique<InputMapPanel>(&showInputMap_));
    // 世界面板（Task 14）：地形状态（视口/场景塑形共用）注入指针；重建/打包/
    // 保存配置经回调（PackageState 已迁入 PackagePanel，RunPackage 返回报告）。
    ctx_.terrain = &terrain_;
    ctx_.rebuildTerrainMesh = [this](SceneEntity& e) { RebuildTerrainMesh(e); };
    ctx_.runPackage = [this](const char* outDir, pack::PackageReport& out) { out = RunPackage(outDir); };
    ctx_.saveEditorConfig = [this]() { SaveEditorConfig(); };
    panels_.Register(std::make_unique<TerrainPanel>(&showTerrain_));
    panels_.Register(std::make_unique<TilemapPanel>(&showTilemap_));
    panels_.Register(std::make_unique<PackagePanel>(&showPackage_));
    // 脚本编辑器（Task 15）：ScriptEditorState 提升为共享结构仍由 EditorApp 持有
    //（OpenScriptFile/SaveScriptEditor/OnUpdate 断点同步共用），注入指针 + 回调；
    // dock 恢复 + 播放调试器经 ctx 访问。openScriptEditor 已在资产面板注入。
    ctx_.scriptEditor = &scriptEditor_;
    ctx_.saveScriptEditor = [this]() { SaveScriptEditor(); };
    ctx_.dockspaceId = &dockspaceId_;
    ctx_.playScriptHost = [this]() -> script::IScriptHost* {
        return play_ ? play_->ScriptHost() : nullptr;
    };
    panels_.Register(std::make_unique<ScriptEditorPanel>(&showScriptEditor_));
    // UI 编辑器（Task 16）：UiEditorState 提升为共享结构仍由 EditorApp 持有
    //（视口交互 UpdateUIEditorViewport 共用），注入指针；辅助方法经回调。
    ctx_.uiEditor = &ui_;
    ctx_.uiSelectNode = [this](ui::UiNode* n) { UISelectNode(n); };
    ctx_.uiToggleSelectNode = [this](ui::UiNode* n) { UIToggleSelectNode(n); };
    ctx_.uiDeleteSelectedNodes = [this]() { UIDeleteSelectedNodes(); };
    ctx_.uiDuplicateSelectedNodes = [this]() { UIDuplicateSelectedNodes(); };
    ctx_.uiAlignSelected = [this](int m) { UIAlignSelected(m); };
    ctx_.markUiDirty = [this]() { MarkUIDirty(); };
    panels_.Register(std::make_unique<UIEditorPanel>(&showUIEditor_));
    // 动画时间线 / 状态机编辑器（Task 17）：状态全迁入面板，仅注入可见标志。
    panels_.Register(std::make_unique<AnimEditorPanel>(&showAnimEditor_));
    panels_.Register(std::make_unique<AsmEditorPanel>(&showStateMachineEditor_));
    // 视口面板（Task 18a）：viewportRect/viewportScreenRect/相机状态仍由 EditorApp
    // 持有（OnRender/OnEvent/相机输入用），注入指针；gizmo/打开模型/添加实体经回调。
    ctx_.viewportRect = &viewportRect_;
    ctx_.viewportScreenRect = &viewportScreenRect_;
    ctx_.drawTransformGizmo = [this]() { DrawTransformGizmo(); };
    ctx_.openModelPreview = [this](const std::string& p) { OpenModelPreview(p); };
    ctx_.showModelPreview = &showModelPreview_;
    ctx_.viewCam = reinterpret_cast<int*>(&viewCam_);
    ctx_.camTarget = &camTarget_;
    ctx_.camDist = &camDist_;
    // playBodyCount 已在性能面板注入（复用）。
    panels_.Register(std::make_unique<ViewportPanel>());
    // 行为树面板（Task 18b）：btGraph_ 由 EditorApp 持有（OnCreate 播种 + 冒烟
    // 测试直接读写），注入指针；BT 文件 IO（EditorApp 方法，冒烟测试也调）与
    // 播放高亮（play_ 是 GameRuntime，面板不直接持有）经回调。
    ctx_.btGraph = &btGraph_;
    ctx_.btLoadFromFile = [this](const std::string& p) { return BtLoadFromFile(p); };
    ctx_.btSaveToFile = [this](const std::string& p) { return BtSaveToFile(p); };
    ctx_.playActiveTreePath = [this]() -> std::string {
        if (!playActive_ || !play_) return "";
        auto view = play_->World().ViewAll<scene::SceneBehaviorTree>();
        if (view.Size() == 0) return "";
        ecs::Entity e = play_->World().EntityAt<scene::SceneBehaviorTree>(0);
        return play_->ActiveTreePath(e);
    };
    {
        auto panel = std::make_unique<BtPanel>(&showBt_);
        btPanel_ = panel.get();
        panels_.Register(std::move(panel));
    }
    panels_.OpenAll(ctx_);
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
    // G5-1: take the audio backend from a native plugin when one is staged
    // under ./plugins (hot-swappable middleware DLL); otherwise fall back to
    // the platform backend (miniaudio -> WinMM -> null).
    if (std::unique_ptr<plugin::AudioBackend> pab =
            plugin::LoadNativeAudioBackend("", ".")) {
        std::unique_ptr<neon::audio::IAudioBackend,
                        std::function<void(neon::audio::IAudioBackend*)>>
            b = pab->CreateBackend();
        if (b && b->Init()) {
            pluginAudio_ = std::move(pab); // keep the plugin DLL resident
            audioBackend_ = std::move(b);
            NEON_LOG_INFO("Editor: audio backend from native plugin '%s'",
                          pluginAudio_->Name().c_str());
        }
    }
    if (!audioBackend_) {
        audioBackend_ = {neon::audio::CreatePlatformAudioBackend().release(),
                         [](neon::audio::IAudioBackend* backend) { delete backend; }};
    }
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
    pluginMgr_->Load("."); // editor plugins are GLOBAL (engine-level), not per-project
    ctx_.pluginMgr = pluginMgr_.get(); // 资产面板的插件素材源区块
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
    if (!smokeMode_ && projectDirOnStart_.empty() && projectDir_ != kDefaultProjectDir) {
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
        std::string abs = projectDirOnStart_;
        const bool isAbs = abs.size() >= 2 && abs[1] == ':' ||
                           (!abs.empty() && (abs[0] == '/' || abs[0] == '\\'));
        if (!isAbs) abs = GetWorkingDir() + "/" + abs;
        projectDir_ = abs;
        std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
        projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
        if (loadProjectOnStart_) LoadProjectScene();
        if (!sceneOnStart_.empty()) {
            // --scene <rel>: open a specific scene under the project instead of
            // the game.json startScene (showcase demos live as siblings).
            LoadScene(projectDir_ + "/" + sceneOnStart_);
        }
    }
    // Start the play LAST: LoadProjectScene/SwitchProject above stop any
    // running play, so --2d-play + --project must start after both.
    if (pvzPlayOnStart_ || playOnStart_) StartPlay();
    // --ui-editor: open the panel and load the first ui/*.ui.json directly.
    // The panel's own auto-open only runs while its dock tab is visible, which
    // a headless/CI layout cannot guarantee.
    if (uiEditorOnStart_) {
        showUIEditor_ = true;
        if (!ui_.uiDocOpen) {
            std::vector<AssetEntry> entries;
            if (ListDirectory(projectDir_ + "/assets/ui", entries)) {
                std::sort(entries.begin(), entries.end(),
                          [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; });
                for (const AssetEntry& f : entries) {
                    if (f.isDir || f.name.size() < 9 ||
                        f.name.compare(f.name.size() - 8, 8, ".ui.json") != 0)
                        continue;
                    if (ui_.uiDoc.Load(f.path)) {
                        ui_.uiDocPath = f.path;
                        ui_.uiDocOpen = true;
                        ui_.uiSelection.clear();
                        UISelectNode(ui_.uiDoc.Find("Start") ? ui_.uiDoc.Find("Start") : &ui_.uiDoc.root);
                        ui_.uiDirty = false;
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

// 模型查看器转发器（Task 7）：状态与实现已迁入 ModelPreviewPanel，这里保留薄
// 转发供外部调用点（editor_ui 右键菜单 / 调试覆盖层 / editor_viewport /
// --preview 启动参数）零改动。
void EditorApp::OpenModelPreview(const std::string& path) {
    if (modelPreviewPanel_) modelPreviewPanel_->Open(path);
}

void EditorApp::RenderModelPreviewPanel() {
    if (modelPreviewPanel_) modelPreviewPanel_->Render();
}

// 调试覆盖层转发器（Task 10）：图层绘制逻辑已迁入 DebugOverlayPanel::DrawOverlay，
// 这里保留薄转发供 editor_viewport（主场景之后画视口图层）调用。
void EditorApp::DrawDebugOverlay(const gfx::Camera& cam) {
    if (debugOverlayPanel_) debugOverlayPanel_->DrawOverlay(ctx_, cam);
}

// 打包（原 panels_script.inc 的 RunPackage，Task 14 迁入并改签名）：PackageState
// 已迁入 PackagePanel，故输出目录由调用方（面板 ctx.runPackage 回调）传入，
// 报告直接返回，面板自行设置 ran/report。
pack::PackageReport EditorApp::RunPackage(const char* outDir) {
    pack::PackConfig cfg;
    cfg.projectDir = projectDir_;
    cfg.outDir = outDir ? outDir : "";
    cfg.playerSource = "build/neon_game.exe";
    pack::PackageReport report = pack::PackProject(cfg);
    if (report.ok) {
        NEON_LOG_INFO("Editor: packaged '%s' -> %s (%zu files, %zu bytes)",
                      projectDir_.c_str(), report.packPath.c_str(),
                      report.fileCount, report.bytesWritten);
    } else {
        NEON_LOG_ERROR("Editor: package failed for '%s' (%zu errors)",
                       projectDir_.c_str(), report.errors.size());
    }
    return report;
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    panels_.Shutdown();
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
        h.scripts.push_back({"lua", "assets/scripts/hero.lua", {}});
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
    LoadScene(std::string(kDefaultProjectDir) + "/" + kSandboxSceneRel);
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
                "BENCH frame=%llu fps=%.1f avgMs=%.2f maxMs=%.2f ents=%zu draws=%u tris=%u "
                "bodies=%zu tex=%zu mesh=%zu",
                static_cast<unsigned long long>(TimeRef().frameIndex), TimeRef().Fps(),
                benchFrameMsSum_ / static_cast<float>(benchFrames_), benchFrameMsMax_,
                entities_.size(), renderer_.Stats().drawCalls, renderer_.Stats().triangles,
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
    // Advance the model-preview playhead（Task 7 已迁入 ModelPreviewPanel::Tick）。
    if (modelPreviewPanel_) modelPreviewPanel_->Tick(dt);
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
    if (showUIEditor_ && ui_.uiDocOpen) {
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
            if (scriptEditor_.breakpointsDirty && play_->ScriptHost()) {
                for (const auto& kv : scriptEditor_.breakpoints) {
                    std::vector<int> lines(kv.second.begin(), kv.second.end());
                    play_->ScriptHost()->SetScriptBreakpoints(kv.first, lines);
                }
                scriptEditor_.breakpointsDirty = false;
            }
            play_->Tick(dt);
            UpdatePlayCameraFromScript();
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
                [this](SceneEntity& e) { return ResolveMesh(e); },
                [this](SceneEntity& e) { ApplyMaterialParams(e); },
                &entities_, selected_, oldKey, ""));
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
            withScript.push_back({"lua", "assets/scripts/smoke_remove.lua", {}});
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
        if (btPanel_) btPanel_->SetSelected(r);
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
        EnsureDirs(proj + "/assets/scripts");
        {
            std::ofstream out(proj + "/assets/scripts/main.lua", std::ios::binary);
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
            newList.push_back({"lua", "assets/scripts/main.lua", vars});
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
        TouchFileMTime(hotReloadProj_ + "/assets/scripts/main.lua", 2);
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

    // Godot-style project switcher smoke: ScanProjects discovers the bundled
    // projects, SwitchProject enters the 2D project's canvas with its level
    // loaded, the 3D project loads its start scene, then we normalize back to
    // the canonical sandbox scene so the play smoke at
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
        LoadScene(std::string(kDefaultProjectDir) + "/" + kSandboxSceneRel);
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
        const std::string path =
            GetTempDir() + "/asset_proj/assets/materials/smoke_mat.mat.json";
        const auto it = materialThumbs_.find(path);
        const bool ok = it != materialThumbs_.end() &&
                        it->second.texId != ImTextureID_Invalid;
        NEON_LOG_INFO("EDITOR-MATERIAL-SMOKE: [%s] material ball sphere preview generated",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
        const std::string zhPath =
            GetTempDir() + "/asset_proj/assets/materials/\u6d4b\u8bd5\u7403.mat.json";
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
    if (!inputMapState_.listenAction.empty() &&
        event.type == platform::InputEvent::Type::KeyDown &&
        event.key != platform::Key::Unknown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (inputMapState_.edit.SetPrimaryKey(inputMapState_.listenAction, event.key))
            NEON_LOG_INFO("Editor: input action '%s' -> %s", inputMapState_.listenAction.c_str(),
                          script::InputMap::KeyToName(event.key).c_str());
        inputMapState_.listenAction = "";
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
                    if (btPanel_ && btPanel_->PanelFocused() && btPanel_->CanRedoBt())
                        btPanel_->RedoBt();
                    else history_.Redo();
                } else {
                    if (btPanel_ && btPanel_->PanelFocused() && btPanel_->CanUndoBt())
                        btPanel_->UndoBt();
                    else history_.Undo();
                }
                ClampSelection();
                return;
            }
            if (event.key == platform::Key::Y) {
                if (btPanel_ && btPanel_->PanelFocused() && btPanel_->CanRedoBt())
                    btPanel_->RedoBt();
                else history_.Redo();
                ClampSelection();
                return;
            }
        }
    }
    // While the FPS camera owns and hides the mouse, Esc is the quick exit
    // back to editor mode (F5 remains the standard Play/Stop toggle).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        event.key == platform::Key::Escape && playActive_ && fpsCameraActive_) {
        StopPlay();
        return;
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
    ui_.uiSelection.clear();
    if (n) ui_.uiSelection.insert(n);
    ui_.uiSelected = n;
}

void EditorApp::UIToggleSelectNode(ui::UiNode* n) {
    if (!n) return;
    const auto it = ui_.uiSelection.find(n);
    if (it != ui_.uiSelection.end()) {
        ui_.uiSelection.erase(it);
        if (ui_.uiSelected == n)
            ui_.uiSelected = ui_.uiSelection.empty() ? nullptr : *ui_.uiSelection.rbegin();
    } else {
        ui_.uiSelection.insert(n);
        ui_.uiSelected = n;
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
    if (ui_.uiSelection.empty()) return;
    std::vector<ui::UiNode*> sel(ui_.uiSelection.begin(), ui_.uiSelection.end());
    ui_.uiSelection.clear();
    for (ui::UiNode* n : sel) {
        if (!n || !n->parent) continue;
        ui::UiNode* copy = UICloneNode(*n);
        n->parent->children.push_back(std::unique_ptr<ui::UiNode>(copy));
        copy->parent = n->parent;
        ui_.uiSelection.insert(copy);
    }
    ui_.uiSelected = ui_.uiSelection.empty() ? nullptr : *ui_.uiSelection.rbegin();
    MarkUIDirty();
}

void EditorApp::UIDeleteSelectedNodes() {
    if (ui_.uiSelection.empty()) return;
    std::vector<ui::UiNode*> sel(ui_.uiSelection.begin(), ui_.uiSelection.end());
    ui_.uiSelection.clear();
    ui_.uiSelected = nullptr;
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
    if (ui_.uiSelection.empty()) return;
    for (ui::UiNode* n : ui_.uiSelection) {
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

















// Queues a material-ball sphere preview (mtime-gated, like mesh thumbnails).

// Renders each queued material ball as a lit sphere (Unity/UE-style preview)
// into a small offscreen target; the ImGui pass samples it next frame.


core::Status EditorApp::ExportScene() {
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Editor: export aborted: %s", rootRes.Error().c_str());
        return core::Status::Err(rootRes.Error());
    }
    core::Json root = rootRes.Value();

    std::string base = projectDir_.empty() ? "." : projectDir_;
    std::string scenesDir = base + "/assets/scenes";
    if (!EnsureDirs(scenesDir)) {
        NEON_LOG_ERROR("Editor: cannot create export directory '%s'", scenesDir.c_str());
        return core::Status::Err("editor: cannot create export directory '" + scenesDir + "'");
    }
    std::string path = scenesDir + "/exported_scene.json";
    std::string json = core::JsonWriter::WritePretty(root);
    if (std::ofstream out(path); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: exported scene (%zu entities) -> %s", entities_.size(),
                      path.c_str());
        return core::Status::Ok(true);
    }
    NEON_LOG_ERROR("Editor: cannot write '%s'", path.c_str());
    return core::Status::Err("editor: cannot write '" + path + "'");
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

// Loads every assets/prefabs/*.json from the current project (Godot-style
// prefab templates referenced by scene entities).

// Saves the selected entity's components as assets/prefabs/<name>.json (a
// component template other entities can instantiate).

// Expands a material-ball asset (assets/materials/*.mat.json) into an entity's
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
