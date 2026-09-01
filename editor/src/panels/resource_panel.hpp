#pragma once

// 资源面板 —— 面板插件化（阶段 2）的第一个迁移（沿用 Task 2-5 样板）：
// 原 EditorApp::BuildResourcePanel（panels_inspector.inc）整体迁入本类。
//
// 状态边界：
// - 数据源是 assets::AssetManager 的统计与缓存枚举（Stats/Textures/Meshes/Fonts），
//   经 EditorContext::assetMgr 访问，本面板无自有持久状态，EditorContext 零扩展；
// - 可见标志在过渡期指向 EditorApp::showResources_（窗口菜单勾选 + ini 持久化
//   + 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class ResourcePanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showResources_）。
    explicit ResourcePanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "资源"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showResources_
};

} // namespace neon::editor
