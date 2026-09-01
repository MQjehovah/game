#pragma once

// 导航面板 —— 面板插件化（阶段 2，Task 9，沿用 Task 2-8 样板）：
// 原 EditorApp::BuildNavPanel（panels_debug.inc）整体迁入本类。
//
// 状态边界：导航工具状态（assetPath/grid/start/goal/path）是 NavState，被本面板
// 与调试覆盖层（DrawDebugOverlay 画导航格）共用——提升为 neon::editor::NavState
// （editor_context.hpp），EditorApp 持有，经 EditorContext::nav 指针访问。
// 可见标志在过渡期指向 EditorApp::showNav_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class NavPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showNav_）。
    explicit NavPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "导航"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showNav_
};

} // namespace neon::editor
