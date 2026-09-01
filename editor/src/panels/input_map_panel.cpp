#include "panels/input_map_panel.hpp"

// 输入映射面板实现 = 原 EditorApp::BuildInputMapPanel（editor_ui.cpp:479-573）
// 方法体逐行迁移：EditorApp 成员（showInputMap_/inputMapState_/projectDir_ +
// LoadInputMapEdit/SaveInputMapEdit）改本类 visible_ / ctx.inputMap / ctx.projectDir
// / ctx 回调。行为零变化。

#include <algorithm>
#include <cstring>
#include <string>

#include "editor.hpp"
#include "imgui.h"
#include "neon/script/input_map.hpp"

namespace neon::editor {

void InputMapPanel::Draw(EditorContext& ctx) {
    InputMapState& im = *ctx.inputMap;
    if (!visible_ || !*visible_) return;
    if (!ImGui::Begin("输入映射", visible_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("项目: %s/input.json", ctx.projectDir->c_str());
    ImGui::SameLine();
    if (ImGui::Button("重新加载")) ctx.loadInputMapEdit();
    ImGui::SameLine();
    if (ImGui::Button("保存")) ctx.saveInputMapEdit();
    ImGui::Separator();
    if (ImGui::BeginTable("##inputmap", 3, ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("动作");
        ImGui::TableSetupColumn("按键");
        ImGui::TableSetupColumn("绑定");
        ImGui::TableHeadersRow();
        for (const std::string& name : im.edit.Names()) {
            const script::InputAction* a = im.edit.Find(name);
            if (!a) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(1);
            std::string keys;
            for (platform::Key k : a->positive)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k) + "+";
            for (platform::Key k : a->negative)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k) + "-";
            for (platform::Key k : a->keys)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k);
            ImGui::TextUnformatted(keys.empty() ? "(无)" : keys.c_str());
            ImGui::TableSetColumnIndex(2);
            const bool listening = im.listenAction == name;
            if (ImGui::Button(listening ? "等待按键..." : "改键", ImVec2(92.0f, 0.0f))) {
                im.listenAction = listening ? "" : name;
            }
        }
        ImGui::EndTable();
    }
    if (!im.listenAction.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           "请按一个新按键绑定到 '%s'...", im.listenAction.c_str());
    ImGui::Separator();
    ImGui::TextDisabled("时序规则 (G7-3): 双击窗口/长按阈值 (毫秒, 0=关); 修饰键为需要按住的和弦");
    for (const std::string& name : im.edit.Names()) {
        script::InputAction* a = im.edit.FindMutable(name);
        if (!a) continue;
        if (ImGui::TreeNode(name.c_str())) {
            int dtMs = static_cast<int>(a->doubleTapMs);
            if (ImGui::SliderInt("双击窗口 (ms)", &dtMs, 0, 1000, "%d",
                                 ImGuiSliderFlags_None)) {
                a->doubleTapMs = static_cast<uint32_t>(dtMs);
            }
            int lpMs = static_cast<int>(a->longPressMs);
            if (ImGui::SliderInt("长按阈值 (ms)", &lpMs, 0, 2000, "%d",
                                 ImGuiSliderFlags_None)) {
                a->longPressMs = static_cast<uint32_t>(lpMs);
            }
            std::string mods;
            for (size_t i = 0; i < a->modifiers.size(); ++i)
                mods += (i == 0 ? "" : ", ") + script::InputMap::KeyToName(a->modifiers[i]);
            char buf[128] = {};
            std::strncpy(buf, mods.c_str(), sizeof(buf) - 1);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputText("修饰键", buf, sizeof(buf))) {
                a->modifiers.clear();
                std::string cur;
                for (char c : std::string(buf)) {
                    if (c == ',' || c == ' ') {
                        if (!cur.empty()) {
                            const platform::Key k = script::InputMap::KeyFromName(cur);
                            if (k != platform::Key::Unknown &&
                                std::find(a->modifiers.begin(), a->modifiers.end(), k) ==
                                    a->modifiers.end())
                                a->modifiers.push_back(k);
                            cur.clear();
                        }
                    } else {
                        cur += c;
                    }
                }
                if (!cur.empty()) {
                    const platform::Key k = script::InputMap::KeyFromName(cur);
                    if (k != platform::Key::Unknown &&
                        std::find(a->modifiers.begin(), a->modifiers.end(), k) ==
                            a->modifiers.end())
                        a->modifiers.push_back(k);
                }
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

} // namespace neon::editor
