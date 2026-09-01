#pragma once

// 本地化面板 —— 面板插件化（阶段 2，Task 11，沿用 Task 2-10 样板）：
// 原 EditorApp::BuildLocPanel（panels_ui.inc）+ LocState（editor.hpp）整体迁入本类。
// LocState 只在本地化面板用，无外部共享——edit/path/language 全部迁为本类成员。
// 可见标志在过渡期指向 EditorApp::showLoc_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <string>

#include "editor_context.hpp"
#include "neon/core/localization.hpp"

namespace neon::editor {

class LocPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showLoc_）。
    explicit LocPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "本地化"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showLoc_
    // 原 EditorApp::LocState（editor.hpp:643-647）逐字段迁入。
    core::Localization edit;
    std::string language = "zh";
    std::string path; // last loaded/saved locales file
};

} // namespace neon::editor
