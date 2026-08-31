void EditorApp::RunPackage() {
    pack::PackConfig cfg;
    cfg.projectDir = projectDir_;
    cfg.outDir = packOutDirBuf_;
    cfg.playerSource = "build/neon_game.exe";
    packReport_ = pack::PackProject(cfg);
    packRan_ = true;
    if (packReport_.ok) {
        NEON_LOG_INFO("Editor: packaged '%s' -> %s (%zu files, %zu bytes)",
                      projectDir_.c_str(), packReport_.packPath.c_str(),
                      packReport_.fileCount, packReport_.bytesWritten);
    } else {
        NEON_LOG_ERROR("Editor: package failed for '%s' (%zu errors)",
                       projectDir_.c_str(), packReport_.errors.size());
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
        if (scriptEditorPath_.empty()) {
            ImGui::TextDisabled("未打开脚本 — 在资产面板双击 .lua 打开");
            ImGui::End();
            return;
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            SaveScriptEditor();
        ImGui::TextDisabled("文件: %s", scriptEditorPath_.c_str());
        ImGui::SameLine();
        if (ImGui::Button("保存")) SaveScriptEditor();
        ImGui::SameLine();
        if (ImGui::Button("外部编辑器打开")) OpenInExternalEditor(scriptEditorPath_);
        ImGui::SameLine();
        if (scriptEditorDirty_) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.6f, 1.0f), "已保存");
        }
        if (scriptEditorCheck_.ok) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                               "✗ 语法错误 (行 %d): %s", scriptEditorCheck_.line,
                               scriptEditorCheck_.message.c_str());
        }
        ImGui::Separator();
        // 断点: F9 (或按钮) 在光标所在行切换; 行号栏红点由 TextEditor 绘制。
        std::set<int>& bps = scriptBreakpoints_[scriptEditorPath_];
        const int cursorLine = scriptEdit_.GetCursorPosition().mLine + 1; // 1-based
        const bool toggleF9 = ImGui::IsKeyPressed(ImGuiKey_F9, false) &&
                              ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (toggleF9) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            scriptBreakpointsDirty_ = true;
        }
        if (ImGui::SmallButton("本行断点 (F9)")) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            scriptBreakpointsDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("清空断点")) {
            bps.clear();
            scriptBreakpointsDirty_ = true;
        }
        if (!bps.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("行:");
            for (auto it = bps.begin(); it != bps.end();) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(*it));
                if (ImGui::SmallButton(("x" + std::to_string(*it)).c_str())) {
                    it = bps.erase(it);
                    scriptBreakpointsDirty_ = true;
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
            const auto cursor = scriptEdit_.GetCursorPosition();
            const std::string all = scriptEdit_.GetText();
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
            TextEditor::Breakpoints bpVis(scriptBreakpoints_[scriptEditorPath_].begin(),
                                          scriptBreakpoints_[scriptEditorPath_].end());
            scriptEdit_.SetBreakpoints(bpVis);
        }        const ImVec2 editSize = ImGui::GetContentRegionAvail();
        scriptEdit_.Render("##script_editor", editSize, true);
        if (scriptEdit_.IsTextChanged()) scriptEditorDirty_ = true;
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
        ImGui::InputText("##anim_path", animPathBuf_, sizeof(animPathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(animPathBuf_, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadClipJson(text);
                if (res.Ok()) {
                    animClip_ = res.Value();
                    animClipPath_ = animPathBuf_;
                    animPlayhead_ = 0.0f;
                    animPlaying_ = false;
                    animClipDirty_ = false;
                } else {
                    NEON_LOG_ERROR("Anim editor: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !animClipPath_.empty()) {
            if (std::ofstream out(animClipPath_, std::ios::binary); out.is_open()) {
                out << anim::SaveClipJson(animClip_);
                animClipDirty_ = false;
                NEON_LOG_INFO("Anim editor: saved '%s'", animClipPath_.c_str());
            }
        }
        if (animClipPath_.empty()) {
            ImGui::TextDisabled("未打开 clip — 输入 .anim.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", animClipPath_.c_str());
        ImGui::SameLine();
        if (animClipDirty_) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", animClip_.name.c_str());
        if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
            animClip_.name = nameBuf;
            animClipDirty_ = true;
        }
        if (ImGui::DragFloat("时长 (秒)", &animClip_.duration, 0.01f, 0.01f, 1000.0f)) {
            animClip_.duration = std::fmax(animClip_.duration, 0.01f);
            animClipDirty_ = true;
        }

        // Playhead transport.
        if (ImGui::Button(animPlaying_ ? "暂停" : "播放")) animPlaying_ = !animPlaying_;
        ImGui::SameLine();
        if (ImGui::Button("回到起点")) {
            animPlayhead_ = 0.0f;
            animPlaying_ = false;
        }
        ImGui::SameLine();
        if (ImGui::DragFloat("时间", &animPlayhead_, 0.01f, 0.0f, animClip_.duration)) {
            animPlayhead_ = std::fmax(0.0f, animPlayhead_);
            animClipDirty_ = true;
        }
        if (animPlaying_) {
            animPlayhead_ += ImGui::GetIO().DeltaTime;
            if (animPlayhead_ > animClip_.duration) animPlayhead_ = 0.0f;
        }

        ImGui::Separator();
        if (ImGui::Button("添加轨道")) {
            anim::Track tr;
            tr.bone = static_cast<int>(animClip_.tracks.size());
            animClip_.tracks.push_back(std::move(tr));
            animClipDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("移除最后轨道") && !animClip_.tracks.empty()) {
            animClip_.tracks.pop_back();
            animClipDirty_ = true;
        }
        ImGui::Separator();

        const char* kInterp[] = {"linear", "step", "cubic"};
        for (size_t ti = 0; ti < animClip_.tracks.size(); ++ti) {
            anim::Track& tr = animClip_.tracks[ti];
            char header[64];
            std::snprintf(header, sizeof(header), "轨道 %zu##anim_tr%zu", ti, ti);
            if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(static_cast<int>(ti));
            if (ImGui::InputInt("骨骼", &tr.bone)) animClipDirty_ = true;
            int ip = tr.interp == anim::Interp::Step
                         ? 1
                         : (tr.interp == anim::Interp::CubicSpline ? 2 : 0);
            if (ImGui::Combo("插值", &ip, kInterp, 3)) {
                tr.interp = ip == 1 ? anim::Interp::Step
                                    : (ip == 2 ? anim::Interp::CubicSpline : anim::Interp::Linear);
                animClipDirty_ = true;
            }
            // Keyframe rows: translations / rotations / scales share times.
            auto keyRow = [&](const char* label, std::vector<math::Vec3>& values, size_t comps) {
                if (values.size() != tr.times.size()) values.resize(tr.times.size());
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::SmallButton("在此处添加关键帧")) {
                        tr.times.push_back(animPlayhead_);
                        values.push_back(comps == 4 ? math::Vec3{0, 0, 0} : math::Vec3{});
                        animClipDirty_ = true;
                    }
                    for (size_t k = 0; k < tr.times.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        float t = tr.times[k];
                        if (ImGui::DragFloat("时间", &t, 0.01f, 0.0f, animClip_.duration)) {
                            tr.times[k] = std::fmax(0.0f, t);
                            animClipDirty_ = true;
                        }
                        float v[3] = {values[k].x, values[k].y, values[k].z};
                        if (ImGui::DragFloat3("值", v, 0.01f)) {
                            values[k] = {v[0], v[1], v[2]};
                            animClipDirty_ = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("删除")) {
                            tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                            values.erase(values.begin() + static_cast<ptrdiff_t>(k));
                            animClipDirty_ = true;
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
                    tr.times.push_back(animPlayhead_);
                    tr.rotations.push_back({0, 0, 0, 1});
                    animClipDirty_ = true;
                }
                for (size_t k = 0; k < tr.times.size(); ++k) {
                    ImGui::PushID(static_cast<int>(k + 1000));
                    float q[4] = {tr.rotations[k].x, tr.rotations[k].y,
                                  tr.rotations[k].z, tr.rotations[k].w};
                    if (ImGui::DragFloat4("四元数", q, 0.01f)) {
                        tr.rotations[k] = {q[0], q[1], q[2], q[3]};
                        tr.rotations[k] = tr.rotations[k].Normalized();
                        animClipDirty_ = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("删除")) {
                        tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                        tr.rotations.erase(tr.rotations.begin() + static_cast<ptrdiff_t>(k));
                        animClipDirty_ = true;
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
        ImGui::InputText("##asm_path", asmPathBuf_, sizeof(asmPathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(asmPathBuf_, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadStateMachineJson(text);
                if (res.Ok()) {
                    asm_ = res.Value();
                    asmPath_ = asmPathBuf_;
                    asmDirty_ = false;
                } else {
                    NEON_LOG_ERROR("状态机编辑器: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !asmPath_.empty()) {
            if (std::ofstream out(asmPath_, std::ios::binary); out.is_open()) {
                out << anim::SaveStateMachineJson(asm_);
                asmDirty_ = false;
                NEON_LOG_INFO("状态机编辑器: saved '%s'", asmPath_.c_str());
            }
        }
        if (asmPath_.empty()) {
            ImGui::TextDisabled("未打开状态机 — 输入 .asm.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", asmPath_.c_str());
        ImGui::SameLine();
        if (asmDirty_) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        // 状态 (state = clip name)
        ImGui::Separator();
        ImGui::Text("状态 (clip 名)");
        int removeState = -1;
        for (size_t si = 0; si < asm_.States().size(); ++si) {
            const std::string stName = asm_.States()[si].name;
            std::string clipName = asm_.States()[si].clipName;
            char nameBuf[128];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", stName.c_str());
            if (ImGui::InputText(("状态名##s" + std::to_string(si)).c_str(), nameBuf,
                                 sizeof(nameBuf))) {
                asm_.MutableStates()[si].name = nameBuf;
                asmDirty_ = true;
            }
            char clipBuf[256];
            std::snprintf(clipBuf, sizeof(clipBuf), "%s", clipName.c_str());
            if (ImGui::InputText(("clip##c" + std::to_string(si)).c_str(), clipBuf,
                                 sizeof(clipBuf))) {
                asm_.SetStateClipName(stName, clipBuf);
                asmDirty_ = true;
            }
            if (ImGui::Button(("移除##s" + std::to_string(si)).c_str()))
                removeState = static_cast<int>(si);
        }
        if (removeState >= 0) {
            auto& sts = asm_.MutableStates();
            sts.erase(sts.begin() + removeState);
            asmDirty_ = true;
        }
        if (ImGui::Button("添加状态")) {
            asm_.AddState("state" + std::to_string(asm_.States().size() + 1), nullptr);
            asmDirty_ = true;
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
        for (size_t ti = 0; ti < asm_.Transitions().size(); ++ti) {
            const std::string id = "##t" + std::to_string(ti);
            char fromBuf[128], toBuf[128], paramBuf[128];
            const anim::AnimTransition& tr = asm_.Transitions()[ti];
            std::snprintf(fromBuf, sizeof(fromBuf), "%s", tr.from.c_str());
            std::snprintf(toBuf, sizeof(toBuf), "%s", tr.to.c_str());
            std::snprintf(paramBuf, sizeof(paramBuf), "%s", tr.param.c_str());
            float threshold = tr.threshold, duration = tr.duration;
            if (ImGui::InputText(("从##f" + std::to_string(ti)).c_str(), fromBuf,
                                 sizeof(fromBuf))) {
                asm_.MutableTransitions()[ti].from = fromBuf;
                asmDirty_ = true;
            }
            if (ImGui::InputText(("到##to" + std::to_string(ti)).c_str(), toBuf,
                                 sizeof(toBuf))) {
                asm_.MutableTransitions()[ti].to = toBuf;
                asmDirty_ = true;
            }
            if (ImGui::InputText(("参数##p" + std::to_string(ti)).c_str(), paramBuf,
                                 sizeof(paramBuf))) {
                asm_.MutableTransitions()[ti].param = paramBuf;
                asmDirty_ = true;
            }
            if (ImGui::DragFloat(("阈值##th" + std::to_string(ti)).c_str(), &threshold,
                                 0.01f, -1000.0f, 1000.0f)) {
                asm_.MutableTransitions()[ti].threshold = threshold;
                asmDirty_ = true;
            }
            if (ImGui::DragFloat(("混合时长##du" + std::to_string(ti)).c_str(), &duration,
                                 0.01f, 0.0f, 10.0f)) {
                asm_.MutableTransitions()[ti].duration = duration;
                asmDirty_ = true;
            }
            if (ImGui::Button(("移除##t" + std::to_string(ti)).c_str()))
                removeTr = static_cast<int>(ti);
            ImGui::Separator();
        }
        if (removeTr >= 0) {
            asm_.MutableTransitions().erase(
                asm_.MutableTransitions().begin() + removeTr);
            asmDirty_ = true;
        }
        if (ImGui::Button("添加过渡")) {
            asm_.AddTransition("idle", "run", "speed", 0.5f, 0.2f);
            asmDirty_ = true;
        }

        // 参数
        ImGui::Separator();
        ImGui::Text("参数");
        std::string removeParam;
        std::vector<std::string> paramKeys;
        for (const auto& [k, v] : asm_.Params()) paramKeys.push_back(k);
        for (const std::string& k : paramKeys) {
            float value = asm_.Params().at(k);
            char keyBuf[128];
            std::snprintf(keyBuf, sizeof(keyBuf), "%s", k.c_str());
            if (ImGui::InputText(("参数名##k" + k).c_str(), keyBuf, sizeof(keyBuf))) {
                // rename: move value to the new key, erase the old.
                const float v = asm_.Params().at(k);
                asm_.SetParam(keyBuf, v);
                asm_.MutableParams().erase(k);
                asmDirty_ = true;
                continue;
            }
            if (ImGui::DragFloat(("值##v" + k).c_str(), &value, 0.01f, -1000.0f, 1000.0f)) {
                asm_.SetParam(k, value);
                asmDirty_ = true;
            }
            if (ImGui::Button(("移除##p" + k).c_str())) removeParam = k;
        }
        if (!removeParam.empty()) {
            asm_.MutableParams().erase(removeParam);
            asmDirty_ = true;
        }
        if (ImGui::Button("添加参数")) {
            asm_.SetParam("param" + std::to_string(asm_.Params().size() + 1), 0.0f);
            asmDirty_ = true;
        }
    }
    ImGui::End();
}

// P1-1 terrain brush panel: select a "terrain" entity, enable 雕刻, then
// click / drag in the viewport to raise or lower the heightmap.
