#pragma once

// 插件管理面板 —— 面板插件化（阶段 2，Task 8，沿用 Task 2-7 样板）：
// 原 EditorApp::BuildPluginsPanel（editor_plugins.cpp）+ 面板私有状态
// （nativePlugins_/nativePluginsDir_，editor.hpp:877-878）整体迁入本类。
//
// 注：EditorApp::BuildPluginPanels（渲染"插件注册的动态面板"）不属于本类——
// 它是 EditorPluginManager::Panels() 的集合转发（editor_ui.cpp:99-100 同样用），
// 独立于 IPanel 体系，保留在 EditorApp。
//
// 可见标志在过渡期指向 EditorApp::showPlugins_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <memory>
#include <string>
#include <vector>

#include "editor_context.hpp"
#include "neon/plugin/native.hpp"

namespace neon::editor {

class PluginsPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showPlugins_）。
    explicit PluginsPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "插件"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showPlugins_
    // G5-1：<project>/plugins 下的原生二进制插件（DLL/SO），懒加载后列出其
    // ABI 信息。原 EditorApp::nativePlugins_ / nativePluginsDir_。
    std::vector<std::unique_ptr<plugin::NativePlugin>> nativePlugins_;
    std::string nativePluginsDir_;
};

} // namespace neon::editor
