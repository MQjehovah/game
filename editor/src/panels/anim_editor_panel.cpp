#include "panels/anim_editor_panel.hpp"

// 动画时间线编辑器实现 = 原 EditorApp::BuildAnimEditorPanel（panels_script.inc:16-183）
// 方法体逐行迁移：anim_.xxx 成员访问照搬（本类成员 anim_），showAnimEditor_ →
// visible_。行为零变化。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

#include "editor.hpp"
#include "imgui.h"
#include "neon/anim/anim.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

void AnimEditorPanel::Draw(EditorContext&) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("动画时间线", visible_)) {
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##anim_path", anim_.pathBuf, sizeof(anim_.pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(anim_.pathBuf, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadClipJson(text);
                if (res.Ok()) {
                    anim_.clip = res.Value();
                    anim_.clipPath = anim_.pathBuf;
                    anim_.playhead = 0.0f;
                    anim_.playing = false;
                    anim_.clipDirty = false;
                } else {
                    NEON_LOG_ERROR("Anim editor: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !anim_.clipPath.empty()) {
            if (std::ofstream out(anim_.clipPath, std::ios::binary); out.is_open()) {
                out << anim::SaveClipJson(anim_.clip);
                anim_.clipDirty = false;
                NEON_LOG_INFO("Anim editor: saved '%s'", anim_.clipPath.c_str());
            }
        }
        if (anim_.clipPath.empty()) {
            ImGui::TextDisabled("未打开 clip — 输入 .anim.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", anim_.clipPath.c_str());
        ImGui::SameLine();
        if (anim_.clipDirty) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", anim_.clip.name.c_str());
        if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
            anim_.clip.name = nameBuf;
            anim_.clipDirty = true;
        }
        if (ImGui::DragFloat("时长 (秒)", &anim_.clip.duration, 0.01f, 0.01f, 1000.0f)) {
            anim_.clip.duration = std::fmax(anim_.clip.duration, 0.01f);
            anim_.clipDirty = true;
        }

        // Playhead transport.
        if (ImGui::Button(anim_.playing ? "暂停" : "播放")) anim_.playing = !anim_.playing;
        ImGui::SameLine();
        if (ImGui::Button("回到起点")) {
            anim_.playhead = 0.0f;
            anim_.playing = false;
        }
        ImGui::SameLine();
        if (ImGui::DragFloat("时间", &anim_.playhead, 0.01f, 0.0f, anim_.clip.duration)) {
            anim_.playhead = std::fmax(0.0f, anim_.playhead);
            anim_.clipDirty = true;
        }
        if (anim_.playing) {
            anim_.playhead += ImGui::GetIO().DeltaTime;
            if (anim_.playhead > anim_.clip.duration) anim_.playhead = 0.0f;
        }

        ImGui::Separator();
        if (ImGui::Button("添加轨道")) {
            anim::Track tr;
            tr.bone = static_cast<int>(anim_.clip.tracks.size());
            anim_.clip.tracks.push_back(std::move(tr));
            anim_.clipDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("移除最后轨道") && !anim_.clip.tracks.empty()) {
            anim_.clip.tracks.pop_back();
            anim_.clipDirty = true;
        }
        ImGui::Separator();

        const char* kInterp[] = {"linear", "step", "cubic"};
        for (size_t ti = 0; ti < anim_.clip.tracks.size(); ++ti) {
            anim::Track& tr = anim_.clip.tracks[ti];
            char header[64];
            std::snprintf(header, sizeof(header), "轨道 %zu##anim_tr%zu", ti, ti);
            if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(static_cast<int>(ti));
            if (ImGui::InputInt("骨骼", &tr.bone)) anim_.clipDirty = true;
            int ip = tr.interp == anim::Interp::Step
                         ? 1
                         : (tr.interp == anim::Interp::CubicSpline ? 2 : 0);
            if (ImGui::Combo("插值", &ip, kInterp, 3)) {
                tr.interp = ip == 1 ? anim::Interp::Step
                                    : (ip == 2 ? anim::Interp::CubicSpline : anim::Interp::Linear);
                anim_.clipDirty = true;
            }
            // Keyframe rows: translations / rotations / scales share times.
            auto keyRow = [&](const char* label, std::vector<math::Vec3>& values, size_t comps) {
                if (values.size() != tr.times.size()) values.resize(tr.times.size());
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::SmallButton("在此处添加关键帧")) {
                        tr.times.push_back(anim_.playhead);
                        values.push_back(comps == 4 ? math::Vec3{0, 0, 0} : math::Vec3{});
                        anim_.clipDirty = true;
                    }
                    for (size_t k = 0; k < tr.times.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        float t = tr.times[k];
                        if (ImGui::DragFloat("时间", &t, 0.01f, 0.0f, anim_.clip.duration)) {
                            tr.times[k] = std::fmax(0.0f, t);
                            anim_.clipDirty = true;
                        }
                        float v[3] = {values[k].x, values[k].y, values[k].z};
                        if (ImGui::DragFloat3("值", v, 0.01f)) {
                            values[k] = {v[0], v[1], v[2]};
                            anim_.clipDirty = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("删除")) {
                            tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                            values.erase(values.begin() + static_cast<ptrdiff_t>(k));
                            anim_.clipDirty = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            };
            keyRow("位移", tr.translations, 3);
            if (ImGui::TreeNodeEx("旋转##r", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (tr.rotations.size() != tr.times.size()) tr.rotations.resize(tr.times.size());
                if (ImGui::SmallButton("在此处添加关键帧")) {
                    tr.times.push_back(anim_.playhead);
                    tr.rotations.push_back({0, 0, 0, 1});
                    anim_.clipDirty = true;
                }
                for (size_t k = 0; k < tr.times.size(); ++k) {
                    ImGui::PushID(static_cast<int>(k + 1000));
                    float q[4] = {tr.rotations[k].x, tr.rotations[k].y,
                                  tr.rotations[k].z, tr.rotations[k].w};
                    if (ImGui::DragFloat4("四元数", q, 0.01f)) {
                        tr.rotations[k] = {q[0], q[1], q[2], q[3]};
                        tr.rotations[k] = tr.rotations[k].Normalized();
                        anim_.clipDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("删除")) {
                        tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                        tr.rotations.erase(tr.rotations.begin() + static_cast<ptrdiff_t>(k));
                        anim_.clipDirty = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            keyRow("缩放", tr.scales, 3);
            ImGui::PopID();
            ImGui::Separator();
        }
    }
    ImGui::End();
}

} // namespace neon::editor
