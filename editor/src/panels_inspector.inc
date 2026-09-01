void EditorApp::BuildResourcePanel() {
    if (!showResources_) return;
    if (ImGui::Begin("资源", &showResources_)) {
        assets::AssetStats s = assetMgr_.Stats();
        ImGui::Text("纹理 %zu | 网格 %zu | 字体 %zu", s.textures, s.meshes, s.fonts);
        ImGui::Text("纹理内存 %.2f MB | 三角形 %zu",
                    static_cast<double>(s.textureBytes) / (1024.0 * 1024.0),
                    s.meshTriangles);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##res_tabs")) {
            if (ImGui::BeginTabItem("纹理")) {
                ImGui::BeginChild("##res_tex");
                for (const auto& kv : assetMgr_.Textures()) {
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
                for (const auto& kv : assetMgr_.Meshes()) {
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
                for (const auto& kv : assetMgr_.Fonts()) {
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

// 属性面板（原 EditorApp::BuildInspectorPanel，本文件最大面板）已整体迁移为
// 独立面板类 panels/inspector_panel.hpp/.cpp（InspectorPanel : IPanel，Task 4，
// 沿用 Task 2/3 样板）。EditorApp 在 OnCreate 注册它；editor_ui.cpp 的
// BuildImGuiUI 经 panels_.DrawAll(ctx_) 分发。此占位注释保留迁移痕迹，
// 待资源面板等后续迁移完成后整体删除本 inc。
