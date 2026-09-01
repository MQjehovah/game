#include "panels/script_editor_panel.hpp"

// 脚本编辑器实现 = 原 EditorApp::BuildScriptEditorPanel（panels_script.inc:4-218）
// 方法体逐行迁移：EditorApp 成员（showScriptEditor_/scriptEditorDockFallbackDone_/
// dockspaceId_/scriptEditor_ + SaveScriptEditor/OpenInExternalEditor/play_）改
// 本类 visible_/dockFallbackDone_ / ctx 指针 / ctx 回调。行为零变化。
// ScriptEditorState 完整类型经 editor.hpp；TextEditor 经 editor.hpp。

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>

#include "editor.hpp"
#include "imgui.h"
#include "neon/script/script.hpp"

namespace neon::editor {

void ScriptEditorPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("脚本编辑器", visible_)) {
        // Same fallback as the viewport: a session that lost the DockId (the
        // window floats over the bottom tabs / Inspector) is re-docked into
        // the bottom tab group on its first frame; a saved user layout with a
        // DockId is always restored as-is.
        if (!dockFallbackDone_) {
            dockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (*ctx.dockspaceId && !win->DockId && !win->DockIsActive) {
                ImGuiDockNode* bottom = nullptr;
                if (ImGuiDockNode* central =
                        ImGui::DockBuilderGetCentralNode(*ctx.dockspaceId)) {
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
        if (ctx.scriptEditor->path.empty()) {
            ImGui::TextDisabled("未打开脚本 — 在资产面板双击 .lua 打开");
            ImGui::End();
            return;
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            ctx.saveScriptEditor();
        ImGui::TextDisabled("文件: %s", ctx.scriptEditor->path.c_str());
        ImGui::SameLine();
        if (ImGui::Button("保存")) ctx.saveScriptEditor();
        ImGui::SameLine();
        if (ImGui::Button("外部编辑器打开")) ctx.openInExternalEditor(ctx.scriptEditor->path);
        ImGui::SameLine();
        if (ctx.scriptEditor->dirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.6f, 1.0f), "已保存");
        }
        if (ctx.scriptEditor->check.ok) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                               "✗ 语法错误 (行 %d): %s", ctx.scriptEditor->check.line,
                               ctx.scriptEditor->check.message.c_str());
        }
        ImGui::Separator();
        // 断点: F9 (或按钮) 在光标所在行切换; 行号栏红点由 TextEditor 绘制。
        std::set<int>& bps = ctx.scriptEditor->breakpoints[ctx.scriptEditor->path];
        const int cursorLine = ctx.scriptEditor->edit.GetCursorPosition().mLine + 1; // 1-based
        const bool toggleF9 = ImGui::IsKeyPressed(ImGuiKey_F9, false) &&
                              ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (toggleF9) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            ctx.scriptEditor->breakpointsDirty = true;
        }
        if (ImGui::SmallButton("本行断点 (F9)")) {
            if (bps.count(cursorLine)) bps.erase(cursorLine); else bps.insert(cursorLine);
            ctx.scriptEditor->breakpointsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("清空断点")) {
            bps.clear();
            ctx.scriptEditor->breakpointsDirty = true;
        }
        if (!bps.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("行:");
            for (auto it = bps.begin(); it != bps.end();) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(*it));
                if (ImGui::SmallButton(("x" + std::to_string(*it)).c_str())) {
                    it = bps.erase(it);
                    ctx.scriptEditor->breakpointsDirty = true;
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
                {"ApplyStatus", "Gameplay.ApplyStatus(entity, name, duration, magnitude)"},
                {"HasStatus", "Gameplay.HasStatus(entity, name) -> bool"},
                {"StatusMagnitude", "Gameplay.StatusMagnitude(entity, name) -> 数值"},
                {"RemoveStatus", "Gameplay.RemoveStatus(entity, name)"},
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
            const auto cursor = ctx.scriptEditor->edit.GetCursorPosition();
            const std::string all = ctx.scriptEditor->edit.GetText();
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
        script::IScriptHost* playHost = ctx.playScriptHost();
        if (playHost && playHost->DebuggerPaused()) {
            const script::IScriptHost::DebugFrame& f = playHost->PausedFrame();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("⏸ 已暂停: %s 行 %d", f.script.c_str(), f.line);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("继续")) playHost->DebuggerResume(false);
            ImGui::SameLine();
            if (ImGui::Button("单步")) playHost->DebuggerResume(true);
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
            TextEditor::Breakpoints bpVis(ctx.scriptEditor->breakpoints[ctx.scriptEditor->path].begin(),
                                          ctx.scriptEditor->breakpoints[ctx.scriptEditor->path].end());
            ctx.scriptEditor->edit.SetBreakpoints(bpVis);
        }        const ImVec2 editSize = ImGui::GetContentRegionAvail();
        ctx.scriptEditor->edit.Render("##script_editor", editSize, true);
        if (ctx.scriptEditor->edit.IsTextChanged()) ctx.scriptEditor->dirty = true;
    }
    ImGui::End();
}

} // namespace neon::editor
