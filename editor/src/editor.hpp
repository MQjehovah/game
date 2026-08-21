#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/ui/system.hpp"
#include "imgui.h"
#include "ImGuizmo.h"
#include "history.hpp"

namespace neon::editor {

struct SceneEntity {
    std::string name;
    std::string meshKey; // "terrain" | "helmet" | "cube" | "tree" | "obj:<path>" | "gltf:<path>"
    math::Vec3 pos{};
    math::Quat rot{};
    math::Vec3 scale{1, 1, 1};
    gfx::Color tint{1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.8f;
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
    void DrawTransformGizmo();
    void RunGizmoDragSim();
    void ApplyMaterialParams(SceneEntity& e);
    void ClampSelection();
    bool ResolveMesh(SceneEntity& e);
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
    gfx::Camera OrbitCamera() const;

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

    bool smokeMode_ = false;
    bool smokeFailed_ = false;
    bool disableShadows_ = false;
    bool bloomEnabled_ = true;
    std::string screenshotPath_;
    uint64_t screenshotFrame_ = 0;

    // ImGui tool UI state.
    std::string pendingText_; // UTF-8 characters queued for ImGui this frame
    bool showHierarchy_ = true;
    bool showInspector_ = true;
    bool showAssets_ = false;
    bool showResources_ = false;
    bool showLog_ = false;
    bool showCustomUIDemo_ = false;
    bool showImGuiDemo_ = false;

    // Tool panel state.
    std::vector<AssetEntry> assetEntries_;
    std::string assetDir_;
    int selectedAsset_ = -1;
    gfx::Texture previewTexture_;
    ImTextureID previewTexId_ = ImTextureID_Invalid;
    math::Rect2 viewportRect_{244, 58, 792, 640};
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
    // Transform values captured when a gizmo drag starts; the write-back pushes
    // a MERGING EditTransformCommand per frame so one drag = one undo step that
    // reverts to these ORIGINAL values.
    bool gizmoDragOriginValid_ = false;
    math::Vec3 gizmoDragOriginPos_{};
    math::Vec3 gizmoDragOriginScale_{1, 1, 1};
    math::Quat gizmoDragOriginRot_{};

    // Custom UI demo widget handles (engine widget system).
    ui::TreeView* demoTree_ = nullptr;
    ui::ComboBox* demoCombo_ = nullptr;
    ui::Button* demoAddButton_ = nullptr;
    int demoAddClicks_ = 0;
    int demoComboChanged_ = -1;
};

} // namespace neon::editor
