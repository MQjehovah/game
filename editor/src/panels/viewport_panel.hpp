#pragma once

// 视口面板 —— 面板插件化（Task 18a，收尾）：
// 原 EditorApp::BuildViewportPanel（panels_debug.inc）迁入本类。
//
// 特殊面板：视口渲染 3D 视图（引擎主视图，非工具面板）。viewportRect /
// viewportScreenRect 是引擎视图矩形（OnRender/OnEvent 消费），仍由 EditorApp
// 持有，经 EditorContext 指针访问；gizmo / 打开模型 / 添加实体经 ctx 回调。
// 无自有可见标志（视口不可关闭，dock 常驻）。

#include "editor_context.hpp"

namespace neon::editor {

class ViewportPanel : public IPanel {
public:
    const char* Title() const override { return "视口"; }
    bool* VisibleFlag() override { return &kAlwaysVisible; } // 视口常显
    void Draw(EditorContext& ctx) override;

private:
    static bool kAlwaysVisible;
    bool dockFallbackDone_ = false; // 原 EditorApp::viewportDockFallbackDone_
};

} // namespace neon::editor
