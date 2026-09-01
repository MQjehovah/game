#pragma once

// 脚本编辑器面板 —— 面板插件化（阶段 2，Task 15，沿用 Task 2-14 样板）：
// 原 EditorApp::BuildScriptEditorPanel（panels_script.inc）整体迁入本类。
//
// 状态边界：ScriptEditorState（含 TextEditor）提升为 neon::editor 共享结构
//（editor.hpp，因 TextEditor 依赖不提升到 ImGui-free 的 editor_context.hpp），
// 仍由 EditorApp 持有（OpenScriptFile / SaveScriptEditor / OnUpdate 断点同步共用），
// 经 EditorContext::scriptEditor 指针访问；Save/OpenExternal 经 ctx 回调；
// 播放调试器经 ctx.playScriptHost 回调；dock 恢复经 ctx.dockspaceId。
// 可见标志在过渡期指向 EditorApp::showScriptEditor_（阶段 3 收编后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class ScriptEditorPanel : public IPanel {
public:
    explicit ScriptEditorPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "脚本编辑器"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showScriptEditor_
    bool dockFallbackDone_ = false; // 原 EditorApp::scriptEditorDockFallbackDone_
};

} // namespace neon::editor
