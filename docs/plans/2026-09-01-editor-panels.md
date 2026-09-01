# 编辑器面板插件化设计（C3）

日期：2026-09-01
状态：待批准
前置：C1/C2/C4 已完成（GameRuntime + Renderer 已组合服务化），C3 是最后一个上帝类

## 1. 目标

把 `EditorApp`（editor.hpp 1020 行）的 ~20 个面板从「EditorApp 成员函数 + 嵌套 struct 状态」
改造为**独立面板类**，通过 `PanelRegistry` 统一管理，**可运行时独立加载/卸载**（Register/Unregister）。

## 2. 现状（已核实）

- `editor.hpp` 1020 行，EditorApp 揽全部面板方法（`BuildScenePanel`/`BuildAssetPanel`/... ~20 个）
  + 面板状态（`TerrainState`/`ModelPreviewState`/`ScriptEditorState`/`NavState`/`PackageState`/...）。
- `panels_*.inc` 9 个文件被 `#include` 进 panels.cpp，展开为 EditorApp 成员函数（能访问所有私有状态）。
- `Panels()` 返回 `PanelDef{title, showFlag, inMenu}`（标题 → 开关标志）。

## 3. 设计

### 3.1 接口

```cpp
// editor/src/editor_context.hpp
namespace neon::editor {

// 面板共享的编辑器上下文：聚合 EditorApp 暴露给面板的共享状态（引用）。
// 面板不持有 EditorApp*，只经此上下文访问共享数据 + 通用操作。
struct EditorContext {
    ecs::World* sceneWorld = nullptr;            // 场景世界（G2-2 规范运行时表示）
    std::vector<SceneEntity>* entities = nullptr; // 实体 UI 读写模型
    int* selected = nullptr;                     // 选中实体索引
    gfx::Renderer* renderer = nullptr;
    assets::AssetManager* assetMgr = nullptr;
    scene::ComponentRegistry* compReg = nullptr;
    std::string* projectDir = nullptr;
    // 通用操作（由 EditorApp 注入的回调，面板经此做跨面板动作）：
    std::function<void()> refreshAssetDir;       // 资产面板刷新
    std::function<void(int)> setSelection;       // 改选中（使脚本面板缓存失效）
    std::function<core::Result<core::Json>()> buildSceneJson; // 场景序列化
    // ... 按需扩展
};

// 面板接口：每个面板一个实现类，独立状态 + ImGui 渲染 + 生命周期。
struct IPanel {
    virtual ~IPanel() = default;
    virtual const char* Title() const = 0;
    virtual bool* VisibleFlag() = 0;             // 面板开/关（ImGui 窗口可见性）
    virtual void Draw(EditorContext& ctx) = 0;   // ImGui 渲染（每个 tick 调）
    virtual void OnOpen(EditorContext& ctx) {}   // 打开时（加载）
    virtual void OnClose() {}                    // 关闭时（卸载/释放）
    virtual void OnUpdate(float dt) {}           // 非渲染更新（可选）
};

} // namespace neon::editor
```

### 3.2 注册表

```cpp
// editor/src/panel_registry.hpp
namespace neon::editor {

// 面板注册表：EditorApp 持有，负责注册/注销/统一渲染/开关状态持久化。
class PanelRegistry {
public:
    void Register(std::unique_ptr<IPanel> panel);   // 加载一个面板
    std::unique_ptr<IPanel> Unregister(const std::string& title); // 卸载并返回（可重新注册）
    IPanel* Find(const std::string& title);
    void DrawAll(EditorContext& ctx);   // 渲染所有可见面板（可见性由 VisibleFlag）
    void UpdateAll(float dt);
    void Shutdown();                    // 关闭所有面板（OnClose）
    std::vector<IPanel*>& Panels();
    // ImGui 菜单：每个 inMenu 面板一个勾选项（切换 VisibleFlag）。
    void DrawMenuItems();
private:
    std::vector<std::unique_ptr<IPanel>> panels_;
};

} // namespace neon::editor
```

### 3.3 面板独立化模式

每个面板从「EditorApp 成员函数 + 嵌套 struct」变成独立类：

```cpp
// editor/src/panels/asset_panel.hpp  （示例）
class AssetPanel : public IPanel {
public:
    const char* Title() const override { return "资产"; }
    bool* VisibleFlag() override { return &visible_; }
    void Draw(EditorContext& ctx) override;
    void OnOpen(EditorContext& ctx) override;
private:
    bool visible_ = true;
    // 原 EditorApp 的资产面板状态（assetDir_/assetFilter_/assetEntries_...）移到这里
    std::string assetDir_;
    int assetFilter_ = 0;
    std::vector<AssetEntry> entries_;
    // ...
};
```

**迁移规则**：
- 面板状态：从 `EditorApp::TerrainState` 等嵌套 struct → 面板类的成员。
- 面板方法：从 `EditorApp::BuildXxxPanel` → `IPanel::Draw`。
- 共享状态：`entities_`/`renderer_`/`assetMgr_`/`sceneWorld_`/`selected_` 等留在 EditorApp，经 `EditorContext` 传给面板。
- 跨面板操作：`setSelection`/`refreshAssetDir` 等注入为 `EditorContext` 回调。

### 3.4 EditorApp 门面化

- EditorApp 持有 `PanelRegistry panels_` + 共享状态（`entities_`/`renderer_`/`assetMgr_`/`sceneWorld_`/`selected_`/`assetVfs_` 等）。
- `OnCreate`：构建 `EditorContext`（注入共享状态 + 回调），`Register` 全部默认面板。
- `OnUpdate`/`OnRender`：`panels_.UpdateAll(dt)`/`panels_.DrawAll(ctx)`。
- 面板开关状态持久化（`RegisterPanelStateHandler`）改经 `PanelRegistry`。

## 4. 分阶段实施

- **阶段 1**：定义 `IPanel`/`EditorContext`/`PanelRegistry` + 拆 **4 个代表性面板**验证模式：
  场景面板 / 资产面板 / 属性面板 / 日志面板（状态清晰、依赖中等）。
- **阶段 2**：拆其余面板（资源/视口/模型预览/插件/导航/调试覆盖/UI 编辑器/本地化/性能/输入映射/脚本/世界/预览）。
- **阶段 3**：EditorApp 门面化收尾（`Panels()`/`PanelDef`/`RegisterPanelStateHandler` 迁移）+ 菜单整合。

每阶段独立 commit，`neon_editor --smoke-test 240` + 全量测试全绿（editor 不参与 neon_tests，靠冒烟 + editor 专项测试验证）。

## 5. 验收

1. `neon_editor --smoke-test 240` 通过（面板全开渲染无崩溃）。
2. 每个面板是独立 `IPanel` 类（`editor/src/panels/`），EditorApp 无面板成员函数/嵌套面板状态。
3. `editor.hpp` 从 1020 → <500 行（只留门面 + 共享状态）。
4. **可独立加载/卸载**：`PanelRegistry::Register/Unregister` 可用；新增面板 = 写一个 IPanel 子类注册即可（无需改 EditorApp 核心）。
5. 面板开/关状态持久化不回归。

## 6. 风险

- 面板对 EditorApp 私有状态的访问面大（20 面板 × 多个状态），`EditorContext` 需要聚合足够的共享状态/回调，否则面板退化回强耦合。
- 视口面板特殊（渲染 3D 视口 + gizmo + 输入），拆分需谨慎（可能作为"核心面板"保留特殊处理）。
- 脚本面板（含 TextEditor/调试器）状态多，是最大单个面板。
