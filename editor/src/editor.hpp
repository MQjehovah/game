#pragma once

#include "TextEditor.h"
#include "TextEditor.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/audio/audio.hpp"
#include "neon/kernel/kernel.hpp"
#include "neon/plugin/backend.hpp"
#include "neon/gfx/light_probe.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/io/vfs.hpp"
#include "neon/neon.hpp"
#include "neon/scene/component_schema.hpp"
#include "neon/scene/skinned_model.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/ui/system.hpp"
#include "bt_editor.hpp"
#include "imgui.h"
#include "ImGuizmo.h"
#include "history.hpp"
#include "packager.hpp"
#include "script_panel_model.hpp"
#include "editor_plugin.hpp"
#include "panel_registry.hpp"

namespace neon::editor {

// 面板插件化（Task 7）：模型查看器迁移后的独立面板类（panels/model_preview_panel.hpp）。
// 仅转发器需要指针，故只前向声明；完整类型由 editor.cpp include。
class ModelPreviewPanel;
// 面板插件化（Task 8）：插件管理面板（panels/plugins_panel.hpp）。经 ctx.pluginMgr
// 访问 EditorPluginManager，无转发器指针。
class PluginsPanel;
// 面板插件化（Task 9）：导航面板（panels/nav_panel.hpp）。经 ctx.nav 访问 NavState。
class NavPanel;
// 面板插件化（Task 10）：调试覆盖层面板（panels/debug_overlay_panel.hpp）。
// DrawDebugOverlay 保留为薄转发（editor_viewport 画视口图层），经指针访问。
class DebugOverlayPanel;
// 面板插件化（Task 11）：本地化面板（panels/loc_panel.hpp）。状态全迁入面板。
class LocPanel;
// 面板插件化（Task 12）：性能面板（panels/profiler_panel.hpp）。经 ctx 指针访问
// ProfilerState / 模拟时钟 / 播放统计。
class ProfilerPanel;
// 面板插件化（Task 13）：输入映射面板（panels/input_map_panel.hpp）。经 ctx.inputMap
// 访问共享 InputMapState（OnEvent 也读写）。
class InputMapPanel;
// 面板插件化（Task 14）：地形 / 瓦片 / 打包面板（panels/{terrain,tilemap,package}_panel.hpp）。
// 地形状态经 ctx.terrain 共享；打包经 ctx.runPackage 回调（RunPackage 返回报告）。
class TerrainPanel;
class TilemapPanel;
class PackagePanel;

// P2-editor UX: a full TRS triple used by the multi-selection batch transform.
struct Transform3 {
    math::Vec3 pos;
    math::Quat rot;
    math::Vec3 scale;
};

// Viewport camera presets (T4.8 multi-camera viewport): the perspective orbit
// camera plus static orthographic top (down -Y) and front (down -Z) views.
enum class ViewCam { Perspective, Top, Front };

struct SceneEntity {
    int id = 0;       // stable per-scene id (0 = unassigned; parentId references this)
    std::string name;
    int parentId = 0; // scene-tree parent by entity id (0 = root)
    std::string prefab; // assets/prefabs/<name>.json template reference ("" = none)
    // Node type (P1-1): "Node" | "MeshInstance3D" | "Camera3D" | "CharacterBody"
    // | "Sprite" | "Light3D" | "" (auto-derived from meshKey/sprite).
    std::string nodeType;
    float cameraFov = 60.0f;  // Camera3D type only (degrees)
    bool cameraOrtho = false; // Camera3D type only
    float cameraOrthoSize = 10.0f; // Camera3D ortho "Size" (half view height)
    float cameraAspect = 0.0f; // Camera3D view aspect (0 = 16:9 default); the
                               // play game area letterboxes to THIS
    // DirectionalLight3D-style light object (Unity): a scene object that lights
    // the world. The editor auto-creates a default Main Camera + Directional
    // Light for a new/empty scene so there is always an observer + a light.
    bool hasLight = false;
    scene::SceneLight light;
    std::string meshKey; // "terrain" | "helmet" | "cube" | "tree" | "obj:<path>" | "gltf:<path>"
    // P1-1 terrain editing: heightmap canvas for meshKey "terrain".
    std::vector<float> terrainHeights_;
    int terrainSegments_ = 48;
    float terrainSize_ = 60.0f;
    float terrainHeightScale_ = 1.0f;
    // G2-3 chunked LOD + vegetation (serialized into the terrain component;
    // the runtime renders chunk patches + veg, the editor still paints a flat
    // heightfield mesh).
    int chunkGridDiv_ = 0;
    int chunkLodLevels_ = 3;
    int chunkBaseSubdiv_ = 16;
    std::string vegMeshKey_;
    uint32_t vegCount_ = 0;
    uint32_t vegSeed_ = 1;
    float vegSize_ = 1.0f;
    float vegImpostorDistance_ = 60.0f;
    float vegMinHeight_ = 0.0f;
    float vegMaxHeight_ = 3.0f;
    float vegMaxSlope_ = 0.30f;
    // P1-1 2D tilemap editing (meshKey "tilemap").
    std::vector<std::string> tilemapTiles_;
    int tilemapCols_ = 8;
    int tilemapRows_ = 5;
    float tilemapCellSize_ = 80.0f;
    // P2-1 ground decal editing (flat XZ quad).
    std::string decalTex;
    float decalSize = 2.0f;
    float decalAlpha = 1.0f;
    math::Vec3 pos{};
    math::Quat rot{};
    math::Vec3 scale{1, 1, 1};
    float zOrder = 0.0f;  // P2-3: 2D sprite draw order (lower draws first)
    gfx::Color tint{1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.8f;
    float uvRepeat = 1.0f; // UV tiling multiplier for the entity's material
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
    // Animated skinned glTF model (meshKey "gltf:..."): loaded by ResolveMesh
    // when the file contains a skinned mesh. Edit-mode drawing uses the
    // parts + bone matrices so the viewport matches the play.
    std::shared_ptr<scene::SkinnedModel> skinned;
    gfx::Mesh decalMesh;       // P2-1: flat ground-decal quad (lazy)
    float ao = 1.0f;               // AO strength (0 = ignore AO map, 1 = full)
    float emissiveIntensity = 1.0f;
    // Material asset reference (assets/materials/<name>.mat.json): when set, the
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
    // Sequence-frame sprite animation (mirrors SceneSprite): non-empty
    // spriteFrames plays them at spriteFps; the editor round-trips them so
    // playtest and save keep the animation.
    std::vector<std::string> spriteFrames;
    float spriteFps = 0.0f;
    bool spriteLoop = true;
    // Spritesheet variant (one atlas texture, sub-rects).
    std::string spriteSheet;
    int spriteSheetFrames = 0;
    gfx::Mesh spriteMesh;       // unit XY quad (faces the front camera)
    gfx::Material spriteMaterial; // unlit texture material
    // Health (mirrors the built-in `health` component): maxHp <= 0 means the
    // entity tracks no health. Used by the play for the hero + combat mobs.
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
// levels, and play the result.
struct EditorProject {
    std::string name;       // game.json "title" (fallback: directory name)
    std::string dir;        // "projects/pvz" (the default sandbox: kDefaultProjectDir)
    std::string startScene; // game.json "startScene" (project-relative)
    std::string mode = "3d"; // game.json "editor.mode": "2d" | "3d" (default 3d)
    std::vector<std::string> scenes; // scenes/*.json (project-relative)
};

// The default sandbox project: a first-class project under projects/ that
// hosts the shared demo/sandbox assets (models, kenney nature pack, fonts)
// and the editor's scratch scene. It replaces the old "repo root as sandbox"
// layout (root assets/ + editor_scene.json).
inline constexpr const char* kDefaultProjectDir = "projects/default";
// The sandbox scratch scene, project-relative inside kDefaultProjectDir.
// User data (not versioned) — the editor writes it on Ctrl+S like any scene.
inline constexpr const char* kSandboxSceneRel = "assets/scenes/editor_scene.json";

// Shared UTF-8 directory listing (editor project scanner + asset panel).
bool ListDirectory(const std::string& dir, std::vector<AssetEntry>& out);
// Deletes a file or directory tree (recycle bin on Windows).
bool DeletePathRecursive(const std::string& path);

class EditorApp : public core::Application {
public:
    friend class EditMeshKeyCommand;
    // Registers the ImGui ini settings handler that persists each panel's
    // open/closed state across launches (needs private member access to build
    // the title -> show-flag table).
    friend void RegisterPanelStateHandler(EditorApp* app);

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
    void SetPvzPlayOnStart(bool v) { pvzPlayOnStart_ = v; }
    // --play: auto-start the current project's play (any mode, not just 2D).
    void SetPlayOnStart(bool v) { playOnStart_ = v; }
    // --ui-editor: open the UI editor panel at startup (auto-opens the
    // project's first ui/*.ui.json so the viewport preview is testable).
    void SetUIEditorOnStart(bool v) { uiEditorOnStart_ = v; }
    void SetProjectOnStart(const std::string& dir, bool loadScene) {
        projectDirOnStart_ = dir;
        loadProjectOnStart_ = loadScene;
    }
    void SetBackendName(const std::string& name) { backendName_ = name; }

    // --- Editor plugin API (NeonEditor.* native bindings) ------------------
    editor::EditorPluginManager* PluginManager() { return pluginMgr_.get(); }
    const editor::EditorPluginManager* PluginManager() const { return pluginMgr_.get(); }
    // Adds an entity with a mesh key at an explicit position (undoable).
    void PluginAddEntity(const std::string& meshKey, float x, float y, float z);
    // Builds a procedural mesh from vertices + 1-based triangle indices,
    // writes it as an OBJ asset and returns the "obj:<path>" mesh key.
    std::string PluginBuildMesh(const std::string& name,
                                const std::vector<math::Vec3>& verts,
                                const std::vector<int>& indices);
    script::Value PluginSelectedEntity() const;
    script::Value PluginEntityList() const;
    void PluginLog(const std::string& msg);
    // Copies a source asset into the current asset browse dir; returns the
    // project-relative path ("" on failure).
    std::string PluginImportAsset(const std::string& srcPath);
    // Lists files (not dirs) under `dir` (relative/absolute path).
    std::vector<std::string> PluginListDir(const std::string& dir) const;
    bool SmokeFailed() const { return smokeFailed_; }
    void SetPreviewOnStart(const std::string& p) { previewOnStart_ = p; }
    void RequestScreenshot(const std::string& path, uint64_t frame) {
        screenshotPath_ = path;
        screenshotFrame_ = frame;
    }

private:
    void SetupScene();
    void InitToolPanels();
    void BuildImGuiUI();
    void BuildViewportPanel();
    // 模型查看器（Task 7）已迁入 panels/model_preview_panel.hpp；OpenModelPreview /
    // RenderModelPreviewPanel 保留为薄转发（editor_ui 右键菜单 / 调试覆盖层 /
    // editor_viewport / --preview 启动参数仍调用），实现一行转 modelPreviewPanel_。
    void OpenModelPreview(const std::string& path);
    void RenderModelPreviewPanel();
    void BuildPluginPanels();
    // 插件管理面板（Task 8）已迁入 panels/plugins_panel.hpp；BuildPluginsPanel 删除。
    // 导航面板（Task 9）已迁入 panels/nav_panel.hpp；BuildNavPanel 删除。
    // G8-3 debug overlay: F3 面板已迁入 panels/debug_overlay_panel.hpp（Task 10）；
    // DrawDebugOverlay 保留为薄转发（editor_viewport 画视口图层）。
    void DrawDebugOverlay(const gfx::Camera& cam);
    void BuildUIEditorPanel();
    // UI editor viewport input: click selects a node, drag moves it, corner
    // handles resize it (design-space coordinates).
    void UpdateUIEditorViewport();
    // Marks the open UI document dirty and, when it has a real file path,
    // saves it immediately so edits survive closing the panel / the editor
    // (untitled docs wait for the explicit 保存 button).
    void MarkUIDirty();
    // 本地化面板（Task 11）已迁入 panels/loc_panel.hpp；BuildLocPanel 删除。
    // 性能面板（Task 12）已迁入 panels/profiler_panel.hpp；BuildProfilerPanel 删除。
    // 输入映射面板（Task 13）已迁入 panels/input_map_panel.hpp；BuildInputMapPanel
    // 删除。LoadInputMapEdit/SaveInputMapEdit 保留（OnCreate 也调 Load）。
    void DrawPlayHUD();
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
    // True when the asset panel is browsing this project's assets/prefabs dir
    // (a .json there is a prefab template, draggable to spawn an instance).
    bool InPrefabsDir() const;
    std::string assetDirSignature_;  // P1-1: cached asset-dir listing signature
    // Asset panel actions: copy a file into the current asset dir and create
    // a new asset (dir / lua / json / empty text). Both refresh the listing.
    void ImportAssetFile(const std::string& srcPath);
    void CreateAssetFile(const std::string& name, int kind);
    // Deletes the selected asset (file or directory, recursive). Windows uses
    // the recycle bin (undo-able); POSIX removes recursively after confirm.
    void DeleteSelectedAsset();
    // Material-ball assets (Unity .mat / Godot Material style): save the
    // selected entity's material as assets/materials/<name>.mat.json and apply a
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
    // Applies the scene environment (day sky + fog, the scene's
    // directional/ambient/point light objects) to the renderer. Shared by the
    // edit view and the 2D play so Play shows exactly what the edit camera
    // sees (same sky, same lighting).
    void ApplySceneEnvironment();
    // True when the mouse (screen px) is inside any visible docked TOOL panel
    // (position-based: this runs before ImGui::NewFrame, so hover flags are
    // stale). Both viewport input paths (3D canvas + UI editor) use it so a
    // click on a panel field never leaks into the canvas - previously clicking
    // a UI-editor inspector field ALSO ran the canvas hit-test, which missed
    // and cleared the selection ("编辑界面消失").
    bool MouseOverToolPanel();
    // === Viewport dock plumbing (SINGLE source of truth) ===================
    // Every render/input path that targets the viewport dock goes through
    // these two helpers. Do NOT call Set2DViewport / Set2DViewportPixels /
    // SetScissor / SetSceneViewport on the renderer directly from feature
    // code - that is how the mappings drifted apart between edit view / 2D
    // play / UI editor before.
    //
    // BindDock2DMapping maps design coordinates onto the dock:
    //   designFit=true  -> the 1280x720 design space fits the dock with the
    //                      shared canvas zoom/pan (2D canvas, 2D play and
    //                      the UI editor are all design-space canvases)
    //   designFit=false -> 1:1 design pixels anchored at the dock origin
    //                      (3D HUD/billboards)
    void BindDock2DMapping(bool designFit, float aspect = 16.0f / 9.0f);
    // RAII scope: BindDock2DMapping + scissor clip to the dock (+ the 3D
    // rasterization viewport when sceneVp); the destructor undoes everything
    // (reset scene viewport, flush 2D, scissor off, reset the 2D mapping).
    // A no-op while the dock rect is not valid yet (first frame).
    class DockViewportScope {
    public:
    DockViewportScope(EditorApp& app, bool designFit, bool sceneVp,
                      float aspect = 16.0f / 9.0f);
    ~DockViewportScope();
        bool Active() const { return active_; }
    private:
        EditorApp& app_;
        bool sceneVp_;
        bool active_ = false;
    };
    // Editor gizmos: draws camera frusta + light icons for scene objects as an
    // ImGui overlay (called from BuildImGuiUI so it draws after the 3D scene and
    // aligns with the transform gizmo).
    void DrawSceneGizmos();
    // The active viewport dock rect (screen pixels) and its aspect ratio; the
    // 3D camera projection and gizmo use these so the scene fits the panel.
    const math::Rect2& ViewportRect() const { return viewportScreenRect_; }
    // The rect the 3D scene renders into (the design-fit sub-rect of the
    // viewport). Picking / gizmo / camera aspect use this, not the raw dock.
    const math::Rect2& SceneRect() const {
        return sceneRect_.w > 0.0f ? sceneRect_ : viewportScreenRect_;
    }
    // The scene rect guaranteed non-empty: falls back to the full window before
    // the viewport dock is laid out. Every screen<->world transform in the
    // editor routes through this single source of truth for the 3D framing.
    math::Rect2 ValidSceneRect() const;
    float ViewportAspect() const {
        const math::Rect2& vp = SceneRect();
        if (vp.w > 0.0f && vp.h > 0.0f) return vp.w / vp.h;
        return static_cast<float>(renderer_.ScreenWidth()) /
               static_cast<float>(renderer_.ScreenHeight());
    }
    // Unprojects the current mouse position into a world ray through the scene
    // rect with the active camera. Picking, terrain brush and terrain hover all
    // share this one viewport transform (they used to each reimplement the
    // mouse->NDC->ray math and drifted apart).
    math::Ray PickRay();
    // Builds the runtime camera for play: the scene's Camera3D entity when one
    // exists, else the fixed 1280x720 design-space ortho. Shared by the 2D and
    // 3D play paths (previously written twice, with drift).
    gfx::Camera PlayCamera() const;
    // The play game area's aspect (scene camera `aspect`, default 16:9).
    float PlayCameraAspect() const;
    void SaveScene();
    void LoadScene(const std::string& path);
    // Unity default scene: ensures a Main Camera + a Directional Light object
    // exist (adds them if missing). Called on new/empty and existing scenes so
    // nothing is observer-less. 2D projects default the camera to orthographic.
    void EnsureSceneDefaultObjects();
    // G1-3: assigns a unique id to every entity missing one (id == 0), using
    // max existing id + 1. Called after scene load and setup so the scene tree
    // can reference parents by id before the first save.
    void NormalizeEntityIds();
    // Stable per-parent sort of the scene tree by entity name (case-
    // insensitive, recursive). Pushed as one undoable reorder command.
    void SortSceneTreeByName();
    // Project switcher (Godot-style): ScanProjects discovers the projects/
    // folders; SwitchProject loads a project's game.json, its scene/level
    // lists and enters the declared edit mode (2d canvas or 3D scene tree).
    void ScanProjects();
    void SwitchProject(const std::string& dir);
    // Loads every assets/prefabs/*.json from the current project into prefabLib_.
    void LoadPrefabLibrary();
    // Saves the selected entity's components as assets/prefabs/<name>.json.
    void SavePrefab(const std::string& name);
    // Rewrites every entity's asset paths (mesh/obj/gltf keys, albedoTex/mrTex/
    // aoTex/emissiveTex, decalTex, spriteTex) into the project-relative "@assets/..."
    // form. Called before saving so a scene always stores portable paths.
    void NormalizeEntityAssetPaths();
    // G5-4-4(项3): asset GUID database (Unity ".meta" model). Builds the
    // project's GUID map, compares it against the previous run's snapshot, and
    // rewrites path references in assets/scenes + assets/prefabs + assets/ui
    // JSON for any asset that
    // moved/renamed (GUID preserved) — a rename never silently breaks a scene.
    void RefreshAssetDatabase();
    // G5-4-4(项1): materializes a prefab template's components into a fresh
    // entity (mesh/health/scripts/extraComponents; transform left default).
    // Shared by AddEntity("prefab:...") and the inspector's 重置为预制体.
    SceneEntity MaterializePrefabEntity(const std::string& pfName, const math::Vec3& pos);
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
    // vectors below so play/level data stays scene-driven.
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
    // In-editor play (F5): a GameRuntime snapshot of the editor scene runs
    // in the viewport while the editor scene stays untouched.
    void TogglePlay();
    void StartPlay();
    void StopPlay();
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
    // play's scripts and the scene's referenced assets (throttled every 30
    // frames). A changed script restarts the play (Stop + Start = state
    // reset); a changed texture/OBJ is re-read through the AssetManager and
    // the owning entities re-resolved. Shaders are compiled from strings at
    // init and are NOT hot-reloaded (documented; see PollHotReload).
    void PollHotReload();

    // Behavior tree editor (T4.4): docked 行为树 panel with a node palette,
    // a drag canvas, link creation, param editing, save/load of .bt.json and a
    // play debug highlight driven by bt::Context::activePath.
    void BuildBtPanel();
    void BuildBtToolbar();
    void BuildBtPalette();
    void BuildBtCanvas();
    void BuildBtParams();
    void BtNewTree();
    bool BtSaveToFile(const std::string& path);
    bool BtLoadFromFile(const std::string& path);
    void BtPushSnapshot(const btgraph::BtGraph& before);
    void BtUpdatePlayHighlight();
    void BtRefreshBehaviorFiles();
    std::string BtBehaviorsDir() const;
    // Canvas mouse handling, extracted so the smoke can drive the real link
    // path: `cm` is a canvas-space point, ctrl/shift carry the modifier state.
    void BtCanvasClick(const math::Vec2& cm, bool ctrl, bool shift);
    void BtParamNumber(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamString(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamBool(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);
    void BtParamJson(const btgraph::BtGraphNode& n, const bt::ParamInfo& p);

    // Script panel (T4.5): docked 脚本 panel listing the project's
    // assets/scripts/
    // with per-file syntax checks, plus attach/configure/detach for the
    // selected entity. RefreshScriptChecks re-scans
    // <projectDir>/assets/scripts/ and
    // re-runs CheckSyntax on each file (used by the panel and the smoke).
    // Script editor (Godot-style built-in): open/save a .lua with live syntax
    // check, plus a one-click external-editor binding (system default).
    void OpenScriptEditor(const std::string& path);
    void SaveScriptEditor();
    void BuildScriptEditorPanel();
    void BuildAnimEditorPanel();
    void BuildStateMachineEditorPanel();
    // 世界面板（Task 14）：BuildTerrainPanel/BuildTilemapPanel/BuildPackagePanel
    // 已迁入 panels/{terrain,tilemap,package}_panel.hpp，声明删除。
    void SaveSceneAsChild();
    void ReloadEntityShader(SceneEntity& e);
    void ApplyEditorTheme();
    void PaintTerrain(const math::Ray& ray);
    void RebuildTerrainMesh(SceneEntity& e);
    void OpenInExternalEditor(const std::string& path);

    // Package panel (T4.6) 已迁入 panels/package_panel.hpp（Task 14）；RunPackage
    // 保留（ctx.runPackage 回调 + 冒烟测试用），输出目录由调用方传入。
    pack::PackageReport RunPackage(const char* outDir);

    // Single source of truth for the toggleable editor panels: one entry per
    // panel drives the 视图 menu, the ini persistence table and the panel
    // dispatch. `inMenu` hides settings-only panels (e.g. the debug overlay,
    // toggled by F3). Replaces the former three-way sync (show-flag field +
    // menu item + settings table) that kept drifting apart (C3).
public:
    struct PanelDef {
        const char* title;
        bool EditorApp::*flag;
        bool inMenu = true;
    };
    static const PanelDef* Panels();
    static int PanelCount();

private:
    // ---------------------------------------------------------------------
    // Per-panel editor state (C3): extracted from the flat EditorApp members.
    // Each panel owns its state; EditorApp holds one instance per panel instead
    // of ~55 flat members that sat directly on the god class.
    // ---------------------------------------------------------------------
    struct AnimEditorState {
        anim::AnimationClip clip;
        std::string clipPath;
        bool clipDirty = false;
        float playhead = 0.0f;
        bool playing = false;
        char pathBuf[512] = {};
    };
    struct AsmEditorState {
        anim::AnimationStateMachine machine;
        std::string path;
        bool dirty = false;
        char pathBuf[512] = {};
    };
    struct ScriptEditorState {
        std::string path;   // file being edited ("" = closed)
        std::string rel;    // project-relative path for checks
        TextEditor edit;    // built-in editor (Lua/JS syntax highlight)
        bool dirty = false;
        ScriptCheckResult check; // last syntax check result
        std::map<std::string, std::set<int>> breakpoints;
        bool breakpointsDirty = false;
        char varsBuf[32768]{};   // raw JSON vars editor (32 KB; truncation detected)
        std::string varsError;   // last vars-parse / truncation message
    };

    gfx::Renderer renderer_;
    // Play audio: procedural SoundFx synthesized per PlaySfx(name) and
    // played through the backend. Default = platform (miniaudio / WinMM /
    // null); G5-1: when a native audio plugin is staged under ./plugins the
    // backend comes from the plugin DLL instead (hot-swappable middleware).
    // Custom deleter so a plugin-created backend is destroyed via its own
    // module (destroy_backend) — never a host-side delete across the ABI.
    // pluginAudio_ is declared first so audioBackend_ dies before the DLL
    // unloads.
    std::unique_ptr<plugin::AudioBackend> pluginAudio_;
    std::unique_ptr<neon::audio::IAudioBackend,
                    std::function<void(neon::audio::IAudioBackend*)>>
        audioBackend_;
    assets::AssetManager assetMgr_;
    // Project-root virtual file system mounted at the current project dir.
    // Asset references use the "@assets/..." scheme; AssetManager resolves them
    // against this root so editor + play share one path model. Recreated on
    // project open so the root follows projectDir_.
    std::unique_ptr<neon::io::DiskFileSystem> assetVfs_;
    // (Re)mounts assetVfs_ at projectDir_ and points the AssetManager at it.
    void MountAssetVfs();
    gfx::Font pixelFont_;
    gfx::Font cjkFont_;

    std::vector<SceneEntity> entities_;
    int selected_ = -1;
    // G2-2 编辑器 ECS 化（第一阶段）：编辑器持有 live `ecs::World` 作为场景的
    // 规范运行时表示——`LoadScene` 用运行时的 `Instantiate` 装载（与播放器完全
    // 相同），entities_ 目前仍是 UI 读写模型；`RefreshSceneWorld` 从当前场景重建
    // World，后续阶段把面板/视口/历史直接迁移到 World 组件读写，消除双模型。
    ecs::World sceneWorld_;
    scene::ComponentRegistry sceneCompReg_;
    void RefreshSceneWorld();
    // G2-2: play/save output from the runtime World (entities_ -> World via
    // SyncWorldFromEntities -> canonical SceneFile::FromWorld).
    void SyncWorldFromEntities();
    core::Result<core::Json> BuildSceneJsonFromEntities();
    // G5-4: rebuild entities_ from the runtime World's components (reverse of
    // SyncWorldFromEntities) — proves the World drives the editor's model.
    void UnflattenWorldToEntities();
    // P2-editor UX: multi-selection set (active entity = selected_).
    std::set<int> selection_;
    int selectionAnchor_ = -1;  // shift-click range anchor
    void ToggleSelection(int idx);
    void SelectRangeTo(int idx);
    void ClearSelection();
    bool IsSelected(int idx) const;
    std::vector<int> SelectedIndices() const;  // sorted ascending
    bool playActive_ = false;
    // Microkernel (P-E): the playtest's replaceable physics/script modules.
    // Created fresh per play session so it outlives play_ (which holds
    // non-owning pointers into its services) and is torn down with it.
    std::unique_ptr<kernel::Kernel> kernel_;
    std::unique_ptr<scene::GameRuntime> play_; // non-null while playing
    bool pvzPlayOnStart_ = false; // --2d-play: auto-start the play
    bool playOnStart_ = false;        // --play: auto-start (any project mode)
    bool uiEditorOnStart_ = false;    // --ui-editor: open the UI editor panel
    std::string projectDirOnStart_;    // --project: open this project
    bool loadProjectOnStart_ = false;  // --project also loads its start scene
    // Godot-style input mapping panel: edit project input.json actions.
    bool showInputMap_ = false;
    InputMapState inputMapState_;
    void LoadInputMapEdit();
    void SaveInputMapEdit();
    bool f5Pressed_ = false; // edge-trigger: Win32 repeats KeyDown while held

    // Undo/redo command stack. Every scene mutation (entity add/delete/
    // duplicate/reorder, transform gizmo + inspector edits, material/name
    // properties) is routed through it instead of mutating entities_ directly.
    HistoryManager history_;

    // 面板插件化（阶段 1）：独立面板注册表 + 面板共享上下文。已迁移的面板
    // （ScenePanel）在 OnCreate 注册进 panels_；ctx_ 在 OnCreate 填充
    // （共享状态指针 + 回调注入，全部指向 EditorApp 成员/方法）。尚未迁移的
    // 面板仍走 BuildXxxPanel；BuildImGuiUI 只替换已迁移面板的调用点。
    PanelRegistry panels_;
    EditorContext ctx_;

    // Project directory: exported scenes are written to
    // <projectDir>/assets/scenes/. Defaults to the sandbox project.
    std::string projectDir_{kDefaultProjectDir};
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
    TerrainState terrain_; // P1-1 terrain brush + hover preview

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
    // gates the play restart, assetMtimes_ the texture/mesh reload.
    bool hotReload_ = false;
    std::map<std::string, uint64_t> scriptMtimes_;
    std::map<std::string, uint64_t> assetMtimes_;
    uint64_t hotReloadFrame_ = 0;
    int hotReloadCount_ = 0; // smoke: number of reloads performed

    // Profiler panel (T4.8): a rolling frame-time buffer drawn with the
    // ImGui plot API plus per-frame renderer/ECS/physics/BT/memory stats.
    bool showProfiler_ = false;
    bool profilerDrawn_ = false; // smoke: the panel rendered its stats
    ProfilerState profiler_;

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
    // G1-3 perf: resolving a skinned glTF re-parses the file and re-uploads
    // meshes on every ResolveMesh, so N copies of one model (e.g. a wolf pack)
    // each paid the full startup cost. Cache one resolved model per path and
    // clone it per entity (GPU handles shared, animator state per entity).
    std::map<std::string, std::shared_ptr<scene::SkinnedModel>> skinnedModelCache_;
    std::map<std::string, gfx::Mesh> gltfStaticMeshCache_;
    std::map<std::string, gfx::Material> gltfStaticMaterialCache_;
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
    std::string previewOnStart_; // "--preview <path>": open the model viewer on launch
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
    // G8-3 debug overlay (F3). Layer toggles drive the viewport overlays.
    // 开关状态保留在此（视口画物理线框 editor_viewport:241 直接读 debugColliders_）；
    // 探针字段缓存（原 debugProbeField_ 等）已迁入 DebugOverlayPanel（Task 10）。
    bool showDebugOverlay_ = false;
    bool debugColliders_ = true;  // physics wireframe (on by default, keeps old UX)
    bool debugNavMesh_ = false;
    bool debugProbes_ = false;
    bool debugAudio_ = false;
    DebugOverlayPanel* debugOverlayPanel_ = nullptr; // 不拥有；OnCreate 注册时设置
    // Data-driven UI editor (ui/*.ui.json): edit a UI document tree and
    // preview it in the viewport. Opens via 视图 → UI 编辑器.
    bool showUIEditor_ = false;
    std::vector<std::string> uiFiles_; // ui/*.ui.json in the active project
    std::string uiDocPath_;            // absolute path of the open document
    ui::UiDocument uiDoc_;             // document being edited
    bool uiDocOpen_ = false;           // a document is loaded/created
    ui::UiNode* uiSelected_ = nullptr; // selected node (owned by uiDoc_)
    // P5-editor UX: UI-editor multi-selection (active = uiSelected_).
    std::set<ui::UiNode*> uiSelection_;
    bool uiSnapToGrid_ = true;
    float uiGridSize_ = 8.0f;
    void UISelectNode(ui::UiNode* n);
    void UIToggleSelectNode(ui::UiNode* n);
    void UIDeleteSelectedNodes();
    void UIDuplicateSelectedNodes();
    ui::UiNode* UICloneNode(const ui::UiNode& src);
    void UIAlignSelected(int mode);  // 0=left 1=hcenter 2=right 3=top 4=vcenter 5=bottom
    float UISnap(float v) const {
        return uiSnapToGrid_ ? std::round(v / uiGridSize_) * uiGridSize_ : v;
    }
    bool uiDirty_ = false;
    bool uiDragging_ = false;
    int uiResizeHandle_ = -1;          // -1 none, 0..3 corner handles
    math::Vec2 uiDragPos_{0.0f, 0.0f}; // mouse in design space
    bool showLoc_ = false;
    bool showPlugins_ = false; // plugin management panel
    std::unique_ptr<editor::EditorPluginManager> pluginMgr_; // editor plugins

    NavState nav_; // navigation tool (A* grid + start/goal/path)

    // Tool panel state.
    std::vector<AssetEntry> assetEntries_;
    std::string assetDir_;
    int assetFilter_ = 0; // 0 all, 1 models, 2 textures, 3 scripts
    bool assetGridView_ = true;  // thumbnail grid vs list (grid 默认)
    int selectedAsset_ = -1;
    int assetDeletePending_ = -1; // asset index awaiting delete confirmation
    bool deleteAssetRequested_ = false; // Delete key -> confirm in-panel next frame
    gfx::Texture previewTexture_;
    ImTextureID previewTexId_ = ImTextureID_Invalid;
    math::Rect2 viewportRect_{244, 58, 792, 640};
    // The 视口 window's rect in SCREEN pixels (set by BuildViewportPanel); the
    // 3D pass is scissored to it so the scene never bleeds into the dock area.
    math::Rect2 viewportScreenRect_{0, 0, 0, 0};
    // The rect the 3D scene actually rasterizes into (design-fit 16:9 sub-rect
    // of the viewport, or the viewport itself). The camera projection, gizmo
    // and mouse picking all use this so world-anchored 2D UI never drifts.
    math::Rect2 sceneRect_{0, 0, 0, 0};
    // 2D canvas camera state (design-space view): zoom 1 = fit the whole
    // 1280x720 design into the viewport; pan = design point at viewport center.
    float canvasZoom_ = 1.0f;
    math::Vec2 canvasPan_{0.0f, 0.0f};

    // Standalone model viewer (single glTF + animation playback) for clean
    // inspection of geometry/textures/animations independent of a scene.
    // 面板插件化（Task 7）：状态与四个方法已整体迁入 panels/model_preview_panel.hpp
    // （ModelPreviewPanel : IPanel）；showModelPreview_ 仍在此（窗口菜单勾选 +
    // ini 持久化 + 冒烟强制开启），过渡期经构造注入面板作可见标志。
    bool showModelPreview_ = false;
    ModelPreviewPanel* modelPreviewPanel_ = nullptr; // 不拥有；OnCreate 注册时设置

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
    // P2-editor UX: batch gizmo drag over the multi-selection.
    bool gizmoBatchCaptured_ = false;
    std::vector<int> gizmoBatchIndices_;
    std::vector<Transform3> gizmoBatchFrom_;
    // Viewport grid overlay toggle.
    bool showViewportGrid_ = true;
    // Post-process FX toggles (applied to both the editor viewport and play).
    bool postSsao_ = false;
    bool postVolumetric_ = false;
    bool postSsr_ = false;
    float postSsaoIntensity_ = 1.0f;
    float postVolumetricIntensity_ = 1.0f;
    float postSsrIntensity_ = 0.8f;
    std::string tileDragPath_;      // P2-editor UX: tilemap palette drag payload
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
    std::string btActivePath_;  // play highlight: tree-path id of the running node
    std::vector<std::string> btBehaviorFiles_;
    uint64_t btFilesRefreshFrame_ = 0; // throttle: refresh behaviors/ listing periodically
    bool btCanvasDrawn_ = false; // smoke: the BT canvas emitted geometry this frame
    // Canvas drag state.
    std::string btDragNode_;
    math::Vec2 btDragStart_{0.f, 0.f};
    math::Vec2 btNodeStartPos_{0.f, 0.f};
    bool btDragging_ = false;
    // Canvas view transform: pan offset (screen px) + zoom factor (no
    // scrollbars — the canvas only pans/zooms).
    float btZoom_ = 1.0f;
    math::Vec2 btPan_{30.f, 30.f};
    // Anchor-link drag: drawing a connection from a node's output anchor.
    bool btLinking_ = false;
    std::string btLinkFrom_;
    // Graph snapshot captured when a node drag began, pushed as one undo step
    // on release (only when the node actually moved).
    btgraph::BtGraph btGraphBeforeDrag_;
    bool btHasGraphBeforeDrag_ = false;
    // Per-param drag origin: args snapshot captured when a slider drag began,
    // so the undo step reverts to the pre-drag value (one drag = one undo step).
    std::map<std::string, btgraph::BtGraph> btArgDragOrigin_;

    // Script panel (T4.5) state.
    std::unique_ptr<script::IScriptHost> scriptCheckHost_; // throwaway host for syntax checks
    // Throwaway QuickJS host for .js syntax checks (dual-script backend).
    std::unique_ptr<script::IScriptHost> scriptCheckHostJs_;
    // Returns the throwaway syntax-check host matching a script file's
    // extension (.js -> QuickJS, everything else -> Lua), creating it lazily.
    script::IScriptHost* ScriptCheckHostFor(const std::string& path);
    bool showScriptEditor_ = false;
    bool showAnimEditor_ = false;
    bool showStateMachineEditor_ = false;
    bool showTerrain_ = false;
    bool showTilemap_ = false;
    AnimEditorState anim_;   // P1-1 animation timeline editor state
    AsmEditorState asmEdit_; // data-driven state machine editor (.asm.json)
    ScriptEditorState scriptEditor_; // script editor + debugger + vars state

    // Package panel (T4.6) state. PackageState 已迁入 panels/package_panel.hpp
    // （Task 14）；showPackage_ 保留（窗口菜单勾选 + ini 持久化）。
    bool showPackage_ = false;
};

} // namespace neon::editor
