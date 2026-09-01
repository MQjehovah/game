#pragma once

// 面板插件化（阶段 1）核心抽象：EditorContext + IPanel。
// 纯新增——现有面板仍走 EditorApp::BuildXxxPanel；后续阶段逐个迁移。
//
// ImGui 边界：本头文件（与 PanelRegistry）刻意不包含任何 ImGui 头，
// neon_editor_common 保持 "Self-contained — no EditorApp / ImGui dependency"
//（见 CMakeLists.txt），单测可在无 ImGui context 的环境下运行。

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "history.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/renderer.hpp"
// scene::ComponentRegistry 定义在 scene_file.hpp（不在 component_schema.hpp）。
#include "neon/scene/scene_file.hpp"

namespace neon::editor {

// 定义在 editor.hpp（依赖 ImGui/TextEditor 等编辑器内部头，这里只持有指针，
// 前向声明即可——std::vector<SceneEntity>* 不要求完整类型）。
struct SceneEntity;
// 同上：资产面板的条目表（定义在 editor.hpp）。
struct AssetEntry;
// 资产面板的插件素材源区块（定义在 editor_plugin.hpp；只经指针访问）。
class EditorPluginManager;

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
    // 缩略图查询：登记/刷新渲染请求（幂等），返回缓存条目的 ImGui 纹理 id
    // （0 = 无条目或尚未生成）。ImTextureID 在本头文件刻意不可见（ImGui-free），
    // 以整数形式传递（ImTextureID 即 ImU64，0 == ImTextureID_Invalid）。
    std::function<std::uint64_t(const std::string&)> meshThumbnail;
    std::function<std::uint64_t(const std::string&)> materialThumbnail;
    // 原生文件对话框的 owner 窗口句柄（无窗口时回调可返回 nullptr）。
    std::function<void*()> nativeWindowHandle;
    // 过渡期逃生舱：面板迁移初期访问未进 ctx 的状态；阶段 3 移除。
    void* editorApp = nullptr;
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
