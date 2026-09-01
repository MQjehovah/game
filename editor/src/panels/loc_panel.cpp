#include "panels/loc_panel.hpp"

// 本地化面板实现 = 原 EditorApp::BuildLocPanel（panels_ui.inc:246-337）方法体
// 逐行迁移：EditorApp 成员（showLoc_/locState_/projectDir_）改本类 visible_ /
// 本类成员 / ctx.projectDir。函数内 static newKey 保留为函数内 static（同原）。
// 行为零变化。AssetEntry/ListDirectory 完整定义经 editor.hpp（同其它面板模式）。

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "editor.hpp"
#include "editor_util.hpp"
#include "imgui.h"
#include "neon/core/json.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

namespace {

// 保存时确保输出目录存在（原 panels.cpp 匿名命名空间的 MakeDirSingle；
// 与本面板同 TU 的本地副本，模式同其它面板）。
bool MakeDirSingle(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

} // namespace

void LocPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("本地化", visible_)) {
        if (ImGui::Button("加载项目字符串表")) {
            edit = core::Localization();
            std::vector<AssetEntry> files;
            if (ListDirectory(*ctx.projectDir + "/assets/locales", files)) {
                for (const AssetEntry& f : files) {
                    if (f.isDir) continue;
                    const std::string& n = f.name;
                    const bool isJson =
                        n.size() > 5 && (n.compare(n.size() - 5, 5, ".json") == 0 ||
                                         n.compare(n.size() - 5, 5, ".JSON") == 0);
                    if (!isJson) continue;
                    std::ifstream in(f.path, std::ios::binary);
                    if (!in.is_open()) continue;
                    std::string text((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    std::string err;
                    if (!edit.LoadTable(text, &err)) {
                        NEON_LOG_WARN("Loc: '%s' failed to load: %s", f.path.c_str(),
                                      err.c_str());
                        continue;
                    }
                    path = f.path;
                }
            }
            edit.SetLanguage(language);
            NEON_LOG_INFO("Loc: loaded project strings (%zu keys)", edit.Keys().size());
        }
        ImGui::SameLine();
        if (ImGui::Button("保存 (locales.json)")) {
            const std::string dir = *ctx.projectDir + "/assets/locales";
            MakeDirSingle(dir);
            const std::string pathOut = dir + "/locales.json";
            std::ofstream out(pathOut, std::ios::binary);
            if (out.is_open()) {
                out << core::JsonWriter::WritePretty(edit.ToJson());
                path = pathOut;
                NEON_LOG_INFO("Loc: saved -> %s", pathOut.c_str());
            } else {
                NEON_LOG_ERROR("Loc: cannot write '%s'", pathOut.c_str());
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("路径: %s", path.c_str());
        ImGui::Separator();

        const std::vector<std::string> langs = edit.Languages();
        if (!langs.empty()) {
            std::vector<const char*> langLabels;
            for (const std::string& l : langs) langLabels.push_back(l.c_str());
            int sel = 0;
            for (size_t i = 0; i < langs.size(); ++i)
                if (langs[i] == language) sel = static_cast<int>(i);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("语言", &sel, langLabels.data(),
                             static_cast<int>(langLabels.size()))) {
                language = langs[static_cast<size_t>(sel)];
                edit.SetLanguage(language);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("默认: %s", edit.DefaultLanguage().c_str());
        } else {
            ImGui::TextDisabled("未加载字符串表");
        }

        static char newKey[128] = {};
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputText("新键", newKey, sizeof(newKey));
        ImGui::SameLine();
        if (ImGui::Button("添加键") && newKey[0] != '\0') {
            edit.Set(language, newKey, "");
            newKey[0] = '\0';
        }
        ImGui::Separator();

        ImGui::BeginChild("##loc_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const std::vector<std::string> keys = edit.Keys();
        for (const std::string& key : keys) {
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s",
                          edit.GetIn(language, key).c_str());
            const std::string label = key + "##" + language;
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                edit.Set(language, key, buf);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace neon::editor
