#pragma once

// 日志面板 —— 面板插件化（阶段 1）的第四个迁移（沿用 Task 2-4 样板）：
// 原 EditorApp::BuildLogPanel（panels_debug.inc）整体迁入本类。
//
// 状态边界：
// - 日志数据源是 core::Log 的全局环形缓冲（GetRecentLogs/ClearLogs），并非
//   EditorApp 持有的缓冲——EditorApp 只有每帧刷新的显示缓存与两个 UI 偏好，
//   且仅本面板使用，故全部迁为本类成员，不经 EditorContext 共享；
// - 可见标志在过渡期指向 EditorApp::showLog_（窗口菜单勾选 + ini 持久化
//   + 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <vector>

#include "editor_context.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

class LogPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showLog_）。
    explicit LogPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "日志"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showLog_
    std::vector<core::LogEntry> logEntries_; // 每帧刷新的显示缓存（原 EditorApp::logEntries_）
    int logFilter_ = 0;         // 0 all, 1 info+, 2 warn+, 3 error（原 EditorApp::logFilter_）
    bool logAutoScroll_ = true; // 原 EditorApp::logAutoScroll_
    bool wasAtBottom_ = true;   // 自动滚动判定（原函数内 static wasAtBottom）
};

} // namespace neon::editor
