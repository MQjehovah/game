#pragma once

// 面板插件化（阶段 1）核心抽象：EditorContext + IPanel。
// 纯新增——现有面板仍走 EditorApp::BuildXxxPanel；后续阶段逐个迁移。
//
// ImGui 边界：本头文件（与 PanelRegistry）刻意不包含任何 ImGui 头，
// neon_editor_common 保持 "Self-contained — no EditorApp / ImGui dependency"
//（见 CMakeLists.txt），单测可在无 ImGui context 的环境下运行。

#include <cstdint>
#include <array>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "history.hpp"
#include "neon/anim/anim.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/core/pack.hpp"
#include "neon/core/time.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/script/input_map.hpp"
#include "neon/script/script.hpp"
#include "neon/ui/document.hpp"
// scene::ComponentRegistry 定义在 scene_file.hpp（不在 component_schema.hpp）。
#include "neon/scene/scene_file.hpp"

namespace neon::editor {

// UI 文档编辑器状态。原 EditorApp::UI 编辑器私有成员（uiFiles_/uiDocPath_/
// uiDoc_/uiDocOpen_/uiSelected_/uiSelection_/uiSnapToGrid_/uiGridSize_/
// uiDirty_/uiDragging_/uiResizeHandle_/uiDragPos_），Task 16 提升为共享结构：
// UI 编辑器面板（BuildUIEditorPanel）与视口交互（UpdateUIEditorViewport /
// editor_viewport 的 UI 拖拽）共用，EditorApp 仍持有 ui_ 实例，经
// EditorContext::uiEditor 指针访问。辅助方法（UISelectNode 等）保留 EditorApp，
// 经 ctx 回调访问。
struct UiEditorState {
    std::vector<std::string> uiFiles; // ui/*.ui.json in the active project
    std::string uiDocPath;            // absolute path of the open document
    ui::UiDocument uiDoc;             // document being edited
    bool uiDocOpen = false;           // a document is loaded/created
    ui::UiNode* uiSelected = nullptr; // selected node (owned by uiDoc)
    std::set<ui::UiNode*> uiSelection; // multi-selection (active = uiSelected)
    bool uiSnapToGrid = true;
    float uiGridSize = 8.0f;
    bool uiDirty = false;
    bool uiDragging = false;
    int uiResizeHandle = -1;          // -1 none, 0..3 corner handles
    math::Vec2 uiDragPos{0.0f, 0.0f}; // mouse in design space
};

// 动画时间线 / 状态机编辑器状态。原 EditorApp::AnimEditorState / AsmEditorState
// 嵌套结构，Task 17 提升为共享结构（仅动画/状态机编辑器面板使用，全迁入面板）。
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

// 输入映射编辑状态。原 EditorApp::InputMapState 嵌套结构，Task 13 提升为共享
// 结构：面板编辑，OnEvent（editor.cpp 监听按键写回 listenAction）也读写，故
// EditorApp 仍持有 inputMapState_ 实例，经 EditorContext::inputMap 指针访问。
struct InputMapState {
    script::InputMap edit;
    std::string listenAction; // "listening" action, "" = idle
};

// 地形雕刻状态。原 EditorApp::TerrainState 嵌套结构，Task 14 提升为共享结构：
// 视口雕刻交互（editor_viewport）与场景塑形（editor_scene）都读写它，故
// EditorApp 仍持有 terrain_ 实例，经 EditorContext::terrain 指针访问。
struct TerrainState {
    bool paintMode = false;
    float brushRadius = 5.0f;
    float brushStrength = 0.12f;
    bool raise = true;
    math::Vec3 hoverPos{};
    bool hoverValid = false;
};

// 性能面板的帧时间环形缓冲。原 EditorApp::ProfilerState 嵌套结构，Task 12 提升
// 为共享结构：面板写入，冒烟测试（editor_smoke.cpp）直接读 profiler_.ms，故
// EditorApp 仍持有 profiler_ 实例，经 EditorContext::profiler 指针访问。
struct ProfilerState {
    static constexpr int kSamples = 180; // profiler ring buffer size
    std::array<float, kSamples> ms{};
    int msHead = 0;
};

// 导航工具状态（A* 网格 + 起点/终点/路径）。原 EditorApp::NavState 嵌套结构，
// Task 9 提升为共享结构：被导航面板（panels/nav_panel）与调试覆盖层
// （DrawDebugOverlay 画导航格）共用，故经 EditorContext::nav 指针访问。
struct NavState {
    std::string assetPath;   // project nav/<name>.json ("" = unsaved)
    nav::NavGrid grid;
    math::Vec2 start{-5, -5}; // cell-space markers (invalid when < 0)
    math::Vec2 goal{-5, -5};
    std::vector<math::Vec2> path;
};

// 定义在 editor.hpp（依赖 ImGui/TextEditor 等编辑器内部头，这里只持有指针，
// 前向声明即可——std::vector<SceneEntity>* 不要求完整类型）。
struct SceneEntity;
// 同上：资产面板的条目表（定义在 editor.hpp）。
struct AssetEntry;
// 资产面板的插件素材源区块（定义在 editor_plugin.hpp；只经指针访问）。
class EditorPluginManager;
// 脚本编辑器状态（定义在 editor.hpp，含 TextEditor 依赖）；只经指针访问。
struct ScriptEditorState;
// 打包报告（定义在 editor/src/packager.hpp，neon::editor::pack 命名空间）；
// 经引用出入参，面板持完整类型，editor_context 只前向声明。
namespace pack { struct PackageReport; }
// 行为树编辑器画布图（定义在 bt_editor.hpp，纯模型无 ImGui；只经指针访问）。
namespace btgraph { class BtGraph; }

// 面板共享的编辑器上下文：聚合 EditorApp 暴露给面板的共享状态（指针）。
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
    // 导航工具状态（Task 9）：导航面板 + 调试覆盖层共用，EditorApp 持有，注入指针。
    NavState* nav = nullptr;
    // 调试覆盖层开关（Task 10）：被调试覆盖面板（勾选）与视口（画物理线框 /
    // DrawDebugOverlay 画图层）共用，EditorApp 持有，注入指针。
    bool* debugColliders = nullptr;
    bool* debugNavMesh = nullptr;
    bool* debugProbes = nullptr;
    bool* debugAudio = nullptr;
    // 性能面板（Task 12）：模拟时钟（帧时间/FPS）+ 帧时间环形缓冲 + 播放状态
    // 计数。profiler_ 与 profilerDrawn_ 仍由 EditorApp 持有（冒烟测试直接读），
    // 注入指针；播放统计经回调（play_ 是 GameRuntime 成员，面板不直接持有）。
    core::Time* time = nullptr;
    ProfilerState* profiler = nullptr;
    bool* profilerDrawn = nullptr;
    bool* playActive = nullptr;
    std::function<std::size_t()> playEntityCount;
    std::function<std::size_t()> playBodyCount;
    std::function<std::size_t()> playBtCount;
    std::function<std::size_t()> playScriptCount;
    // 输入映射面板（Task 13）：InputMapState 仍由 EditorApp 持有（OnEvent 监听
    // 按键写回 listenAction），注入指针；Load/Save 保留为 EditorApp 方法
    // （OnCreate 也调 Load），经回调访问。
    InputMapState* inputMap = nullptr;
    std::function<void()> loadInputMapEdit;
    std::function<void()> saveInputMapEdit;
    // 世界面板（Task 14）：地形雕刻状态（视口/场景塑形共用，EditorApp 持有）
    // + 地形网格重建 / 打包回调（EditorApp 方法）。
    TerrainState* terrain = nullptr;
    std::function<void(SceneEntity&)> rebuildTerrainMesh;
    // 打包：输出目录 + 报告出入参（面板持完整 PackageReport 类型，回调填充）。
    std::function<void(const char*, pack::PackageReport&)> runPackage;
    std::function<void()> saveEditorConfig;
    // 脚本面板（Task 15）：ScriptEditorState 仍由 EditorApp 持有（OpenScriptFile /
    // SaveScriptEditor / OnUpdate 断点同步共用），注入指针；保存经回调（冒烟
    // 测试也调 SaveScriptEditor）。
    ScriptEditorState* scriptEditor = nullptr;
    std::function<void()> saveScriptEditor;
    // 脚本面板的 dock 恢复 + 播放调试器访问（play_ 是 GameRuntime，面板不直接持有）。
    uint32_t* dockspaceId = nullptr;
    std::function<script::IScriptHost*()> playScriptHost;
    // UI 编辑器（Task 16）：UiEditorState 提升为共享结构（视口交互共用），
    // EditorApp 持有，注入指针；辅助方法（UISelectNode 等 + MarkUIDirty）保留
    // EditorApp（视口/冒烟共用），经回调访问。
    UiEditorState* uiEditor = nullptr;
    std::function<void(ui::UiNode*)> uiSelectNode;
    std::function<void(ui::UiNode*)> uiToggleSelectNode;
    std::function<void()> uiDeleteSelectedNodes;
    std::function<void()> uiDuplicateSelectedNodes;
    std::function<void(int)> uiAlignSelected;
    std::function<void()> markUiDirty;
    // 视口面板（Task 18a）：viewportRect/viewportScreenRect 是引擎视图矩形
    //（OnRender/OnEvent 用），仍由 EditorApp 持有，注入指针；gizmo/打开模型
    // /添加实体经回调。
    math::Rect2* viewportRect = nullptr;
    math::Rect2* viewportScreenRect = nullptr;
    std::function<void()> drawTransformGizmo;
    std::function<void(const std::string&)> openModelPreview;
    bool* showModelPreview = nullptr;
    // 视口提示行（相机标签/目标/距离 + 播放物理刚体数，playBodyCount 复用 Task 12）。
    int* viewCam = nullptr;        // ViewCam 枚举（0 透视, 1 顶视, 2 前视）
    math::Vec3* camTarget = nullptr;
    float* camDist = nullptr;
    // 行为树面板（Task 18b）：btGraph_ 由 EditorApp 持有（OnCreate 播种 + 冒烟
    // 测试直接读写），注入指针；BT 文件 IO（EditorApp 方法，冒烟测试也调）经
    // 回调；播放高亮（play_ 是 GameRuntime，面板不直接持有）经回调。
    btgraph::BtGraph* btGraph = nullptr;
    std::function<bool(const std::string&)> btLoadFromFile;
    std::function<bool(const std::string&)> btSaveToFile;
    std::function<std::string()> playActiveTreePath;
    // 跨面板操作（EditorApp 注入回调）
    std::function<void()> refreshAssetDir;
    std::function<void(int)> setSelection;
    // --- 场景面板（Task 2，迁移样板）扩展：后续面板按需复用 ----------------
    // 共享状态：多选集合（active 实体 = *selected）、撤销栈、后处理开关
    // （面板 UI 读写，视口/播放也消费——所以仍由 EditorApp 持有，注入指针）。
    std::set<int>* selection = nullptr;
    HistoryManager* history = nullptr;
    bool* postSsao = nullptr;
    float* postSsaoIntensity = nullptr;
    bool* postVolumetric = nullptr;
    float* postVolumetricIntensity = nullptr;
    bool* postSsr = nullptr;
    float* postSsrIntensity = nullptr;
    // 回调：实体级操作（全部经撤销栈，与原 EditorApp 方法行为一致）。
    std::function<void(const std::string&)> addEntity;       // meshKey 添加实体
    std::function<void(const std::string&)> addSpriteEntity; // 纹理 -> 2D 精灵实体
    std::function<bool(int)> isSelected;                     // 行选中高亮
    std::function<void(int)> toggleSelection;                // Ctrl 加/减选
    std::function<void(int)> selectRangeTo;                  // Shift 连选
    std::function<std::vector<int>()> selectedIndices;       // 选区快照（升序）
    std::function<void()> clampSelection;                    // 删除后清理越界选区
    std::function<void()> sortSceneTreeByName;               // 场景树按名称排序
    std::function<void()> normalizeEntityIds;                // 稳定实体 id（树/拖拽守卫）
    std::function<void(const std::string&)> savePrefab;      // 保存预置体模板
    // --- 资产面板（Task 3）扩展：浏览状态指针 + 资产操作回调 ----------------
    // 共享状态：当前浏览目录/条目表/过滤页签/网格视图/选中/Delete 键待处理标志
    // 仍由 EditorApp 拥有（核心 RefreshAssetDir/Import*/Create/DeleteSelectedAsset、
    // 资产目录监视与冒烟测试都直接读写它们），经指针共享。
    std::string* assetDir = nullptr;
    std::vector<AssetEntry>* assetEntries = nullptr;
    int* assetFilter = nullptr;           // 0 全部, 1 模型, 2 贴图, 3 脚本, 4 材质
    bool* assetGridView = nullptr;        // 缩略图网格 vs 列表
    int* selectedAsset = nullptr;
    bool* deleteAssetRequested = nullptr; // Delete 键布防 -> 面板下一帧消费
    // 插件素材源区块（可空；EditorApp 在 OnCreate 创建插件管理器后注入）。
    EditorPluginManager* pluginMgr = nullptr;
    // 回调：资产操作（原面板直接调用的 EditorApp 方法，行为一致）。
    std::function<void(const std::string&)> importAssetFile;      // 拷入当前浏览目录
    std::function<void(const std::string&, int)> createAssetFile; // 新建资产（kind 0..5）
    std::function<void()> deleteSelectedAsset;                    // 删除选中资产
    std::function<void(const std::string&)> importAssetPath;      // 双击/导入到场景/预览
    std::function<bool()> inPrefabsDir;                 // 浏览目录是否 prefabs（.json = 预置体）
    std::function<void(const std::string&)> openScriptEditor;     // .lua -> 内置脚本编辑器
    std::function<void(const std::string&)> openInExternalEditor; // 系统外部编辑器
    // --- 属性面板（Task 4）扩展：场景脏标志 + 预置体库 + 实体重指令回调 ----
    // 共享状态：场景脏标志（相机/光源等非撤销字段编辑置位，保存时清除）与
    // 预置体库（实例检测 + 重置为预制体的模板来源）仍由 EditorApp 拥有，
    // 注入指针。
    bool* sceneDirty = nullptr;
    scene::PrefabLibrary* prefabLib = nullptr;
    scene::SceneEnvironment* sceneEnvironment = nullptr;
    bool* hasSceneEnvironment = nullptr;
    scene::RenderStack* sceneRenderStack = nullptr;
    bool* hasSceneRenderStack = nullptr;
    // 回调：实体级重指令（原面板直接调用的 EditorApp 方法；这些方法被视口/
    // 播放/场景加载/冒烟测试等多处共用，故保留在 EditorApp，经回调访问，
    // 行为一致）。SceneEntity 定义在 editor.hpp（指针/引用即可，前向声明）。
    std::function<bool(SceneEntity&)> resolveMesh;             // meshKey -> 网格解析
    std::function<void(SceneEntity&)> applyMaterialParams;     // 展开材质参数到渲染材质
    std::function<SceneEntity(const std::string&, const math::Vec3&)> materializePrefab;
    std::function<void(SceneEntity&)> reloadEntityShader;      // 重编译自定义着色器
    std::function<void(const std::string&)> saveMaterialAsset; // 另存为材质球资产
    std::function<void(const std::string&)> applyMaterialAsset; // 材质球资产 -> 选中实体
    // 缩略图查询：登记/刷新渲染请求（幂等），返回缓存条目的 ImGui 纹理 id
    // （0 = 无条目或尚未生成）。ImTextureID 在本头文件刻意不可见（ImGui-free），
    // 以整数形式传递（ImTextureID 即 ImU64，0 == ImTextureID_Invalid）。
    std::function<std::uint64_t(const std::string&)> meshThumbnail;
    std::function<std::uint64_t(const std::string&)> materialThumbnail;
    // 原生文件对话框的 owner 窗口句柄（无窗口时回调可返回 nullptr）。
    std::function<void*()> nativeWindowHandle;
};

// 面板接口：独立状态 + ImGui 渲染 + 生命周期。
// 窗口包裹（ImGui::Begin/End）是各面板 Draw 自己的责任（面板自治），
// 注册表只做可见性分发——见 PanelRegistry::DrawAll。
struct IPanel {
    virtual ~IPanel() = default;
    virtual const char* Title() const = 0;
    virtual bool* VisibleFlag() = 0;             // 面板开/关（ImGui 窗口可见性）
    virtual void Draw(EditorContext& ctx) = 0;   // ImGui 渲染（含自己的 Begin/End）
    virtual void OnOpen(EditorContext&) {}       // 打开时（加载）
    virtual void OnClose() {}                    // 关闭时（卸载）
    virtual void OnUpdate(float) {}              // 非渲染更新（可选）
    virtual bool InMenu() const { return true; } // 是否出现在窗口菜单
};

} // namespace neon::editor
