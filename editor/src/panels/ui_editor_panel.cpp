#include "panels/ui_editor_panel.hpp"

// UI 编辑器实现 = 原 EditorApp::BuildUIEditorPanel（panels_ui.inc:1-242）方法体
// 逐行迁移：EditorApp 成员（showUIEditor_/uiDoc_/uiSelection_/uiSelected_/...
// + UISelectNode/UIAlignSelected/MarkUIDirty 等）改本类 visible_ / ctx.uiEditor
// / ctx 回调。行为零变化。MakeDirSingle/UiFileBaseName/ListDirectory 为共享工具
//（panels.cpp / editor.hpp）。

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "editor.hpp"
#include "imgui.h"
#include "neon/ui/document.hpp"

namespace neon::editor {

namespace {
// 原 panels.cpp 匿名命名空间的 MakeDirSingle（同其它面板的本地副本模式）。
bool MakeDirSingle(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}
std::string UiFileBaseName(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}
} // namespace

void UIEditorPanel::Draw(EditorContext& ctx) {
    UiEditorState& ui = *ctx.uiEditor;
    if (!visible_ || !*visible_) return;
    // Give the panel a usable size the first time it opens (docking/resize
    // afterwards persists); otherwise it can float tiny and hide the fields.
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("UI 编辑器", visible_)) {
        // --- File bar -----------------------------------------------------
        if (ImGui::Button("新建")) {
            ui.uiDoc = ui::UiDocument{};
            ui.uiSelection.clear();
            ui.uiDoc.root.name = "root";
            ui.uiDoc.root.rect = {0, 0, 1280, 720};
            ui::UiNode* menu = ui.uiDoc.root.AddChild(ui::UiNodeType::Panel, "Menu");
            menu->rect = {340, 180, 600, 360};
            menu->color = {0.08f, 0.12f, 0.20f, 0.92f};
            ui::UiNode* title = menu->AddChild(ui::UiNodeType::Label, "Title");
            title->rect = {0, 20, 600, 60};
            title->text = "新界面";
            title->fontSize = 40.0f;
            ui::UiNode* startBtn = menu->AddChild(ui::UiNodeType::Button, "Start");
            startBtn->rect = {180, 200, 240, 56};
            startBtn->text = "开始";
            startBtn->color = {0.15f, 0.45f, 0.28f, 1.0f};
            ui::UiNode* bar = menu->AddChild(ui::UiNodeType::Bar, "Hp");
            bar->rect = {140, 300, 320, 20};
            bar->fill = 0.7f;
            bar->color = {0.85f, 0.25f, 0.25f, 1.0f};
            ui.uiDocPath = *ctx.projectDir + "/assets/ui/untitled.ui.json";
            ui.uiDocOpen = true;
            ctx.uiSelectNode(&ui.uiDoc.root);
            ui.uiDirty = true; // untitled: wait for the explicit 保存 button
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (ui.uiDocOpen && !ui.uiDocPath.empty()) {
                MakeDirSingle(*ctx.projectDir + "/assets/ui");
                if (ui.uiDoc.Save(ui.uiDocPath)) {
                    ui.uiDirty = false;
                    NEON_LOG_INFO("UI: saved '%s'", ui.uiDocPath.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(ui.uiDirty ? "有未保存修改" : "");

        // P5-editor UX: align tools + grid snap + batch copy/delete.
        ImGui::Separator();
        if (ImGui::Button("左对齐")) ctx.uiAlignSelected(0);
        ImGui::SameLine();
        if (ImGui::Button("水平居中")) ctx.uiAlignSelected(1);
        ImGui::SameLine();
        if (ImGui::Button("右对齐")) ctx.uiAlignSelected(2);
        ImGui::SameLine();
        if (ImGui::Button("顶对齐")) ctx.uiAlignSelected(3);
        ImGui::SameLine();
        if (ImGui::Button("垂直居中")) ctx.uiAlignSelected(4);
        ImGui::SameLine();
        if (ImGui::Button("底对齐")) ctx.uiAlignSelected(5);
        ImGui::SameLine();
        ImGui::Checkbox("网格吸附", &ui.uiSnapToGrid);
        ImGui::SameLine();
        if (ImGui::Button("复制选中")) ctx.uiDuplicateSelectedNodes();
        ImGui::SameLine();
        if (ImGui::Button("删除选中")) ctx.uiDeleteSelectedNodes();
        if (ui.uiSelection.size() > 1)
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "已选中 %zu 个节点 (Ctrl 加选, 方向键微调)",
                               ui.uiSelection.size());
        ImGui::Separator();

        // --- Document list ------------------------------------------------
        ImGui::Separator();
        ImGui::TextDisabled("项目 UI 文档 (assets/ui/*.ui.json)");
        ui.uiFiles.clear();
        std::vector<AssetEntry> entries;
        if (ListDirectory(*ctx.projectDir + "/assets/ui", entries)) {
            for (const AssetEntry& f : entries) {
                if (f.isDir || f.name.size() < 9 ||
                    f.name.compare(f.name.size() - 8, 8, ".ui.json") != 0)
                    continue;
                ui.uiFiles.push_back(f.path);
            }
        }
        std::sort(ui.uiFiles.begin(), ui.uiFiles.end());
        if (ui.uiFiles.empty()) {
            ImGui::TextDisabled("(无文档 — 点“新建”创建)");
        }
        for (const std::string& path : ui.uiFiles) {
            const bool isOpen = path == ui.uiDocPath;
            if (ImGui::Selectable(UiFileBaseName(path).c_str(), isOpen)) {
                if (ui.uiDoc.Load(path)) {
                    ui.uiDocPath = path;
                    ui.uiDocOpen = true;
                    ui.uiSelection.clear();
                    ctx.uiSelectNode(&ui.uiDoc.root);
                    ui.uiDirty = false;
                    NEON_LOG_INFO("UI: opened '%s'", path.c_str());
                }
            }
        }
        // Auto-open the first document when the panel is enabled with nothing
        // loaded, so the viewport preview is immediately usable.
        if (!ui.uiDocOpen && !ui.uiFiles.empty()) {
            if (ui.uiDoc.Load(ui.uiFiles[0])) {
                ui.uiDocPath = ui.uiFiles[0];
                ui.uiDocOpen = true;
                ui.uiSelection.clear();
                ctx.uiSelectNode(ui.uiDoc.Find("Start") ? ui.uiDoc.Find("Start") : &ui.uiDoc.root);
                ui.uiDirty = false;
            }
        }

        if (!ui.uiDocOpen) {
            ImGui::End();
            return;
        }

        ImGui::Separator();
        ImGui::BeginChild("##ui_editor_body", ImVec2(0, 0), true);
        {
            // Left: node tree.
            ImGui::BeginChild("##ui_tree", ImVec2(230, -30), true);
            std::function<void(ui::UiNode*)> drawTree = [&](ui::UiNode* node) {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_OpenOnDoubleClick |
                ImGuiTreeNodeFlags_SpanAvailWidth;
                if (ui.uiSelection.count(node)) flags |= ImGuiTreeNodeFlags_Selected;
                const bool open =
                    ImGui::TreeNodeEx(node->name.c_str(), flags, "%s##%p",
                                      node->name.c_str(), static_cast<void*>(node));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    if (ImGui::GetIO().KeyCtrl)
                        ctx.uiToggleSelectNode(node);
                    else
                        ctx.uiSelectNode(node);
                }
                if (open) {
                    for (auto& c : node->children) drawTree(c.get());
                    ImGui::TreePop();
                }
            };
            drawTree(&ui.uiDoc.root);
            ImGui::EndChild();

            // Right: properties.
            ImGui::SameLine();
            ImGui::BeginChild("##ui_props", ImVec2(0, -30), true);
            if (!ui.uiSelected) {
                ImGui::TextDisabled("未选择节点 — 在视口或左侧树中点击选择");
            } else {
                char nameBuf[128];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", ui.uiSelected->name.c_str());
                if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
                    ui.uiSelected->name = nameBuf;
                    ctx.markUiDirty();
                }
                ImGui::TextDisabled("类型: %s", ui::UiNodeTypeName(ui.uiSelected->type));
                ImGui::Separator();
                bool rectChanged = false;
                rectChanged |= ImGui::DragFloat("X", &ui.uiSelected->rect.x, 1.0f);
                rectChanged |= ImGui::DragFloat("Y", &ui.uiSelected->rect.y, 1.0f);
                rectChanged |= ImGui::DragFloat("宽", &ui.uiSelected->rect.w, 1.0f, 1.0f, 4096.0f);
                rectChanged |= ImGui::DragFloat("高", &ui.uiSelected->rect.h, 1.0f, 1.0f, 4096.0f);
                if (rectChanged) ctx.markUiDirty();

                float color[4] = {ui.uiSelected->color.r, ui.uiSelected->color.g,
                                  ui.uiSelected->color.b, ui.uiSelected->color.a};
                if (ImGui::ColorEdit4("颜色", color)) {
                    ui.uiSelected->color = {color[0], color[1], color[2], color[3]};
                    ctx.markUiDirty();
                }
                if (ui.uiSelected->type == ui::UiNodeType::Label ||
                    ui.uiSelected->type == ui::UiNodeType::Button) {
                    char textBuf[256];
                    std::snprintf(textBuf, sizeof(textBuf), "%s", ui.uiSelected->text.c_str());
                    if (ImGui::InputText("文本", textBuf, sizeof(textBuf))) {
                        ui.uiSelected->text = textBuf;
                        ctx.markUiDirty();
                    }
                    if (ImGui::DragFloat("字号", &ui.uiSelected->fontSize, 1.0f, 6.0f, 96.0f))
                        ctx.markUiDirty();
                }
                if (ui.uiSelected->type == ui::UiNodeType::Bar) {
                    if (ImGui::SliderFloat("填充", &ui.uiSelected->fill, 0.0f, 1.0f))
                        ctx.markUiDirty();
                }
                bool visible = ui.uiSelected->visible;
                if (ImGui::Checkbox("可见", &visible)) {
                    ui.uiSelected->visible = visible;
                    ctx.markUiDirty();
                }
            }
            ImGui::EndChild();

            // Bottom: add/delete node.
            if (ui.uiSelected) {
                ImGui::Separator();
                ImGui::TextDisabled("添加子节点:");
                int typeCount[5] = {0, 0, 0, 0, 0};
                for (auto& c : ui.uiSelected->children)
                    typeCount[static_cast<int>(c->type)] += 1;
                auto addBtn = [&](const char* label, ui::UiNodeType t) {
                    if (!ImGui::Button(label)) return;
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s_%d",
                                  ui::UiNodeTypeName(t), typeCount[static_cast<int>(t)] + 1);
                    ui.uiSelected = ui.uiSelected->AddChild(t, name);
                    ctx.markUiDirty();
                };
                addBtn("面板", ui::UiNodeType::Panel);
                ImGui::SameLine();
                addBtn("文本", ui::UiNodeType::Label);
                ImGui::SameLine();
                addBtn("按钮", ui::UiNodeType::Button);
                ImGui::SameLine();
                addBtn("进度条", ui::UiNodeType::Bar);
                if (ui.uiSelected != &ui.uiDoc.root) {
                    ImGui::SameLine();
                    if (ImGui::Button("删除节点")) {
                        ui::UiNode* doomed = ui.uiSelected;
                        ui.uiSelected = nullptr;
                        std::function<bool(ui::UiNode*)> removeFrom =
                            [&](ui::UiNode* n) -> bool {
                            for (auto it = n->children.begin(); it != n->children.end(); ++it) {
                                if (it->get() == doomed) {
                                    n->children.erase(it);
                                    return true;
                                }
                                if (removeFrom(it->get())) return true;
                            }
                            return false;
                        };
                        removeFrom(&ui.uiDoc.root);
                        ctx.markUiDirty();
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace neon::editor
