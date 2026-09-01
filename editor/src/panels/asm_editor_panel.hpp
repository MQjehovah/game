#pragma once

// 动画状态机编辑器 —— 面板插件化（Task 17，沿用 Task 2-16 样板）：
// 原 EditorApp::BuildStateMachineEditorPanel（panels_script.inc）整体迁入本类。
// AsmEditorState 提升为共享结构（editor_context.hpp），无外部访问，全迁入本类。
// 可见标志在过渡期指向 EditorApp::showStateMachineEditor_（阶段 3 收编后改为自有 bool）。

#include "editor_context.hpp"

namespace neon::editor {

class AsmEditorPanel : public IPanel {
public:
    explicit AsmEditorPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "动画状态机"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showStateMachineEditor_
    AsmEditorState asm_;        // 原 EditorApp::asmEdit_ 状态迁入
};

} // namespace neon::editor
