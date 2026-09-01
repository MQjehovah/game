// RunPackage（原本文件开头）已迁至 editor.cpp（Task 14）：PackageState 迁入
// panels/package_panel.hpp，签名改为 RunPackage(const char* outDir) 返回报告。

// 脚本编辑器面板（原 EditorApp::BuildScriptEditorPanel）已整体迁移为独立面板类
// editor/src/panels/script_editor_panel.hpp/.cpp（ScriptEditorPanel : IPanel，
// Task 15，沿用 Task 2-14 样板）。ScriptEditorState 提升为共享结构（editor.hpp，
// 因 TextEditor 依赖不提升到 ImGui-free 的 editor_context.hpp）并仍由 EditorApp
// 持有（OpenScriptFile/SaveScriptEditor/OnUpdate 断点同步共用），经 ctx.scriptEditor
// 指针访问。EditorApp 在 OnCreate 注册它；editor_ui.cpp 经 panels_.DrawAll(ctx_)
// 分发。此占位注释保留迁移痕迹，待动画/状态机编辑器迁移完成后整体删除本 inc。

// P1-1 animation timeline editor: opens .anim.json clips (the anim module's
// data-driven clip format), scrubs a playhead, edits tracks/keyframes and
// saves. Playback advances the playhead but has no live mesh preview in the
// editor (clips play in the runtime animator / play).
void EditorApp::BuildAnimEditorPanel() {
    if (!showAnimEditor_) return;
    if (ImGui::Begin("动画时间线", &showAnimEditor_)) {
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

// G5-4-4(项2) data-driven state machine editor (.asm.json): author states
// (clip names), parameter transitions and params; save as an asset the runtime
// loads. "从选中模型导入" fills the clip-name list from the selected entity's
// skinned model so state clips resolve.
void EditorApp::BuildStateMachineEditorPanel() {
    if (!showStateMachineEditor_) return;
    if (ImGui::Begin("动画状态机", &showStateMachineEditor_)) {
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##asm_path", asmEdit_.pathBuf, sizeof(asmEdit_.pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(asmEdit_.pathBuf, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadStateMachineJson(text);
                if (res.Ok()) {
                    asmEdit_.machine = res.Value();
                    asmEdit_.path = asmEdit_.pathBuf;
                    asmEdit_.dirty = false;
                } else {
                    NEON_LOG_ERROR("状态机编辑器: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !asmEdit_.path.empty()) {
            if (std::ofstream out(asmEdit_.path, std::ios::binary); out.is_open()) {
                out << anim::SaveStateMachineJson(asmEdit_.machine);
                asmEdit_.dirty = false;
                NEON_LOG_INFO("状态机编辑器: saved '%s'", asmEdit_.path.c_str());
            }
        }
        if (asmEdit_.path.empty()) {
            ImGui::TextDisabled("未打开状态机 — 输入 .asm.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", asmEdit_.path.c_str());
        ImGui::SameLine();
        if (asmEdit_.dirty) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        // 状态 (state = clip name)
        ImGui::Separator();
        ImGui::Text("状态 (clip 名)");
        int removeState = -1;
        for (size_t si = 0; si < asmEdit_.machine.States().size(); ++si) {
            const std::string stName = asmEdit_.machine.States()[si].name;
            std::string clipName = asmEdit_.machine.States()[si].clipName;
            char nameBuf[128];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", stName.c_str());
            if (ImGui::InputText(("状态名##s" + std::to_string(si)).c_str(), nameBuf,
                                 sizeof(nameBuf))) {
                asmEdit_.machine.MutableStates()[si].name = nameBuf;
                asmEdit_.dirty = true;
            }
            char clipBuf[256];
            std::snprintf(clipBuf, sizeof(clipBuf), "%s", clipName.c_str());
            if (ImGui::InputText(("clip##c" + std::to_string(si)).c_str(), clipBuf,
                                 sizeof(clipBuf))) {
                asmEdit_.machine.SetStateClipName(stName, clipBuf);
                asmEdit_.dirty = true;
            }
            if (ImGui::Button(("移除##s" + std::to_string(si)).c_str()))
                removeState = static_cast<int>(si);
        }
        if (removeState >= 0) {
            auto& sts = asmEdit_.machine.MutableStates();
            sts.erase(sts.begin() + removeState);
            asmEdit_.dirty = true;
        }
        if (ImGui::Button("添加状态")) {
            asmEdit_.machine.AddState("state" + std::to_string(asmEdit_.machine.States().size() + 1), nullptr);
            asmEdit_.dirty = true;
        }
        // 从选中实体的蒙皮模型导入 clip 名（作为下拉提示）。
        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            const SceneEntity& se = entities_[static_cast<size_t>(selected_)];
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
        for (size_t ti = 0; ti < asmEdit_.machine.Transitions().size(); ++ti) {
            const std::string id = "##t" + std::to_string(ti);
            char fromBuf[128], toBuf[128], paramBuf[128];
            const anim::AnimTransition& tr = asmEdit_.machine.Transitions()[ti];
            std::snprintf(fromBuf, sizeof(fromBuf), "%s", tr.from.c_str());
            std::snprintf(toBuf, sizeof(toBuf), "%s", tr.to.c_str());
            std::snprintf(paramBuf, sizeof(paramBuf), "%s", tr.param.c_str());
            float threshold = tr.threshold, duration = tr.duration;
            if (ImGui::InputText(("从##f" + std::to_string(ti)).c_str(), fromBuf,
                                 sizeof(fromBuf))) {
                asmEdit_.machine.MutableTransitions()[ti].from = fromBuf;
                asmEdit_.dirty = true;
            }
            if (ImGui::InputText(("到##to" + std::to_string(ti)).c_str(), toBuf,
                                 sizeof(toBuf))) {
                asmEdit_.machine.MutableTransitions()[ti].to = toBuf;
                asmEdit_.dirty = true;
            }
            if (ImGui::InputText(("参数##p" + std::to_string(ti)).c_str(), paramBuf,
                                 sizeof(paramBuf))) {
                asmEdit_.machine.MutableTransitions()[ti].param = paramBuf;
                asmEdit_.dirty = true;
            }
            if (ImGui::DragFloat(("阈值##th" + std::to_string(ti)).c_str(), &threshold,
                                 0.01f, -1000.0f, 1000.0f)) {
                asmEdit_.machine.MutableTransitions()[ti].threshold = threshold;
                asmEdit_.dirty = true;
            }
            if (ImGui::DragFloat(("混合时长##du" + std::to_string(ti)).c_str(), &duration,
                                 0.01f, 0.0f, 10.0f)) {
                asmEdit_.machine.MutableTransitions()[ti].duration = duration;
                asmEdit_.dirty = true;
            }
            if (ImGui::Button(("移除##t" + std::to_string(ti)).c_str()))
                removeTr = static_cast<int>(ti);
            ImGui::Separator();
        }
        if (removeTr >= 0) {
            asmEdit_.machine.MutableTransitions().erase(
                asmEdit_.machine.MutableTransitions().begin() + removeTr);
            asmEdit_.dirty = true;
        }
        if (ImGui::Button("添加过渡")) {
            asmEdit_.machine.AddTransition("idle", "run", "speed", 0.5f, 0.2f);
            asmEdit_.dirty = true;
        }

        // 参数
        ImGui::Separator();
        ImGui::Text("参数");
        std::string removeParam;
        std::vector<std::string> paramKeys;
        for (const auto& [k, v] : asmEdit_.machine.Params()) paramKeys.push_back(k);
        for (const std::string& k : paramKeys) {
            float value = asmEdit_.machine.Params().at(k);
            char keyBuf[128];
            std::snprintf(keyBuf, sizeof(keyBuf), "%s", k.c_str());
            if (ImGui::InputText(("参数名##k" + k).c_str(), keyBuf, sizeof(keyBuf))) {
                // rename: move value to the new key, erase the old.
                const float v = asmEdit_.machine.Params().at(k);
                asmEdit_.machine.SetParam(keyBuf, v);
                asmEdit_.machine.MutableParams().erase(k);
                asmEdit_.dirty = true;
                continue;
            }
            if (ImGui::DragFloat(("值##v" + k).c_str(), &value, 0.01f, -1000.0f, 1000.0f)) {
                asmEdit_.machine.SetParam(k, value);
                asmEdit_.dirty = true;
            }
            if (ImGui::Button(("移除##p" + k).c_str())) removeParam = k;
        }
        if (!removeParam.empty()) {
            asmEdit_.machine.MutableParams().erase(removeParam);
            asmEdit_.dirty = true;
        }
        if (ImGui::Button("添加参数")) {
            asmEdit_.machine.SetParam("param" + std::to_string(asmEdit_.machine.Params().size() + 1), 0.0f);
            asmEdit_.dirty = true;
        }
    }
    ImGui::End();
}

// P1-1 terrain brush panel: select a "terrain" entity, enable 雕刻, then
// click / drag in the viewport to raise or lower the heightmap.
