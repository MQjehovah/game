#pragma once

// 性能面板 —— 面板插件化（阶段 2，Task 12，沿用 Task 2-11 样板）：
// 原 EditorApp::BuildProfilerPanel（panels_debug.inc）整体迁入本类。
//
// 状态边界：帧时间环形缓冲 ProfilerState 与 profilerDrawn_ 仍由 EditorApp 持有
// （冒烟测试 editor_smoke.cpp:712-715 直接读 profiler_.ms / profilerDrawn_），
// 经 EditorContext::profiler / profilerDrawn 指针访问；模拟时钟经 ctx.time；
// 播放统计经 ctx.playActive + play*Count 回调。
// 可见标志在过渡期指向 EditorApp::showProfiler_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class ProfilerPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showProfiler_）。
    explicit ProfilerPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "性能"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showProfiler_
};

} // namespace neon::editor
