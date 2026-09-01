# 编辑器面板插件化 实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 EditorApp 的 ~20 个面板改造成独立 `IPanel` 类，经 `PanelRegistry` 统一管理、可运行时独立加载/卸载；EditorApp 门面化。

**Architecture:** `IPanel` 接口（Title/VisibleFlag/Draw/OnOpen/OnClose）+ `EditorContext`（共享状态聚合，面板不持有 EditorApp*）+ `PanelRegistry`（Register/Unregister/DrawAll/菜单项）。面板类放 `editor/src/panels/`，注册表与接口放 `neon_editor_common`（可被 neon_tests 测试，延续 script_panel_model 纯模型先例）。

**Tech Stack:** C++17、Dear ImGui（docking）、CMake + MSVC、`neon_tests`（注册表纯逻辑）+ `neon_editor --smoke-test 240`。

**关键上下文（已核实）：**
- `editor.hpp` 1020 行：EditorApp 揽 ~20 个 `BuildXxxPanel` 方法 + 面板状态嵌套 struct（TerrainState/ModelPreviewState/ScriptEditorState/NavState/PackageState/ProfilerState/LocState/InputMapState/AnimEditorState/AsmEditorState）。
- `panels.cpp` 428 行 + 9 个 `panels_*.inc`（被 `#include` 展开为 EditorApp 成员函数）。
- `Panels()`（editor.cpp:174）返回 `g_panelStateEntries`（`PanelDef{title, showFlag, inMenu}`）——面板标题→开关标志表，供 ImGui ini 持久化（`RegisterPanelStateHandler`）。
- editor 可测先例：`script_panel_model.hpp`/`bt_editor.hpp`/`history.hpp`/`packager.cpp`（ImGui-free 纯模型）在 `neon_editor_common`，被 `neon_tests` 直接测。
- 验证：`neon_editor --smoke-test 240`（面板全开渲染）+ `neon_tests`（注册表/纯模型）。

**构建/测试命令（全程）：**
- `cmake --build build-msvc --config Release`
- `& "build-msvc\Release\neon_tests.exe"`（基线 753 全绿）
- `& "build-msvc\Release\neon_editor.exe" --smoke-test 240`（退出码 0）

---

## 阶段 1：接口 + 4 个代表面板

### Task 1: `IPanel` / `EditorContext` / `PanelRegistry` 核心

**Files:**
- Create: `editor/src/editor_context.hpp`（EditorContext + IPanel）
- Create: `editor/src/panel_registry.hpp` / `editor/src/panel_registry.cpp`（PanelRegistry）
- Modify: `CMakeLists.txt`（neon_editor_common 加 panel_registry.cpp）
- Test: `tests/test_panel_registry.cpp`

**Step 1:** `editor_context.hpp`：

```cpp
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "neon/ecs/world.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/scene/component_schema.hpp"

namespace neon::editor {

struct SceneEntity;  // editor_scene.hpp（前向声明避免重依赖）

// 面板共享的编辑器上下文：聚合 EditorApp 暴露给面板的共享状态（指针/引用）。
// 面板不持有 EditorApp*——一切共享访问经此上下文。
struct EditorContext {
    // 共享状态（EditorApp 拥有，注入指针）
    ecs::World* sceneWorld = nullptr;
    std::vector<SceneEntity>* entities = nullptr;
    int* selected = nullptr;
    gfx::Renderer* renderer = nullptr;
    assets::AssetManager* assetMgr = nullptr;
    scene::ComponentRegistry* compReg = nullptr;
    std::string* projectDir = nullptr;
    // 跨面板操作（EditorApp 注入回调）
    std::function<void()> refreshAssetDir;
    std::function<void(int)> setSelection;
    void* editorApp = nullptr;  // 过渡期逃生舱：少量未迁移状态经此访问（阶段 3 移除）
};

// 面板接口：独立状态 + ImGui 渲染 + 生命周期。
struct IPanel {
    virtual ~IPanel() = default;
    virtual const char* Title() const = 0;
    virtual bool* VisibleFlag() = 0;
    virtual void Draw(EditorContext& ctx) = 0;
    virtual void OnOpen(EditorContext& ctx) {}
    virtual void OnClose() {}
    virtual void OnUpdate(float dt) {}
    virtual bool InMenu() const { return true; }
};

} // namespace neon::editor
```

（`void* editorApp` 是过渡期逃生舱：面板迁移初期可能访问未进 ctx 的状态；阶段 3 移除。）

**Step 2:** `panel_registry.hpp/.cpp`：

```cpp
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "editor_context.hpp"

namespace neon::editor {

class PanelRegistry {
public:
    void Register(std::unique_ptr<IPanel> panel);              // 加载
    std::unique_ptr<IPanel> Unregister(const std::string& title); // 卸载并返回
    IPanel* Find(const std::string& title);
    void OpenAll(EditorContext& ctx);   // 全部 OnOpen
    void Shutdown();                    // 全部 OnClose
    void UpdateAll(float dt);
    void DrawAll(EditorContext& ctx);   // 可见面板 Draw
    void DrawMenuItems();               // ImGui 菜单勾选项（切换 VisibleFlag）
    std::vector<IPanel*>& Panels() { return panels_; }
    size_t Count() const { return panels_.size(); }

private:
    std::vector<std::unique_ptr<IPanel>> panels_;
};

} // namespace neon::editor
```

`DrawAll`：对每个面板，`if (*panel->VisibleFlag()) ImGui::Begin(panel->Title(), panel->VisibleFlag()) + panel->Draw(ctx) + ImGui::End()`。`DrawMenuItems`：`ImGui::MenuItem(title, nullptr, visibleFlag)`。

**Step 3:** 单测（`tests/test_panel_registry.cpp`，用假面板记录调用）：Register/Find/Unregister 生命周期、DrawAll 只画可见面板、Shutdown 调 OnClose、卸载后再注册。

**Step 4:** CMakeLists：neon_editor_common 加 `panel_registry.cpp`；neon_tests 加 `test_panel_registry.cpp`（neon_editor_common 已被 neon_tests 链接）。

**Step 5:** 构建 + 全绿（753 + 新测试）+ editor 冒烟。
**Step 6:** Commit: `feat: 面板插件化核心（IPanel/EditorContext/PanelRegistry）`

---

### Task 2: 拆场景面板（ScenePanel）

**Files:**
- Create: `editor/src/panels/scene_panel.hpp/.cpp`
- Modify: `editor/src/panels_scene.inc`（删除 `BuildScenePanel` 实现，迁移到新类）
- Modify: `editor/src/editor.hpp`（删面板状态成员/方法声明）+ `editor/src/editor.cpp`（OnCreate 注册面板）
- Test: `neon_editor --smoke-test 240`

**迁移规则**（后续面板任务同）：
1. 读 `panels_scene.inc` 的 `BuildScenePanel` + editor.hpp 里场景面板状态。
2. 新类 `ScenePanel : IPanel`：状态成员从 EditorApp 移入，Draw 实现 = 原 BuildScenePanel 方法体（`EditorApp::xxx` → `ctx.xxx`；私有辅助函数若只被本面板用，一并迁入）。
3. editor.hpp 删对应状态/方法声明；panels_scene.inc 删实现。
4. editor.cpp OnCreate：`panels_.Register(std::make_unique<ScenePanel>())`；BuildImGuiUI 里原 `BuildScenePanel()` 调用改 `panels_.DrawAll(ctx_)`（或逐步替换）。
5. EditorApp 持有 `PanelRegistry panels_` + `EditorContext ctx_`（OnCreate 构建）。

**Step 1-6:** 迁移 → 构建 → 冒烟（场景面板正常渲染/树形层级/增删）→ 全绿。
**Step 7:** Commit: `refactor: 场景面板拆为独立 ScenePanel`

---

### Task 3: 拆资产面板（AssetPanel）

同 Task 2 模式。`panels_asset_panel.inc` 436 行（最大 UI 面板之一）：`BuildAssetPanel` + 资产状态（`assetDir_`/`assetDirSignature_`/`InPrefabsDir`/`ImportAssetFile`/`CreateAssetFile`/`DeleteSelectedAsset`）+ `PickImportFile`/`ListDirectory`（已共享，留 editor_util）。拖拽生成实例、材质球、缩略图逻辑全在面板内。
Commit: `refactor: 资产面板拆为独立 AssetPanel`

---

### Task 4: 拆属性面板（InspectorPanel）

同模式。`panels_inspector.inc` 942 行（最大）：`BuildInspectorPanel` + `ApplyMaterialParams`/`ResolveMesh`/`ClampSelection` + kNodeTypes 表。**注意**：属性面板大量用 `setSelection`/实体编辑（经 ctx 的 entities/selected），材质球保存逻辑一并迁入。
Commit: `refactor: 属性面板拆为独立 InspectorPanel`

---

### Task 5: 拆日志面板（LogPanel）

同模式。日志面板较简单（BuildLogPanel + 日志缓冲状态）。作为模式验证的最小面板。
Commit: `refactor: 日志面板拆为独立 LogPanel`

---

## 阶段 2：其余面板（概要，模式同 Task 2-5）

按依赖从简到繁逐个拆（每个独立 commit + 冒烟）：

- **Task 6**: 资源面板（BuildResourcePanel，panels_assets.inc）
- **Task 7**: 模型预览（BuildModelPreviewPanel/RenderModelPreviewPanel/ModelPreviewState + OpenModelPreview——有 RT/渲染，中等）
- **Task 8**: 插件面板（BuildPluginPanels/BuildPluginsPanel）
- **Task 9**: 导航面板（BuildNavPanel + NavState）
- **Task 10**: 调试覆盖（BuildDebugOverlayPanel/DrawDebugOverlay——F3 面板 + 视口层）
- **Task 11**: 本地化（BuildLocPanel + LocState）
- **Task 12**: 性能（BuildProfilerPanel + ProfilerState）
- **Task 13**: 输入映射（BuildInputMapPanel + InputMapState）
- **Task 14**: 世界面板（panels_world.inc：地形/瓦片 + TerrainState 等）
- **Task 15**: 脚本面板（panels_script.inc 574 行 + ScriptEditorState——含 TextEditor/断点，最大单面板）
- **Task 16**: UI 编辑器（BuildUIEditorPanel/UpdateUIEditorViewport/MarkUIDirty）
- **Task 17**: 预览（panels_preview.inc + AnimEditorState/AsmEditorState——动画时间线/状态机编辑器）

**视口面板特殊**（BuildViewportPanel/DrawTransformGizmo/RunGizmoDragSim/DrawPlayHUD）：渲染 3D 视口 + gizmo + 输入，与 EditorApp 的 OnEvent/相机强耦合。**建议保留为"核心面板"**（最后一个拆或暂不拆，在 Task 18 单独评估）。

## 阶段 3：收尾（概要）

- **Task 18**: 视口面板评估/拆分（或文档化保留理由）
- **Task 19**: EditorApp 门面化收尾：`Panels()`/`PanelDef`/`g_panelStateEntries`/`RegisterPanelStateHandler` 迁到 PanelRegistry（ini 持久化经 VisibleFlag 表）；`BuildImGuiUI` 改纯 `panels_.DrawAll(ctx_)`；删除 `void* editorApp` 逃生舱；editor.hpp 目标 <500 行
- **Task 20**: 菜单整合（窗口菜单经 `panels_.DrawMenuItems()`）+ 最终冒烟/文档更新（C3 → [x]）

---

## 验收（全部完成后）

1. `neon_tests` 全绿（753 基线 + 注册表/纯模型新测试）。
2. `neon_editor --smoke-test 240` 通过（面板全开渲染无崩溃，面板开关状态持久化不回归）。
3. 每个面板是 `editor/src/panels/` 下独立 IPanel 类；EditorApp 无面板成员函数/嵌套面板状态。
4. `editor.hpp` < 500 行（门面 + 共享状态）。
5. **可独立加载/卸载**：`PanelRegistry::Register/Unregister` 可用；新增面板 = 写 IPanel 子类注册，无需改 EditorApp 核心。

## 风险与回滚

- 面板对 EditorApp 私有状态访问面大：`EditorContext` 逐面板扩展（阶段 1 的 4 个面板定下 ctx 形态，后续面板沿用）；`void* editorApp` 逃生舱兜底，阶段 3 移除。
- 视口面板与输入/相机强耦合，阶段 3 单独评估。
- 每任务独立 commit + 冒烟，可单独回滚。

---

## ��ɼ�¼��2026-09-01 ��β��

- Task 1-17��19 ����壨Scene/Asset/Inspector/Log/Resource/ModelPreview/Plugins/Nav/
  DebugOverlay/Loc/Profiler/InputMap/Terrain/Tilemap/Package/ScriptEditor/UIEditor/
  AnimEditor/AsmEditor��ȫ����Ϊ���� IPanel �࣬�� PanelRegistry ע�ᣨ�ɶ�������/ж�أ���
- Task 18 ���ߣ��ӿ���壨BuildViewportPanel������Ϊ���༭����BuildBtPanel �� 4 ��
  Bt* ����壩����Ϊ��������塹������Ⱦ 3D ��ͼ + gizmo + �ڵ㻭������ EditorApp ��
  ����/���/undo �����ϣ���ֵ��������ȵ͡�documented in DEVELOPMENT.md C3��
- Task 19��ini �־û���Panels()/RegisterPanelStateHandler ����Աָ�������������show
  ��־���� EditorApp��oid* editorApp �����ձ�����EditMeshKeyCommand ��Ҫ EditorApp*����
- Task 20��DEVELOPMENT.md C3 ��� [x]��
- editor.hpp��1020 �� 926 �У����״̬�Ƴ�����ͼ/BT/����״̬��������
- ��֤��
eon_tests 757 ȫ�̣�
eon_editor --smoke-test 240 fail-delta = 0
  ��11 ��Ԥ�滷��ʧ����Ķ��޹أ���
