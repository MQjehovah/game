#pragma once

// 输入映射面板 —— 面板插件化（阶段 2，Task 13，沿用 Task 2-12 样板）：
// 原 EditorApp::BuildInputMapPanel（editor_ui.cpp）整体迁入本类。
//
// 状态边界：InputMapState（edit/listenAction）仍由 EditorApp 持有（OnEvent
// 监听按键时写回 listenAction，editor.cpp），经 EditorContext::inputMap 指针
// 访问；Load/Save 保留为 EditorApp 方法（OnCreate 也调 Load），经
// ctx.loadInputMapEdit / saveInputMapEdit 回调访问。
// 可见标志在过渡期指向 EditorApp::showInputMap_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class InputMapPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showInputMap_）。
    explicit InputMapPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "输入映射"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showInputMap_
};

} // namespace neon::editor
