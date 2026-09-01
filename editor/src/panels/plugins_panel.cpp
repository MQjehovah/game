#include "panels/plugins_panel.hpp"

// 插件管理面板实现 = 原 EditorApp::BuildPluginsPanel（editor_plugins.cpp:119-173）
// 方法体逐行迁移：EditorApp 成员（showPlugins_/pluginMgr_/nativePlugins_/
// nativePluginsDir_）改本类 visible_ / ctx.pluginMgr / 本类成员。行为零变化。

#include <utility>

#include "editor_plugin.hpp"
#include "imgui.h"
#include "neon/plugin/plugin.hpp"

namespace neon::editor {

void PluginsPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_ || !ctx.pluginMgr) return;
    if (ImGui::Begin("插件", visible_)) {
        ImGui::TextDisabled("全局插件目录: plugins/");
        ImGui::SameLine();
        if (ImGui::SmallButton("重新加载")) {
            ctx.pluginMgr->Load(".");
            // G5-1: refresh the native plugin list from the same global dir.
            nativePlugins_.clear();
            nativePluginsDir_.clear();
        }
        ImGui::Separator();
        const auto& manifests = ctx.pluginMgr->Manifests();
        if (manifests.empty()) {
            ImGui::TextDisabled("未发现编辑器插件 (type=editor)");
        }
        for (const plugin::PluginManifest& m : manifests) {
            ImGui::BulletText("%s  v%s  [%s/%s]", m.name.c_str(), m.version.c_str(),
                              m.backend.c_str(), plugin::PluginTypeName(m.type));
            ImGui::TextDisabled("  id: %s  entry: %s", m.id.c_str(), m.entry.c_str());
        }

        // G5-1: native binary plugins (DLL/SO) under the global plugins/. Loaded
        // lazily on first open or after 重新加载, then listed with their ABI
        // info; a module-specific API getter (e.g. physics world factory) is
        // resolved through the same plugin:: loader the runtime uses.
        ImGui::Separator();
        ImGui::TextDisabled("原生插件 (DLL/SO):");
        const std::string nativeDir = std::string(".");
        if (nativePluginsDir_ != nativeDir) {
            nativePlugins_ = plugin::LoadNativePlugins(nativeDir);
            nativePluginsDir_ = nativeDir;
        }
        if (nativePlugins_.empty()) {
            ImGui::TextDisabled("  (无 %s/plugins 下的 native 插件)", nativeDir.c_str());
        }
        for (const std::unique_ptr<plugin::NativePlugin>& p : nativePlugins_) {
            ImGui::BulletText("%s  v%s", p->Info().name ? p->Info().name : "?",
                              p->Info().version ? p->Info().version : "?");
            ImGui::TextDisabled("  api=%u  %s", p->Info().apiVersion, p->Path().c_str());
        }
        ImGui::Separator();
        if (!ctx.pluginMgr->Panels().empty()) {
            ImGui::TextDisabled("面板 (%zu):", ctx.pluginMgr->Panels().size());
            for (const editor::PluginPanel& p : ctx.pluginMgr->Panels())
                ImGui::TextDisabled("  %s", p.title.c_str());
        }
        if (!ctx.pluginMgr->AssetSources().empty()) {
            ImGui::TextDisabled("资产源 (%zu):", ctx.pluginMgr->AssetSources().size());
            for (const editor::PluginAssetSource& s : ctx.pluginMgr->AssetSources())
                ImGui::TextDisabled("  %s", s.name.c_str());
        }
    }
    ImGui::End();
}

} // namespace neon::editor
