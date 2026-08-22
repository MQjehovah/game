#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/ui/system.hpp"
#include "bt_editor.hpp"
#include "imgui.h"
#include "ImGuizmo.h"
#include "history.hpp"
#include "packager.hpp"
#include "script_panel_model.hpp"

namespace neon::editor {

// Viewport camera presets (T4.8 multi-camera viewport): the perspective orbit
// camera plus static orthographic top (down -Y) and front (down -Z) views.
enum class ViewCam { Perspective, Top, Front };

struct SceneEntity {
    std::string name;
    std::string meshKey; // "terrain" | "helmet" | "cube" | "tree" | "obj:<path>" | "gltf:<path>"
    math::Vec3 pos{};
    math::Quat rot{};
    math::Vec3 scale{1, 1, 1};
    gfx::Color tint{1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.8f;
    // Material texture slots: file paths (empty = none) resolved through the
    // AssetManager into the entity's gfx::Material texture handles below.
    std::string albedoTex;
    std::string mrTex;
    std::string aoTex;
    std::string emissiveTex;
    float ao = 1.0f;               // AO strength (0 = ignore AO map, 1 = full)
    float emissiveIntensity = 1.0f;
    // Health (mirrors the built-in `health` component): maxHp <= 0 means the
    // entity tracks no health. Used by the playtest for the hero + combat mobs.
    float hp = 0.0f;
    float maxHp = 0.0f;
    // Script component (mirrors scene::SceneScript, T4.5): backend/path/vars.
    // An empty scriptPath means no script is attached. scriptVars is a JSON
    // object (or null when absent) written into the exported script component.
    std::string scriptBackend;
    std::string scriptPath;
    core::Json scriptVars;
    gfx::Mesh mesh;
    gfx::Material material;
};

struct AssetEntry {
    std::string name;
    std::string path; // absolute
    uint64_t size = 0;
    bool isDir = false;
};

class EditorApp : public core::Application {
public:
    bool OnCreate() override;
    void OnShutdown() override;
    void OnUpdate(float dt) override;
    void OnRender() override;
    void OnEvent(const platform::InputEvent& event) override;

    void SetSmokeMode(bool v) { smokeMode_ = v; }
    void SetDisableShadows(bool v) { disableShadows_ = v; }
    void SetBloomEnabled(bool v) { bloomEnabled_ = v; }
    void SetHotReload(bool v) { hotReload_ = v; }
    void SetBackendName(const std::string& name) { backendName_ = name; }
    bool SmokeFailed() const { return smokeFailed_; }
    void RequestScreenshot(const std::string& path, uint64_t frame) {
        screenshotPath_ = path;
        screenshotFrame_ = frame;
    }

private:
    void SetupScene();
    void BuildCustomUIDemo();
    void InitToolPanels();
    void BuildImGuiUI();
    void BuildScenePanel();
    void BuildAssetPanel();
    void BuildResourcePanel();
    void BuildInspectorPanel();
    void BuildLogPanel();
    void BuildViewportPanel();
    void BuildProfilerPanel();
    void DrawPlaytestHUD();
    void DrawTransformGizmo();
    void RunGizmoDragSim();
    void ApplyMaterialParams(SceneEntity& e);
    void ClampSelection();
    bool ResolveMesh(SceneEntity& e);
    // Reassign the selected entity index. Also invalidates the script panel's
    // index-keyed sync cache (scriptSyncEntity_), so a mutation that shifts or
    // reappoints the selection can never leave the 脚本 panel showing a stale
    // entity's script (the panel re-syncs on the next frame it runs).
    void SetSelection(int index);
    void RefreshAssetDir();
    void ImportAssetPath(const std::string& path);
    void ImportSelectedAsset();
    void UpdateViewport(float dt);
    void SaveScene();
    void LoadScene(const std::string& path);
    void AddEntity(const std::string& meshKey);
    core::Status ExportScene();
    void LoadEditorConfig();
    void SaveEditorConfig();
    void RunUISmokeTest();

    // In-editor playtest (F5): a GameRuntime snapshot of the editor scene runs
    // in the viewport while the editor scene stays untouched.
    void TogglePlaytest();
    void StartPlaytest();
    void StopPlaytest();
    core::Result<core::Json> BuildPlaySceneJson();
    // Active viewport camera: the perspective orbit (透视) or one of the
    // orthographic presets (顶视 down -Y / 前视 down -Z). Every consumer
    // (renderer, gizmo, picking) uses this so the tool always matches the view.
    gfx::Camera ActiveCamera() const;

    // T4.8 asset thumbnails: a mesh asset (OBJ/glTF) selected in the asset
    // panel is rendered into a small offscreen target once per path+mtime;
    // the resulting texture is shown inline. RequestMeshThumbnail queues the
    // path; GenerateMeshThumbnails (called inside the frame's OnRender) does
    // the GPU work so the panel displays the image on the next frame.
    void RequestMeshThumbnail(const std::string& path);
    void GenerateMeshThumbnails();

    // T4.8 hot reload (--hot / toolbar toggle, off by default): polls the
    // playtest's scripts and the scene's referenced assets (throttled every 30
    // frames). A changed script restarts the playtest (Stop + Start = state
    // reset); a changed texture/OBJ is re-read through the AssetManager and
    // the owning entities re-resolved. Shaders are compiled from strings at
    // init and are NOT hot-reloaded (documented; see PollHotReload).
    void PollHotReload();

    // Behavior tree editor (T4.4): docked 行为树 panel with a node palette,
    // a drag canvas, link creation, param editing, save/load of .bt.json and a
    // playtest debug highlight driven by bt::Context::activePath.
    void BuildBtPanel();
    void BuildBtToolbar();
    void BuildBtPalette();
    void BuildBtCanvas();
    void BuildBtParams();
    void BtNewTree();
    bool BtSaveToFile(const std::string& path);
    bool BtLoadFromFile(const std::string& path);
    void BtPushSnapshot(const btgraph::BtGraph& before);
    void BtUpdatePlaytestHighlight();
    void BtRefreshBehaviorFiles();
    std::string BtBehaviorsDir() const;
    // Canvas mouse handling, extracted so the smoke can drive the real link
    // path: `cm` is a canvas-space point, ctrl/shift carry the modifier state.
    void BtCanvasClick(const math::Vec2& cm, bool ctrl, bool shift);
    void BtParamNumber(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamString(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamBool(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamJson(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);

    // Script panel (T4.5): docked 脚本 panel listing the project's scripts/
    // with per-file syntax checks, plus attach/configure/detach for the
    // selected entity. RefreshScriptChecks re-scans <projectDir>/scripts/ and
    // re-runs CheckSyntax on each file (used by the panel and the smoke).
    void BuildScriptPanel();
    void RefreshScriptChecks();

    // Package panel (T4.6): docked 打包 panel with project/out dir inputs, a
    // one-click 打包 button and the last PackageReport rendered.
    void BuildPackagePanel();
    void RunPackage();

    gfx::Renderer renderer_;
    assets::AssetManager assetMgr_;
    ui::UIManager ui_;
    gfx::Font pixelFont_;
    gfx::Font cjkFont_;

    std::vector<SceneEntity> entities_;
    int selected_ = -1;
    bool playtestActive_ = false;
    std::unique_ptr<scene::GameRuntime> playtest_; // non-null while playtesting
    bool f5Pressed_ = false; // edge-trigger: Win32 repeats KeyDown while held

    // Undo/redo command stack. Every scene mutation (entity add/delete/
    // duplicate/reorder, transform gizmo + inspector edits, material/name
    // properties) is routed through it instead of mutating entities_ directly.
    HistoryManager history_;

    // Project directory: exported scenes are written to <projectDir>/scenes/.
    std::string projectDir_{"."};
    char projectDirBuf_[4096]{};

    float yaw_ = 0.7f;
    float pitch_ = 0.35f;
    float camDist_ = 14.0f;
    math::Vec3 camTarget_{0, 1.2f, 0};

    // Multi-camera viewport (T4.8): the active preset plus the ortho zoom
    // level (half-height of the ortho frustum in world units).
    ViewCam viewCam_ = ViewCam::Perspective;
    float orthoSize_ = 16.0f;

    // Hot reload (T4.8): off by default (--hot flag / toolbar toggle). The
    // throttled poll compares recorded mtimes against the disk; scriptMtimes_
    // gates the playtest restart, assetMtimes_ the texture/mesh reload.
    bool hotReload_ = false;
    std::map<std::string, uint64_t> scriptMtimes_;
    std::map<std::string, uint64_t> assetMtimes_;
    uint64_t hotReloadFrame_ = 0;
    int hotReloadCount_ = 0; // smoke: number of reloads performed

    // Profiler panel (T4.8): a rolling frame-time buffer drawn with the
    // ImGui plot API plus per-frame renderer/ECS/physics/BT/memory stats.
    bool showProfiler_ = false;
    bool profilerDrawn_ = false; // smoke: the panel rendered its stats
    static constexpr int kProfilerSamples = 180;
    std::array<float, kProfilerSamples> profilerMs_{};
    int profilerMsHead_ = 0;

    // Asset thumbnail cache (T4.8): per-path GPU render target (kept alive so
    // its color texture stays sampleable) + the ImGui texture id, keyed by
    // path+mtime. meshThumbQueue_ holds paths awaiting the next frame's render.
    struct MeshThumb {
        gfx::RenderTargetHandle rt;
        gfx::TextureHandle texHandle;
        ImTextureID texId = ImTextureID_Invalid;
        uint64_t mtime = 0;
    };
    std::map<std::string, MeshThumb> meshThumbs_;
    std::vector<std::string> meshThumbQueue_;

    // Smoke instrumentation (T4.8): the camera used by the last scene render,
    // its scene draw-call count, and the temp-project paths for the hot-reload
    // script restart check.
    bool lastRenderCamOrtho_ = false;
    uint32_t smokeDrawCalls_ = 0;
    uint64_t lastRenderTick_ = 0; // tick processed by the most recent OnRender
    bool thumbSmokeDone_ = false; // T4.8 smoke: offscreen mesh thumbnail verified
    bool camSmokeDone_ = false;   // T4.8 smoke: ortho viewport render verified
    std::string smokeThumbPath_;
    std::string hotReloadProj_;
    std::string prevProjectDir_;

    bool smokeMode_ = false;
    bool smokeFailed_ = false;
    bool disableShadows_ = false;
    bool bloomEnabled_ = true;
    std::string backendName_ = "gl";
    std::string screenshotPath_;
    uint64_t screenshotFrame_ = 0;

    // ImGui tool UI state.
    std::string pendingText_; // UTF-8 characters queued for ImGui this frame
    bool showHierarchy_ = true;
    bool showInspector_ = true;
    bool showAssets_ = false;
    bool showResources_ = false;
    // Log panel visible by default so the bottom dock node is never empty
    // (an all-hidden bottom node collapses into the viewport and the full-window
    // 3D scene bleeds down into where the panels should be).
    bool showLog_ = true;
    bool showCustomUIDemo_ = false;
    bool showImGuiDemo_ = false;

    // Tool panel state.
    std::vector<AssetEntry> assetEntries_;
    std::string assetDir_;
    int assetFilter_ = 0; // 0 all, 1 models, 2 textures, 3 scripts
    int selectedAsset_ = -1;
    gfx::Texture previewTexture_;
    ImTextureID previewTexId_ = ImTextureID_Invalid;
    math::Rect2 viewportRect_{244, 58, 792, 640};
    // The 视口 window's rect in SCREEN pixels (set by BuildViewportPanel); the
    // 3D pass is scissored to it so the scene never bleeds into the dock area.
    math::Rect2 viewportScreenRect_{0, 0, 0, 0};
    std::vector<core::LogEntry> logEntries_;
    int logFilter_ = 0; // 0 all, 1 info+, 2 warn+, 3 error
    bool logAutoScroll_ = true;

    // Transform gizmo (ImGuizmo) state for the viewport.
    ImGuizmo::OPERATION gizmoOp_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode_ = ImGuizmo::WORLD;
    bool gizmoDrawn_ = false;    // set the first time the gizmo renders (smoke)
    bool gizmoBeginFrame_ = false; // set every frame ImGuizmo::BeginFrame runs (smoke)
    bool gizmoAltWindowSet_ = false; // set every frame the hover window is bound (smoke)
    bool gizmoDragActive_ = false;   // ImGuizmo::IsUsing() after the last Manipulate
    bool gizmoDragSimulated_ = false; // the smoke frame's synthetic drag ran
    float gizmoRect_[4] = {0, 0, 0, 0}; // rect passed to ImGuizmo::SetRect (smoke)
    // True while a gizmo drag is producing write-back deltas; the drag-end seal
    // uses it to mark the finished drag command so the next drag opens its own
    // undo step.
    bool gizmoDragOriginValid_ = false;

    // Custom UI demo widget handles (engine widget system).
    ui::TreeView* demoTree_ = nullptr;
    ui::ComboBox* demoCombo_ = nullptr;
    ui::Button* demoAddButton_ = nullptr;
    int demoAddClicks_ = 0;
    int demoComboChanged_ = -1;

    // Behavior tree editor (T4.4) state.
    bool showBt_ = false;
    bool btPanelFocused_ = false; // undo/redo routing: BT panel owns Ctrl+Z while focused
    btgraph::BtGraph btGraph_;
    HistoryManager btHistory_;
    std::string btFileName_ = "behavior";
    char btFileNameBuf_[256]{};
    std::string btSelected_;   // selected canvas node id
    std::string btPendingType_; // armed palette node type (click canvas to place)
    std::string btActivePath_;  // playtest highlight: tree-path id of the running node
    std::vector<std::string> btBehaviorFiles_;
    uint64_t btFilesRefreshFrame_ = 0; // throttle: refresh behaviors/ listing periodically
    bool btCanvasDrawn_ = false; // smoke: the BT canvas emitted geometry this frame
    // Canvas drag state.
    std::string btDragNode_;
    math::Vec2 btDragStart_{0.f, 0.f};
    math::Vec2 btNodeStartPos_{0.f, 0.f};
    bool btDragging_ = false;
    // Graph snapshot captured when a node drag began, pushed as one undo step
    // on release (only when the node actually moved).
    btgraph::BtGraph btGraphBeforeDrag_;
    bool btHasGraphBeforeDrag_ = false;
    // Per-param drag origin: args snapshot captured when a slider drag began,
    // so the undo step reverts to the pre-drag value (one drag = one undo step).
    std::map<std::string, btgraph::BtGraph> btArgDragOrigin_;

    // Script panel (T4.5) state.
    bool showScripts_ = false;
    std::unique_ptr<script::IScriptHost> scriptCheckHost_; // throwaway host for syntax checks
    std::vector<std::string> scriptFiles_;                 // project-relative "scripts/*.lua"
    std::vector<ScriptCheckResult> scriptChecks_;          // parallel: per-file check results
    uint64_t scriptRefreshFrame_ = 0; // throttle: refresh scripts/ listing + checks
    int scriptAttachIndex_ = -1;      // dropdown/list selection into scriptFiles_
    int scriptSyncEntity_ = -1;       // entity whose vars the buffer currently shows
    char scriptVarsBuf_[32768]{};     // raw JSON vars editor (32 KB; truncation is detected)
    std::string scriptVarsError_;     // last vars-parse / truncation message (empty when valid)

    // Package panel (T4.6) state.
    bool showPackage_ = false;
    char packOutDirBuf_[4096]{};      // output dir for the pack ("" = none yet)
    pack::PackageReport packReport_;  // last run's report (rendered by the panel)
    bool packRan_ = false;            // the 打包 button ran at least once
};

} // namespace neon::editor
