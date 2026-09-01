#pragma once

// 打包面板 —— 面板插件化（阶段 2，Task 14，沿用 Task 2-13 样板）：
// 原 EditorApp::BuildPackagePanel（panels_world.inc）整体迁入本类。
//
// 状态边界：PackageState（outDirBuf/report/ran）只被本面板用，迁为本类成员；
// 项目目录经 ctx.projectDir 读写（projectDirBuf_ 仍由 EditorApp 持有，顶部
// 工具栏共用）；SaveEditorConfig/RunPackage 是 EditorApp 方法，经 ctx 回调。
// 可见标志在过渡期指向 EditorApp::showPackage_（阶段 3 收编后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <array>
#include <string>

#include "editor_context.hpp"
#include "neon/core/pack.hpp"

namespace neon::editor {

class PackagePanel : public IPanel {
public:
    explicit PackagePanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "打包"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showPackage_
    // 原 EditorApp::PackageState（outDirBuf/report/ran）逐字段迁入。
    char outDirBuf[4096]{};      // output dir for the pack ("" = none yet)
    pack::PackageReport report;  // last run's report
    bool ran = false;            // the 打包 button ran at least once
};

} // namespace neon::editor
