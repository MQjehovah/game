#pragma once

// 面板插件化（阶段 1）核心抽象：EditorContext + IPanel。
// 纯新增——现有面板仍走 EditorApp::BuildXxxPanel；后续阶段逐个迁移。
//
// ImGui 边界：本头文件（与 PanelRegistry）刻意不包含任何 ImGui 头，
// neon_editor_common 保持 "Self-contained — no EditorApp / ImGui dependency"
//（见 CMakeLists.txt），单测可在无 ImGui context 的环境下运行。

#include <functional>
#include <string>
#include <vector>

#include "neon/assets/asset_manager.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/renderer.hpp"
// scene::ComponentRegistry 定义在 scene_file.hpp（不在 component_schema.hpp）。
#include "neon/scene/scene_file.hpp"

namespace neon::editor {

// 定义在 editor.hpp（依赖 ImGui/TextEditor 等编辑器内部头，这里只持有指针，
// 前向声明即可——std::vector<SceneEntity>* 不要求完整类型）。
struct SceneEntity;

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
