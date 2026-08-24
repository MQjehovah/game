#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/audio/audio.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/neon.hpp"
#include "neon/scene/component_schema.hpp"
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
    std::string parent; // scene-tree parent by entity name ("" = root)
    std::string prefab; // prefabs/<name>.json template reference ("" = none)
    // Node type (P1-1): "Node" | "MeshInstance3D" | "Camera3D" | "CharacterBody"
    // | "Sprite" | "Light3D" | "" (auto-derived from meshKey/sprite).
    std::string nodeType;
    float cameraFov = 60.0f;  // Camera3D type only (degrees)
    bool cameraOrtho = false; // Camera3D type only
    std::string meshKey; // "terrain" | "helmet" | "cube" | "tree" | "obj:<path>" | "gltf:<path>"
    // P1-1 terrain editing: heightmap canvas for meshKey "terrain".
    std::vector<float> terrainHeights_;
    int terrainSegments_ = 48;
    float terrainSize_ = 60.0f;
    float terrainHeightScale_ = 1.0f;
    math::Vec3 pos{};
    math::Quat rot{};
    math::Vec3 scale{1, 1, 1};
    float zOrder = 0.0f;  // P2-3: 2D sprite draw order (lower draws first)
    gfx::Color tint{1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.8f;
    // Material texture slots: file paths (empty = none) resolved through the
    // AssetManager into the entity's gfx::Material texture handles below.
    std::string albedoTex;
    std::string mrTex;
    std::string aoTex;
    std::string emissiveTex;
    // Custom fragment shader file (P2-6 shader hot reload): compiled against
    // the built-in unlit vertex contract (vUV/vColor + uTex). Empty = the
    // material's built-in shader.
    std::string shaderPath;
    gfx::Shader customShader;  // editor-side compiled handle
    float ao = 1.0f;               // AO strength (0 = ignore AO map, 1 = full)
    float emissiveIntensity = 1.0f;
    // Material asset reference (materials/<name>.mat.json): when set, the
    // entity's material params come from that asset (material "ball" like
    // Unity/Godot). LoadScene expands it into the flattened fields below; the
    // export writes both the reference and the expanded params.
    std::string materialRef;
    // 2D sprite: non-empty spriteTex turns the entity into an image quad
    // (XY plane facing the front-ortho camera) rendered with an unlit texture
    // material. Display size comes from `scale`; flips mirror the quad.
    std::string spriteTex;
    bool spriteFlipX = false;
    bool spriteFlipY = false;
    gfx::Mesh spriteMesh;       // unit XY quad (faces the front camera)
    gfx::Material spriteMaterial; // unlit texture material
    // Health (mirrors the built-in `health` component): maxHp <= 0 means the
    // entity tracks no health. Used by the playtest for the hero + combat mobs.
    float hp = 0.0f;
    float maxHp = 0.0f;
    // Script components: one flat list, no "primary" concept. Every mounted
    // script is an equal entry (backend/path/vars) and multiple scripts are
    // allowed; the runtime attaches each in order. Serialized as
    // "scripts": [{backend,path,vars}, ...] (legacy single "script" and flat
    // scriptPath/scriptVars fields are still read back for old scenes).
    std::vector<SceneScriptFields> scripts;
    // Non-flattened component data (Godot-style): every component of the
    // entity that isn't one of the built-in flattened fields above. Kept so
    // the inspector can edit arbitrary components (schema-driven) and project
    // scenes round-trip without data loss. 2D layout components (plant/zombie)
    // also live here; the 2D canvas mirrors them.
    std::map<std::string, core::Json> extraComponents;
    gfx::Mesh mesh;
    gfx::Material material;
};

struct AssetEntry {
    std::string name;
    std::string path; // absolute
    uint64_t size = 0;
    bool isDir = false;
};

// One discovered project (a directory with a game.json), Godot-style. The
// editor can switch projects from the toolbar, load any of its scenes or 2D
// levels, and playtest the result.
struct EditorProject {
    std::string name;       // game.json "title" (fallback: directory name)
    std::string dir;        // "projects/pvz" or "." for the default sandbox
    std::string startScene; // game.json "startScene" (project-relative)
    std::string mode = "3d"; // game.json "editor.mode": "2d" | "3d" (default 3d)
    std::vector<std::string> scenes; // scenes/*.json (project-relative)
};

// Shared UTF-8 directory listing (editor project scanner + asset panel).
bool ListDirectory(const std::string& dir, std::vector<AssetEntry>& out);
// Deletes a file or directory tree (recycle bin on Windows).
bool DeletePathRecursive(const std::string& path);

class EditorApp : public core::Application {
public:
    friend class EditMeshKeyCommand;

    bool OnCreate() override;
    void OnShutdown() override;
    void OnUpdate(float dt) override;
    void OnRender() override;
    void OnEvent(const platform::InputEvent& event) override;

    void SetSmokeMode(bool v) { smokeMode_ = v; }
    void SetDisableShadows(bool v) { disableShadows_ = v; }
    void SetBloomEnabled(bool v) { bloomEnabled_ = v; }
    void SetMsaaEnabled(bool v) { msaaEnabled_ = v; }
    void SetTonemapEnabled(bool v) { tonemapEnabled_ = v; }
    void SetBenchMode(bool v) { benchMode_ = v; }
    void SetHotReload(bool v) { hotReload_ = v; }
    // Godot/Unity-style view lock: 2D is the front-ortho camera, 3D is the
    // perspective camera. The project/scene/content never changes - only the
    // camera that views it.
    void Set2DMode(bool v) {
        editMode_ = v ? EditMode::Scene2D : EditMode::Scene3D;
        viewCam_ = v ? ViewCam::Front : ViewCam::Perspective;
    }
    void SetPvzPlaytestOnStart(bool v) { pvzPlaytestOnStart_ = v; }
    void SetProjectOnStart(const std::string& dir, bool loadScene) {
        projectDirOnStart_ = dir;
        loadProjectOnStart_ = loadScene;
    }
    void SetBackendName(const std::string& name) { backendName_ = name; }
    bool SmokeFailed() const { return smokeFailed_; }
    void RequestScreenshot(const std::string& path, uint64_t frame) {
        screenshotPath_ = path;
        screenshotFrame_ = frame;
    }

private:
    void SetupScene();
    void InitToolPanels();
    void BuildImGuiUI();
    void BuildScenePanel();
    void BuildAssetPanel();
    void BuildResourcePanel();
    void BuildInspectorPanel();
    void BuildLogPanel();
    void BuildViewportPanel();
    void BuildNavPanel();
    void BuildUIEditorPanel();
    // UI editor viewport input: click selects a node, drag moves it, corner
    // handles resize it (design-space coordinates).
    void UpdateUIEditorViewport();
    // Marks the open UI document dirty and, when it has a real file path,
    // saves it immediately so edits survive closing the panel / the editor
    // (untitled docs wait for the explicit 保存 button).
    void MarkUIDirty();
    void BuildLocPanel();
    void BuildProfilerPanel();
    void BuildInputMapPanel();
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
    std::string assetDirSignature_;  // P1-1: cached asset-dir listing signature
    // Asset panel actions: copy a file into the current asset dir and create
    // a new asset (dir / lua / json / empty text). Both refresh the listing.
    void ImportAssetFile(const std::string& srcPath);
    void CreateAssetFile(const std::string& name, int kind);
    // Deletes the selected asset (file or directory, recursive). Windows uses
    // the recycle bin (undo-able); POSIX removes recursively after confirm.
    void DeleteSelectedAsset();
    // Material-ball assets (Unity .mat / Godot Material style): save the
    // selected entity's material as materials/<name>.mat.json and apply a
    // material asset to the selected entity (both through undo).
    void SaveMaterialAsset(const std::string& name);
    void ApplyMaterialAsset(const std::string& path);
    // Expands a material asset into an entity's flattened material fields
    // (used by ApplyMaterialAsset and scene loading). False when missing/invalid.
    bool LoadMaterialParamsInto(SceneEntity& e, const std::string& path);
    void ImportAssetPath(const std::string& path);
    void ImportSelectedAsset();
    void UpdateViewport(float dt);
    // Marks the front-ortho camera's visible rect (the "camera border" Unity
    // shows for a 2D/ortho camera): a cyan rect on the plane the camera looks
    // at, so the user can see exactly what the locked camera frames.
    void DrawCameraFrame();
    // The active viewport dock rect (screen pixels) and its aspect ratio; the
    // 3D camera projection and gizmo use these so the scene fits the panel.
    const math::Rect2& ViewportRect() const { return viewportScreenRect_; }
    float ViewportAspect() const {
        const math::Rect2& vp = viewportScreenRect_;
        if (vp.w > 0.0f && vp.h > 0.0f) return vp.w / vp.h;
        return static_cast<float>(renderer_.ScreenWidth()) /
               static_cast<float>(renderer_.ScreenHeight());
    }
    void SaveScene();
    void LoadScene(const std::string& path);
    // Project switcher (Godot-style): ScanProjects discovers the projects/
    // folders; SwitchProject loads a project's game.json, its scene/level
    // lists and enters the declared edit mode (2d canvas or 3D scene tree).
    void ScanProjects();
    void SwitchProject(const std::string& dir);
    // Loads every prefabs/*.json from the current project into prefabLib_.
    void LoadPrefabLibrary();
    // Saves the selected entity's components as prefabs/<name>.json.
    void SavePrefab(const std::string& name);
    // Reads <dir>/game.json + scene/level lists into `p`; false if no game.json.
    bool ReadProjectMeta(EditorProject& p);
    // Loads the current project's start scene (3D projects) / level (2D).
    void LoadProjectScene();
    // Loads a specific scene from the current project (scenes/*.json).
    void LoadProjectScene(const std::string& rel);
    void AddEntity(const std::string& meshKey);
    // Adds a 2D sprite entity for a texture asset (spriteTex + unlit quad).
    void AddSpriteEntity(const std::string& texPath);
    core::Status ExportScene();
    void LoadEditorConfig();
    void SaveEditorConfig();
    void RunUISmokeTest();
    // 2D mode is a camera view (front-ortho), not a separate canvas: the
    // project, scene and content stay identical in 2D and 3D. 2D scenes can
    // still carry plant/zombie components; LoadScene parses them into the
    // vectors below so playtest/level data stays scene-driven.
    enum class EditMode { Scene3D, Scene2D };
    EditMode editMode_ = EditMode::Scene3D;
    // Sets the active viewport camera and keeps the edit mode in sync with it
    // (2D canvas editing only exists in the front-ortho view; perspective and
    // top are 3D-mode cameras). Used by the toolbar combo, Tab and toolbar
    // 2D/3D toggle so the two can never disagree.
    void SetViewCam(ViewCam v) {
        viewCam_ = v;
        editMode_ = (v == ViewCam::Front) ? EditMode::Scene2D : EditMode::Scene3D;
    }
    struct Pvz2DCell {
        int row = 0;
        int col = 0;
        int type = 0; // 0 sunflower, 1 peashooter, 2 wallnut, 3 snowpea, 4 cherry
    };
    struct PvzZombieSpawn {
        int row = 0;
        float delay = 0.0f;
        int type = 0; // 0 basic, 1 cone, 2 bucket
    };
    std::vector<Pvz2DCell> pvzPlants_;
    std::vector<PvzZombieSpawn> pvzZombies_;
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
    // Material-ball previews (Unity/UE-style): render a lit sphere with the
    // material asset's params into a small offscreen target, cached by mtime.
    void RequestMaterialThumbnail(const std::string& path);
    void GenerateMaterialThumbnails();

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
    // Script editor (Godot-style built-in): open/save a .lua with live syntax
    // check, plus a one-click external-editor binding (system default).
    void OpenScriptEditor(const std::string& path);
    void SaveScriptEditor();
    void BuildScriptEditorPanel();
    void BuildAnimEditorPanel();
    void BuildTerrainPanel();
    void SaveSceneAsChild();
    void ReloadEntityShader(SceneEntity& e);
    void PaintTerrain(const math::Ray& ray);
    void RebuildTerrainMesh(SceneEntity& e);
    void OpenInExternalEditor(const std::string& path);

    // Package panel (T4.6): docked 打包 panel with project/out dir inputs, a
    // one-click 打包 button and the last PackageReport rendered.
    void BuildPackagePanel();
    void RunPackage();

    gfx::Renderer renderer_;
    // Playtest audio: procedural SoundFx synthesized per PlaySfx(name) and
    // played through the platform backend (miniaudio / WinMM / null).
    std::unique_ptr<neon::audio::IAudioBackend> audioBackend_;
    assets::AssetManager assetMgr_;
    gfx::Font pixelFont_;
    gfx::Font cjkFont_;

    std::vector<SceneEntity> entities_;
    int selected_ = -1;
    bool playtestActive_ = false;
    std::unique_ptr<scene::GameRuntime> playtest_; // non-null while playtesting
    bool pvzPlaytestOnStart_ = false; // --2d-play: auto-start the playtest
    std::string projectDirOnStart_;    // --project: open this project
    bool loadProjectOnStart_ = false;  // --project also loads its start scene
    // Godot-style input mapping panel: edit project input.json actions.
    bool showInputMap_ = false;
    script::InputMap inputMapEdit_;
    std::string inputMapListenAction_; // "listening" action, "" = idle
    void LoadInputMapEdit();
    void SaveInputMapEdit();
    bool f5Pressed_ = false; // edge-trigger: Win32 repeats KeyDown while held

    // Undo/redo command stack. Every scene mutation (entity add/delete/
    // duplicate/reorder, transform gizmo + inspector edits, material/name
    // properties) is routed through it instead of mutating entities_ directly.
    HistoryManager history_;

    // Project directory: exported scenes are written to <projectDir>/scenes/.
    std::string projectDir_{"."};
    char projectDirBuf_[4096]{};
    // Godot-style project switcher state (ScanProjects / SwitchProject).
    std::vector<EditorProject> projects_;
    int projectSel_ = -1;
    std::string projectName_;        // current game.json title ("" = sandbox)
    std::string projectMode_ = "3d"; // "2d" | "3d"
    std::string projectStartScene_;  // current game.json startScene (relative)
    std::vector<std::string> projectScenes_; // current project scene files
    std::string currentSceneName_; // scene picker label (loaded scene file)
    scene::PrefabLibrary prefabLib_; // current project's prefab templates
    std::vector<std::string> projectPrefabs_; // prefab names (sorted, for UI)
    // The parsed root of the scene currently in the editor + its file path.
    // 2D levels are scene entities (plant/zombie components); the scene file
    // is the single source of truth for both the editor and the runtime.
    core::Json currentSceneRoot_;
    std::string currentScenePath_;
    std::string sceneExtends_; // P1-1: parent scene path ("" = no inheritance)
    bool sceneDirty_ = false;  // scene edited since last save (terrain brush etc.)
    bool cameraFollowSelected_ = false; // P1-1: view through the selected Camera3D
    // P1-1 terrain brush state.
    bool terrainPaintMode_ = false;
    float terrainBrushRadius_ = 5.0f;
    float terrainBrushStrength_ = 0.12f;
    bool terrainRaise_ = true;

    float yaw_ = 0.7f;
    float pitch_ = 0.35f;
    float camDist_ = 14.0f;
    math::Vec3 camTarget_{0, 1.2f, 0};

    // Multi-camera viewport (T4.8): the active preset plus the ortho zoom
    // level (half-height of the ortho frustum in world units).
    ViewCam viewCam_ = ViewCam::Perspective;
    float orthoSize_ = 16.0f;
    // 2D views auto-fit the 1280x720 design space to the viewport height
    // (1 world unit = 1 design pixel) until the user explicitly zooms/pans;
    // then the user's framing wins.
    bool cameraUserAdjusted_ = false;

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
    std::map<std::string, MeshThumb> materialThumbs_;
    std::vector<std::string> materialThumbQueue_;

    // Smoke instrumentation (T4.8): the camera used by the last scene render,
    // its scene draw-call count, and the temp-project paths for the hot-reload
    // script restart check.
    bool lastRenderCamOrtho_ = false;
    uint32_t smokeDrawCalls_ = 0;
    uint64_t lastRenderTick_ = 0; // tick processed by the most recent OnRender
    bool thumbSmokeDone_ = false; // T4.8 smoke: offscreen mesh thumbnail verified
    bool camSmokeDone_ = false;   // T4.8 smoke: ortho viewport render verified
    // Smoke: a real click on a component's 移除 button (frame 33 captures the
    // 网格 remove button's action (the command the button pushes) through the
    // undo stack. Mouse-position tests are unreliable here: the real cursor
    // overrides synthetic input every frame.
    bool smokeRemoveActionDone_ = false;
    std::string smokeThumbPath_;
    std::string hotReloadProj_;
    std::string prevProjectDir_;

    bool smokeMode_ = false;
    bool smokeFailed_ = false;
    bool disableShadows_ = false;
    bool bloomEnabled_ = true;
    bool msaaEnabled_ = true;
    bool tonemapEnabled_ = true;
    bool benchMode_ = false;  // P2-6: per-interval frame-time/draw logs + summary
    uint64_t benchFrames_ = 0;
    float benchFrameMsSum_ = 0.0f;
    float benchFrameMsMax_ = 0.0f;
    uint64_t benchLastLogFrame_ = 0;
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
    bool showImGuiDemo_ = false;
    bool showNav_ = false;
    // Data-driven UI editor (ui/*.ui.json): edit a UI document tree and
    // preview it in the viewport. Opens via 视图 → UI 编辑器.
    bool showUIEditor_ = false;
    std::vector<std::string> uiFiles_; // ui/*.ui.json in the active project
    std::string uiDocPath_;            // absolute path of the open document
    ui::UiDocument uiDoc_;             // document being edited
    bool uiDocOpen_ = false;           // a document is loaded/created
    ui::UiNode* uiSelected_ = nullptr; // selected node (owned by uiDoc_)
    bool uiDirty_ = false;
    bool uiDragging_ = false;
    int uiResizeHandle_ = -1;          // -1 none, 0..3 corner handles
    math::Vec2 uiDragPos_{0.0f, 0.0f}; // mouse in design space
    bool showLoc_ = false;

    // Localization editor: merged string tables from <project>/locales/*.json
    // plus the active language for the preview.
    core::Localization locEdit_;
    std::string locLanguage_ = "zh";
    std::string locPath_; // last loaded/saved locales file

    // Navigation tool (A* on a 2D walkability grid): asset path, grid, and
    // the current start/goal/path for viewport visualization.
    std::string navAssetPath_; // project nav/<name>.json ("" = unsaved)
    nav::NavGrid navGrid_;
    math::Vec2 navStart_{-5, -5}; // cell-space markers (invalid when < 0)
    math::Vec2 navGoal_{-5, -5};
    std::vector<math::Vec2> navPath_;

    // Tool panel state.
    std::vector<AssetEntry> assetEntries_;
    std::string assetDir_;
    int assetFilter_ = 0; // 0 all, 1 models, 2 textures, 3 scripts
    bool assetGridView_ = false; // thumbnail grid vs list
    int selectedAsset_ = -1;
    int assetDeletePending_ = -1; // asset index awaiting delete confirmation
    bool deleteAssetRequested_ = false; // Delete key -> confirm in-panel next frame
    gfx::Texture previewTexture_;
    ImTextureID previewTexId_ = ImTextureID_Invalid;
    math::Rect2 viewportRect_{244, 58, 792, 640};
    // The 视口 window's rect in SCREEN pixels (set by BuildViewportPanel); the
    // 3D pass is scissored to it so the scene never bleeds into the dock area.
    math::Rect2 viewportScreenRect_{0, 0, 0, 0};
    // 2D canvas camera state (design-space view): zoom 1 = fit the whole
    // 1280x720 design into the viewport; pan = design point at viewport center.
    float canvasZoom_ = 1.0f;
    math::Vec2 canvasPan_{0.0f, 0.0f};
    std::vector<core::LogEntry> logEntries_;
    int logFilter_ = 0; // 0 all, 1 info+, 2 warn+, 3 error
    bool logAutoScroll_ = true;

    // Transform gizmo (ImGuizmo) state for the viewport.
    ImGuizmo::OPERATION gizmoOp_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode_ = ImGuizmo::WORLD;
    // Docking: the center (视口) and bottom (脚本编辑器) dock node IDs from the
    // rebuilt default layout, used to force-dock windows that can otherwise
    // float over the Inspector and swallow its clicks.
    ImGuiID dockspaceId_ = 0;      // the DockSpace id (for the central node)
    bool viewportDockFallbackDone_ = false; // viewport dockId-lost fallback ran
    bool scriptEditorDockFallbackDone_ = false; // script editor dockId-lost fallback
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
    bool showScriptEditor_ = false;
    bool showAnimEditor_ = false;
    bool showTerrain_ = false;
    // P1-1 animation timeline editor state.
    anim::AnimationClip animClip_;
    std::string animClipPath_;
    bool animClipDirty_ = false;
    float animPlayhead_ = 0.0f;
    bool animPlaying_ = false;
    char animPathBuf_[512] = {};
    std::string scriptEditorPath_;   // file being edited ("" = closed)
    std::string scriptEditorRel_;    // project-relative path for checks
    char scriptEditorBuf_[256 * 1024] = {};
    bool scriptEditorDirty_ = false;
    ScriptCheckResult scriptEditorCheck_; // last syntax check result
    // P1-2 debugger: breakpoints keyed by the script path being edited, plus
    // a dirty flag that pushes them into the running playtest host.
    std::map<std::string, std::set<int>> scriptBreakpoints_;
    bool scriptBreakpointsDirty_ = false;
    char breakpointLineBuf_[64] = {};
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
