void EditorApp::RunPackage() {
    pack::PackConfig cfg;
    cfg.projectDir = projectDir_;
    cfg.outDir = package_.outDirBuf;
    cfg.playerSource = "build/neon_game.exe";
    package_.report = pack::PackProject(cfg);
    package_.ran = true;
    if (package_.report.ok) {
        NEON_LOG_INFO("Editor: packaged '%s' -> %s (%zu files, %zu bytes)",
                      projectDir_.c_str(), package_.report.packPath.c_str(),
                      package_.report.fileCount, package_.report.bytesWritten);
    } else {
        NEON_LOG_ERROR("Editor: package failed for '%s' (%zu errors)",
                       projectDir_.c_str(), package_.report.errors.size());
    }
}

void EditorApp::BuildScriptEditorPanel() {
    if (!showScriptEditor_) return;
    if (ImGui::Begin("脚本编辑器", &showScriptEditor_)) {
        // Same fallback as the viewport: a session that lost the DockId (the
        // window floats over the bottom tabs / Inspector) is re-docked into
        // the bottom tab group on its first frame; a saved user layout with a
        // DockId is always restored as-is.
        if (!scriptEditorDockFallbackDone_) {
            scriptEditorDockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (dockspaceId_ && !win->DockId && !win->DockIsActive) {
                ImGuiDockNode* bottom = nullptr;
                if (ImGuiDockNode* central =
                        ImGui::DockBuilderGetCentralNode(dockspaceId_)) {
                    if (central->ParentNode && central->ParentNode->IsSplitNode()) {
                        ImGuiDockNode* parent = central->ParentNode;
                        bottom = (parent->ChildNodes[0] == central)
                                     ? parent->ChildNodes[1]
                                     : parent->ChildNodes[0];
                    }
                }
                if (bottom && !bottom->IsSplitNode()) {
                    ImGui::DockBuilderDockWindow("脚本编辑器", bottom->ID);
                    NEON_LOG_INFO(
                        "Editor: script editor DockId was lost; re-docked to the bottom tabs");
                }
            }
        }
        if (scriptEditor_.path.empty()) {
            ImGui::TextDisabled("未打开脚本 — 在资产面板双击 .lua 打开");
            ImGui::End();
            return;
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            SaveScriptEditor();
        ImGui::TextDisabled("文件: %s", scriptEditor_.path.c_str());
        ImGui::SameLine();
        if (ImGui::Button("保存")) SaveScriptEditor();
        ImGui::SameLine();
        if (ImGui::Button("外部编辑器打开")) OpenInExternalEditor(scriptEditor_.path);
        ImGui::SameLine();
        if (scriptEditor_.dirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.6f, 1.0f), "已保存");
        }
        if (scriptEditor_.check.ok) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                               "✗ 语法错误 (行 %d): %s", scriptEditor_.check.line,
                               scriptEditor_.check.message.c_str());
        }
        ImGui::Separator();
        // 断点: F9 (或按钮) 在光标所在行切换; 行号栏红点由 TextEditor 绘制。
        std::set<int>& bps = scriptEditor_.breakpoints[scriptEditor_.path];
        const int cursorLine = scriptEditor_.edit.GetCursorPosition().mLine + 1; // 1-based
        const bool toggleF9 = ImGui::IsKeyPressed(ImGuiKey_F9, false) &&
                              ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (toggleF9) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            scriptEditor_.breakpointsDirty = true;
        }
        if (ImGui::SmallButton("本行断点 (F9)")) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            scriptEditor_.breakpointsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("清空断点")) {
            bps.clear();
            scriptEditor_.breakpointsDirty = true;
        }
        if (!bps.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("行:");
            for (auto it = bps.begin(); it != bps.end();) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(*it));
                if (ImGui::SmallButton(("x" + std::to_string(*it)).c_str())) {
                    it = bps.erase(it);
                    scriptEditor_.breakpointsDirty = true;
                } else {
                    ++it;
                }
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::TextDisabled("(F9 切换光标行)");
        }
        {
            static const struct {
                const char* name;
                const char* doc;
            } kBindings[] = {
                {"Spawn", "Spawn(kind, pos[, scriptPath]) 生成实体"},
                {"Despawn", "Despawn(entity) 销毁实体"},
                {"GetPosition", "GetPosition(entity) -> x,y,z"},
                {"SetPosition", "SetPosition(entity, x,y,z)"},
                {"GetVar", "GetVar(name) 读全局变量"},
                {"SetVar", "SetVar(name, value) 写全局变量"},
                {"Raycast", "Raycast(x,y,z, dx,dy,dz, range) -> t,owner"},
                {"PhysicsAddSphere", "PhysicsAddSphere(x,y,z, r, dynamic, {mass,...}) -> id"},
                {"PhysicsAddBox", "PhysicsAddBox(cx,cy,cz, hx,hy,hz, dynamic, {..}) -> id"},
                {"PhysicsAddCharacter", "PhysicsAddCharacter(x,y,z, r, halfH, {layer,mask}) -> id"},
                {"PhysicsSetCharacterMove", "PhysicsSetCharacterMove(id, x,y,z) 角色移动速度"},
                {"PhysicsSetVelocity", "PhysicsSetVelocity(id, x,y,z)"},
                {"PhysicsGetVelocity", "PhysicsGetVelocity(id) -> x,y,z"},
                {"PhysicsSetPosition", "PhysicsSetPosition(id, x,y,z)"},
                {"PhysicsGetPosition", "PhysicsGetPosition(id) -> x,y,z"},
                {"PhysicsIsOnGround", "PhysicsIsOnGround(id) -> bool"},
                {"PhysicsCollisions", "PhysicsCollisions() -> [{a,b},...]"},
                {"PhysicsRemove", "PhysicsRemove(id)"},
                {"Tween", "Tween(entity, prop, fx,fy,fz, tx,ty,tz, time, easing) 属性动画"},
                {"GetEntitiesInGroup", "GetEntitiesInGroup('enemy') -> 实体表"},
                {"PlaySfx", "PlaySfx(name) 播放音效"},
                {"InputAxis", "InputAxis(name) 输入轴 (-1..1)"},
                {"InputKey", "InputKey(name) -> 按键是否按下"},
                {"ActionDown", "ActionDown(name) 动作按下"},
                {"ActionPressed", "ActionPressed(name) 动作按下沿"},
                {"ActionReleased", "ActionReleased(name) 动作抬起沿"},
                {"SetRotationY", "SetRotationY(entity, radians)"},
                {"GetHealth", "GetHealth(entity) -> hp"},
                {"SetHealth", "SetHealth(entity, hp)"},
                {"SpawnProjectile", "SpawnProjectile(pos, dir, speed, damage, life, caster)"},
                {"MeleeAttack", "MeleeAttack(pos, dir, range, arcDeg, damage) -> 命中数"},
                {"CastSkill", "CastSkill(name, pos, dir, caster) -> 结果"},
                {"SkillCooldown", "SkillCooldown(name, caster) -> 剩余秒"},
                {"AttackBox", "AttackBox(center, half, yaw, damage) -> 命中数"},
                {"ApplyStatus", "ApplyStatus(entity, name, duration, magnitude)"},
                {"HasStatus", "HasStatus(entity, name) -> bool"},
                {"StatusMagnitude", "StatusMagnitude(entity, name) -> 数值"},
                {"RemoveStatus", "RemoveStatus(entity, name)"},
                {"DrawRect", "DrawRect(x,y,w,h, r,g,b,a)"},
                {"DrawSprite", "DrawSprite(tex, x,y,w,h, flipX, flipY)"},
                {"DrawText", "DrawText(text, x,y, size, r,g,b,a)"},
                {"ReadText", "ReadText(path) 读数据文件"},
                {"WriteText", "WriteText(path, content) 写数据文件"},
                {"UIShow", "UIShow(path) 显示 UI 文档"},
                {"UIHide", "UIHide() 隐藏 UI"},
                {"UIClicked", "UIClicked(name) -> 按钮是否被点击"},
                {"Loc", "Loc(key) 本地化文本"},
                {"FindNamedEntity", "FindNamedEntity(name) -> entity"},
                {"SetVisible", "SetVisible(entity, bool)"},
                {"ChangeScene", "ChangeScene(path) 切换场景"},
                {"SignalConnect", "SignalConnect(name, fn) 连接信号"},
                {"SignalEmit", "SignalEmit(name, arg) 发射信号"},
            };
            // 光标所在词: 从行首取到光标位置, 回扫标识符字符。
            const auto cursor = scriptEditor_.edit.GetCursorPosition();
            const std::string all = scriptEditor_.edit.GetText();
            // 手动切出第 cursor.mLine 行的前 cursor.mColumn 字符 (GetText(区间) 为 private)。
            std::string lineText;
            {
                size_t ln = 0, i = 0;
                while (i < all.size() && ln < static_cast<size_t>(cursor.mLine)) {
                    if (all[i] == '\n') ++ln;
                    ++i;
                }
                lineText.reserve(static_cast<size_t>(cursor.mColumn));
                for (size_t k = 0; k < static_cast<size_t>(cursor.mColumn) && i < all.size(); ++k, ++i)
                    lineText += all[i];
            }
            std::string word;
            for (auto it = lineText.rbegin(); it != lineText.rend(); ++it) {
                const char c = *it;
                const bool ident = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                if (!ident) break;
                word.insert(word.begin(), c);
            }
            if (ImGui::CollapsingHeader("绑定参考 (按词过滤)",
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginChild("##binding_ref", ImVec2(0, 180), true)) {
                    for (const auto& b : kBindings) {
                        if (!word.empty() &&
                            std::strncmp(b.name, word.c_str(), word.size()) != 0)
                            continue;
                        ImGui::TextUnformatted(b.name);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", b.doc);
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
        }
        if (play_ && play_->DebuggerPaused()) {
            const script::IScriptHost::DebugFrame& f = play_->ScriptHost()->PausedFrame();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("⏸ 已暂停: %s 行 %d", f.script.c_str(), f.line);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("继续")) play_->ScriptHost()->DebuggerResume(false);
            ImGui::SameLine();
            if (ImGui::Button("单步")) play_->ScriptHost()->DebuggerResume(true);
            if (ImGui::CollapsingHeader("局部变量")) {
                if (f.locals.empty()) ImGui::TextDisabled("(无)");
                for (const auto& l : f.locals) {
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", l.value.c_str());
                }
            }
            if (ImGui::CollapsingHeader("调用栈")) {
                for (size_t i = 0; i < f.callstack.size(); ++i)
                    ImGui::Text("%zu. %s", i, f.callstack[i].c_str());
            }
            ImGui::Separator();
        }
        // 行号栏断点标记: 同步当前脚本的断点集到 TextEditor (红点可视化)。
        {
            TextEditor::Breakpoints bpVis(scriptEditor_.breakpoints[scriptEditor_.path].begin(),
                                          scriptEditor_.breakpoints[scriptEditor_.path].end());
            scriptEditor_.edit.SetBreakpoints(bpVis);
        }        const ImVec2 editSize = ImGui::GetContentRegionAvail();
        scriptEditor_.edit.Render("##script_editor", editSize, true);
        if (scriptEditor_.edit.IsTextChanged()) scriptEditor_.dirty = true;
    }
    ImGui::End();
}

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
