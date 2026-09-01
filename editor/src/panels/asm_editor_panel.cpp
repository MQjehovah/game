#include "panels/asm_editor_panel.hpp"

// 动画状态机编辑器实现 = 原 EditorApp::BuildStateMachineEditorPanel
//（panels_script.inc:189-360）方法体逐行迁移：asmEdit_.xxx → 本类成员 asm_.xxx，
// showStateMachineEditor_ → visible_，selected_/entities_ → ctx.selected/ctx.entities。
// 行为零变化。

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "editor.hpp"
#include "imgui.h"
#include "neon/anim/anim.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

void AsmEditorPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("动画状态机", visible_)) {
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##asm_path", asm_.pathBuf, sizeof(asm_.pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(asm_.pathBuf, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadStateMachineJson(text);
                if (res.Ok()) {
                    asm_.machine = res.Value();
                    asm_.path = asm_.pathBuf;
                    asm_.dirty = false;
                } else {
                    NEON_LOG_ERROR("状态机编辑器: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !asm_.path.empty()) {
            if (std::ofstream out(asm_.path, std::ios::binary); out.is_open()) {
                out << anim::SaveStateMachineJson(asm_.machine);
                asm_.dirty = false;
                NEON_LOG_INFO("状态机编辑器: saved '%s'", asm_.path.c_str());
            }
        }
        if (asm_.path.empty()) {
            ImGui::TextDisabled("未打开状态机 — 输入 .asm.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", asm_.path.c_str());
        ImGui::SameLine();
        if (asm_.dirty) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        // 状态 (state = clip name)
        ImGui::Separator();
        ImGui::Text("状态 (clip 名)");
        int removeState = -1;
        for (size_t si = 0; si < asm_.machine.States().size(); ++si) {
            const std::string stName = asm_.machine.States()[si].name;
            std::string clipName = asm_.machine.States()[si].clipName;
            char nameBuf[128];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", stName.c_str());
            if (ImGui::InputText(("状态名##s" + std::to_string(si)).c_str(), nameBuf,
                                 sizeof(nameBuf))) {
                asm_.machine.MutableStates()[si].name = nameBuf;
                asm_.dirty = true;
            }
            char clipBuf[256];
            std::snprintf(clipBuf, sizeof(clipBuf), "%s", clipName.c_str());
            if (ImGui::InputText(("clip##c" + std::to_string(si)).c_str(), clipBuf,
                                 sizeof(clipBuf))) {
                asm_.machine.SetStateClipName(stName, clipBuf);
                asm_.dirty = true;
            }
            if (ImGui::Button(("移除##s" + std::to_string(si)).c_str()))
                removeState = static_cast<int>(si);
        }
        if (removeState >= 0) {
            auto& sts = asm_.machine.MutableStates();
            sts.erase(sts.begin() + removeState);
            asm_.dirty = true;
        }
        if (ImGui::Button("添加状态")) {
            asm_.machine.AddState("state" + std::to_string(asm_.machine.States().size() + 1), nullptr);
            asm_.dirty = true;
        }
        // 从选中实体的蒙皮模型导入 clip 名（作为下拉提示）。
        const int sel = ctx.selected ? *ctx.selected : -1;
        if (sel >= 0 && sel < static_cast<int>(ctx.entities->size())) {
            const SceneEntity& se = (*ctx.entities)[static_cast<size_t>(sel)];
            if (se.skinned && !se.skinned->clips.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("选中模型 clip: ");
                ImGui::SameLine();
                for (const anim::AnimationClip& c : se.skinned->clips) {
                    ImGui::TextDisabled("%s ", c.name.c_str());
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
        }

        // 过渡 (transition = 参数阈值)
        ImGui::Separator();
        ImGui::Text("过渡 (参数阈值)");
        int removeTr = -1;
        for (size_t ti = 0; ti < asm_.machine.Transitions().size(); ++ti) {
            const std::string id = "##t" + std::to_string(ti);
            char fromBuf[128], toBuf[128], paramBuf[128];
            const anim::AnimTransition& tr = asm_.machine.Transitions()[ti];
            std::snprintf(fromBuf, sizeof(fromBuf), "%s", tr.from.c_str());
            std::snprintf(toBuf, sizeof(toBuf), "%s", tr.to.c_str());
            std::snprintf(paramBuf, sizeof(paramBuf), "%s", tr.param.c_str());
            float threshold = tr.threshold, duration = tr.duration;
            if (ImGui::InputText(("从##f" + std::to_string(ti)).c_str(), fromBuf,
                                 sizeof(fromBuf))) {
                asm_.machine.MutableTransitions()[ti].from = fromBuf;
                asm_.dirty = true;
            }
            if (ImGui::InputText(("到##to" + std::to_string(ti)).c_str(), toBuf,
                                 sizeof(toBuf))) {
                asm_.machine.MutableTransitions()[ti].to = toBuf;
                asm_.dirty = true;
            }
            if (ImGui::InputText(("参数##p" + std::to_string(ti)).c_str(), paramBuf,
                                 sizeof(paramBuf))) {
                asm_.machine.MutableTransitions()[ti].param = paramBuf;
                asm_.dirty = true;
            }
            if (ImGui::DragFloat(("阈值##th" + std::to_string(ti)).c_str(), &threshold,
                                 0.01f, -1000.0f, 1000.0f)) {
                asm_.machine.MutableTransitions()[ti].threshold = threshold;
                asm_.dirty = true;
            }
            if (ImGui::DragFloat(("混合时长##du" + std::to_string(ti)).c_str(), &duration,
                                 0.01f, 0.0f, 10.0f)) {
                asm_.machine.MutableTransitions()[ti].duration = duration;
                asm_.dirty = true;
            }
            if (ImGui::Button(("移除##t" + std::to_string(ti)).c_str()))
                removeTr = static_cast<int>(ti);
            ImGui::Separator();
        }
        if (removeTr >= 0) {
            asm_.machine.MutableTransitions().erase(
                asm_.machine.MutableTransitions().begin() + removeTr);
            asm_.dirty = true;
        }
        if (ImGui::Button("添加过渡")) {
            asm_.machine.AddTransition("idle", "run", "speed", 0.5f, 0.2f);
            asm_.dirty = true;
        }

        // 参数
        ImGui::Separator();
        ImGui::Text("参数");
        std::string removeParam;
        std::vector<std::string> paramKeys;
        for (const auto& [k, v] : asm_.machine.Params()) paramKeys.push_back(k);
        for (const std::string& k : paramKeys) {
            float value = asm_.machine.Params().at(k);
            char keyBuf[128];
            std::snprintf(keyBuf, sizeof(keyBuf), "%s", k.c_str());
            if (ImGui::InputText(("参数名##k" + k).c_str(), keyBuf, sizeof(keyBuf))) {
                // rename: move value to the new key, erase the old.
                const float v = asm_.machine.Params().at(k);
                asm_.machine.SetParam(keyBuf, v);
                asm_.machine.MutableParams().erase(k);
                asm_.dirty = true;
                continue;
            }
            if (ImGui::DragFloat(("值##v" + k).c_str(), &value, 0.01f, -1000.0f, 1000.0f)) {
                asm_.machine.SetParam(k, value);
                asm_.dirty = true;
            }
            if (ImGui::Button(("移除##p" + k).c_str())) removeParam = k;
        }
        if (!removeParam.empty()) {
            asm_.machine.MutableParams().erase(removeParam);
            asm_.dirty = true;
        }
        if (ImGui::Button("添加参数")) {
            asm_.machine.SetParam("param" + std::to_string(asm_.machine.Params().size() + 1), 0.0f);
            asm_.dirty = true;
        }
    }
    ImGui::End();
}

} // namespace neon::editor
