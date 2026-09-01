#include "panels/resource_panel.hpp"

// 资源面板实现 = 原 EditorApp::BuildResourcePanel（panels_inspector.inc）方法体
// 逐行迁移：EditorApp 成员访问（assetMgr_/showResources_）改 ctx.assetMgr / 本类
// visible_。数据源是 AssetManager 的统计与缓存枚举，行为零变化。

#include "imgui.h"
#include "neon/assets/asset_manager.hpp"

namespace neon::editor {

void ResourcePanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("资源", visible_)) {
        assets::AssetStats s = ctx.assetMgr->Stats();
        ImGui::Text("纹理 %zu | 网格 %zu | 字体 %zu", s.textures, s.meshes, s.fonts);
        ImGui::Text("纹理内存 %.2f MB | 三角形 %zu",
                    static_cast<double>(s.textureBytes) / (1024.0 * 1024.0),
                    s.meshTriangles);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##res_tabs")) {
            if (ImGui::BeginTabItem("纹理")) {
                ImGui::BeginChild("##res_tex");
                for (const auto& kv : ctx.assetMgr->Textures()) {
                    if (!kv.second.Valid()) continue;
                    // Strip the load-option cache suffix ("\x1Ff" = glTF flip)
                    // when displaying the asset path.
                    std::string display = kv.first;
                    const size_t sep = display.find('\x1F');
                    if (sep != std::string::npos) display = display.substr(0, sep);
                    ImGui::Text("%s", display.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%dx%d", kv.second.Width(), kv.second.Height());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("网格")) {
                ImGui::BeginChild("##res_mesh");
                for (const auto& kv : ctx.assetMgr->Meshes()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s", kv.first.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%u 三角", kv.second.TriangleCount());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("字体")) {
                ImGui::BeginChild("##res_font");
                for (const auto& kv : ctx.assetMgr->Fonts()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s (%dpx)", kv.first.first.c_str(), kv.first.second);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace neon::editor
