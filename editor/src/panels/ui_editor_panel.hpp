#pragma once

// UI 编辑器面板 —— 面板插件化（阶段 2，Task 16，沿用 Task 2-15 样板）：
// 原 EditorApp::BuildUIEditorPanel（panels_ui.inc）整体迁入本类。
//
// 状态边界：UiEditorState 提升为共享结构（editor_context.hpp，ui:: 为 engine
// 类型可提升），仍由 EditorApp 持有（视口交互 UpdateUIEditorViewport 共用），
// 经 EditorContext::uiEditor 指针访问；辅助方法（UISelectNode/UIAlignSelected/
// MarkUIDirty 等）保留 EditorApp（视口/冒烟共用），经 ctx 回调访问。
// 可见标志在过渡期指向 EditorApp::showUIEditor_（阶段 3 收编后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class UIEditorPanel : public IPanel {
public:
    explicit UIEditorPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "UI 编辑器"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showUIEditor_
};

} // namespace neon::editor
