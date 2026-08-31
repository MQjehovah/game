void EditorApp::BuildUIEditorPanel() {
    if (!showUIEditor_) return;
    // Give the panel a usable size the first time it opens (docking/resize
    // afterwards persists); otherwise it can float tiny and hide the fields.
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("UI 编辑器", &showUIEditor_)) {
        // --- File bar -----------------------------------------------------
        if (ImGui::Button("新建")) {
            uiDoc_ = ui::UiDocument{};
            uiSelection_.clear();
            uiDoc_.root.name = "root";
            uiDoc_.root.rect = {0, 0, 1280, 720};
            ui::UiNode* menu = uiDoc_.root.AddChild(ui::UiNodeType::Panel, "Menu");
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
            uiDocPath_ = projectDir_ + "/assets/ui/untitled.ui.json";
            uiDocOpen_ = true;
            UISelectNode(&uiDoc_.root);
            uiDirty_ = true; // untitled: wait for the explicit 保存 button
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (uiDocOpen_ && !uiDocPath_.empty()) {
                MakeDirSingle(projectDir_ + "/assets/ui");
                if (uiDoc_.Save(uiDocPath_)) {
                    uiDirty_ = false;
                    NEON_LOG_INFO("UI: saved '%s'", uiDocPath_.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(uiDirty_ ? "有未保存修改" : "");

        // P5-editor UX: align tools + grid snap + batch copy/delete.
        ImGui::Separator();
        if (ImGui::Button("左对齐")) UIAlignSelected(0);
        ImGui::SameLine();
        if (ImGui::Button("水平居中")) UIAlignSelected(1);
        ImGui::SameLine();
        if (ImGui::Button("右对齐")) UIAlignSelected(2);
        ImGui::SameLine();
        if (ImGui::Button("顶对齐")) UIAlignSelected(3);
        ImGui::SameLine();
        if (ImGui::Button("垂直居中")) UIAlignSelected(4);
        ImGui::SameLine();
        if (ImGui::Button("底对齐")) UIAlignSelected(5);
        ImGui::SameLine();
        ImGui::Checkbox("网格吸附", &uiSnapToGrid_);
        ImGui::SameLine();
        if (ImGui::Button("复制选中")) UIDuplicateSelectedNodes();
        ImGui::SameLine();
        if (ImGui::Button("删除选中")) UIDeleteSelectedNodes();
        if (uiSelection_.size() > 1)
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "已选中 %zu 个节点 (Ctrl 加选, 方向键微调)",
                               uiSelection_.size());
        ImGui::Separator();

        // --- Document list ------------------------------------------------
        ImGui::Separator();
        ImGui::TextDisabled("项目 UI 文档 (assets/ui/*.ui.json)");
        uiFiles_.clear();
        std::vector<AssetEntry> entries;
        if (ListDirectory(projectDir_ + "/assets/ui", entries)) {
            for (const AssetEntry& f : entries) {
                if (f.isDir || f.name.size() < 9 ||
                    f.name.compare(f.name.size() - 8, 8, ".ui.json") != 0)
                    continue;
                uiFiles_.push_back(f.path);
            }
        }
        std::sort(uiFiles_.begin(), uiFiles_.end());
        if (uiFiles_.empty()) {
            ImGui::TextDisabled("(无文档 — 点“新建”创建)");
        }
        for (const std::string& path : uiFiles_) {
            const bool isOpen = path == uiDocPath_;
            if (ImGui::Selectable(UiFileBaseName(path).c_str(), isOpen)) {
                if (uiDoc_.Load(path)) {
                    uiDocPath_ = path;
                    uiDocOpen_ = true;
                    uiSelection_.clear();
                    UISelectNode(&uiDoc_.root);
                    uiDirty_ = false;
                    NEON_LOG_INFO("UI: opened '%s'", path.c_str());
                }
            }
        }
        // Auto-open the first document when the panel is enabled with nothing
        // loaded, so the viewport preview is immediately usable.
        if (!uiDocOpen_ && !uiFiles_.empty()) {
            if (uiDoc_.Load(uiFiles_[0])) {
                uiDocPath_ = uiFiles_[0];
                uiDocOpen_ = true;
                uiSelection_.clear();
                UISelectNode(uiDoc_.Find("Start") ? uiDoc_.Find("Start") : &uiDoc_.root);
                uiDirty_ = false;
            }
        }

        if (!uiDocOpen_) {
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
                if (uiSelection_.count(node)) flags |= ImGuiTreeNodeFlags_Selected;
                const bool open =
                    ImGui::TreeNodeEx(node->name.c_str(), flags, "%s##%p",
                                      node->name.c_str(), static_cast<void*>(node));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    if (ImGui::GetIO().KeyCtrl)
                        UIToggleSelectNode(node);
                    else
                        UISelectNode(node);
                }
                if (open) {
                    for (auto& c : node->children) drawTree(c.get());
                    ImGui::TreePop();
                }
            };
            drawTree(&uiDoc_.root);
            ImGui::EndChild();

            // Right: properties.
            ImGui::SameLine();
            ImGui::BeginChild("##ui_props", ImVec2(0, -30), true);
            if (!uiSelected_) {
                ImGui::TextDisabled("未选择节点 — 在视口或左侧树中点击选择");
            } else {
                char nameBuf[128];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", uiSelected_->name.c_str());
                if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
                    uiSelected_->name = nameBuf;
                    MarkUIDirty();
                }
                ImGui::TextDisabled("类型: %s", ui::UiNodeTypeName(uiSelected_->type));
                ImGui::Separator();
                bool rectChanged = false;
                rectChanged |= ImGui::DragFloat("X", &uiSelected_->rect.x, 1.0f);
                rectChanged |= ImGui::DragFloat("Y", &uiSelected_->rect.y, 1.0f);
                rectChanged |= ImGui::DragFloat("宽", &uiSelected_->rect.w, 1.0f, 1.0f, 4096.0f);
                rectChanged |= ImGui::DragFloat("高", &uiSelected_->rect.h, 1.0f, 1.0f, 4096.0f);
                if (rectChanged) MarkUIDirty();

                float color[4] = {uiSelected_->color.r, uiSelected_->color.g,
                                  uiSelected_->color.b, uiSelected_->color.a};
                if (ImGui::ColorEdit4("颜色", color)) {
                    uiSelected_->color = {color[0], color[1], color[2], color[3]};
                    MarkUIDirty();
                }
                if (uiSelected_->type == ui::UiNodeType::Label ||
                    uiSelected_->type == ui::UiNodeType::Button) {
                    char textBuf[256];
                    std::snprintf(textBuf, sizeof(textBuf), "%s", uiSelected_->text.c_str());
                    if (ImGui::InputText("文本", textBuf, sizeof(textBuf))) {
                        uiSelected_->text = textBuf;
                        MarkUIDirty();
                    }
                    if (ImGui::DragFloat("字号", &uiSelected_->fontSize, 1.0f, 6.0f, 96.0f))
                        MarkUIDirty();
                }
                if (uiSelected_->type == ui::UiNodeType::Bar) {
                    if (ImGui::SliderFloat("填充", &uiSelected_->fill, 0.0f, 1.0f))
                        MarkUIDirty();
                }
                bool visible = uiSelected_->visible;
                if (ImGui::Checkbox("可见", &visible)) {
                    uiSelected_->visible = visible;
                    MarkUIDirty();
                }
            }
            ImGui::EndChild();

            // Bottom: add/delete node.
            if (uiSelected_) {
                ImGui::Separator();
                ImGui::TextDisabled("添加子节点:");
                int typeCount[5] = {0, 0, 0, 0, 0};
                for (auto& c : uiSelected_->children)
                    typeCount[static_cast<int>(c->type)] += 1;
                auto addBtn = [&](const char* label, ui::UiNodeType t) {
                    if (!ImGui::Button(label)) return;
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s_%d",
                                  ui::UiNodeTypeName(t), typeCount[static_cast<int>(t)] + 1);
                    uiSelected_ = uiSelected_->AddChild(t, name);
                    MarkUIDirty();
                };
                addBtn("面板", ui::UiNodeType::Panel);
                ImGui::SameLine();
                addBtn("文本", ui::UiNodeType::Label);
                ImGui::SameLine();
                addBtn("按钮", ui::UiNodeType::Button);
                ImGui::SameLine();
                addBtn("进度条", ui::UiNodeType::Bar);
                if (uiSelected_ != &uiDoc_.root) {
                    ImGui::SameLine();
                    if (ImGui::Button("删除节点")) {
                        ui::UiNode* doomed = uiSelected_;
                        uiSelected_ = nullptr;
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
                        removeFrom(&uiDoc_.root);
                        MarkUIDirty();
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Localization editor (Godot-style): load <project>/assets/locales/*.json, pick the
// preview language, edit key/value pairs, save back to locales.json.
void EditorApp::BuildLocPanel() {
    if (!showLoc_) return;
    if (ImGui::Begin("本地化", &showLoc_)) {
        if (ImGui::Button("加载项目字符串表")) {
            locState_.edit = core::Localization();
            std::vector<AssetEntry> files;
            if (ListDirectory(projectDir_ + "/assets/locales", files)) {
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
                    if (!locState_.edit.LoadTable(text, &err)) {
                        NEON_LOG_WARN("Loc: '%s' failed to load: %s", f.path.c_str(),
                                      err.c_str());
                        continue;
                    }
                    locState_.path = f.path;
                }
            }
            locState_.edit.SetLanguage(locState_.language);
            NEON_LOG_INFO("Loc: loaded project strings (%zu keys)", locState_.edit.Keys().size());
        }
        ImGui::SameLine();
        if (ImGui::Button("保存 (locales.json)")) {
            const std::string dir = projectDir_ + "/assets/locales";
            MakeDirSingle(dir);
            const std::string path = dir + "/locales.json";
            std::ofstream out(path, std::ios::binary);
            if (out.is_open()) {
                out << core::JsonWriter::WritePretty(locState_.edit.ToJson());
                locState_.path = path;
                NEON_LOG_INFO("Loc: saved -> %s", path.c_str());
            } else {
                NEON_LOG_ERROR("Loc: cannot write '%s'", path.c_str());
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("路径: %s", locState_.path.c_str());
        ImGui::Separator();

        const std::vector<std::string> langs = locState_.edit.Languages();
        if (!langs.empty()) {
            std::vector<const char*> langLabels;
            for (const std::string& l : langs) langLabels.push_back(l.c_str());
            int sel = 0;
            for (size_t i = 0; i < langs.size(); ++i)
                if (langs[i] == locState_.language) sel = static_cast<int>(i);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("语言", &sel, langLabels.data(),
                             static_cast<int>(langLabels.size()))) {
                locState_.language = langs[static_cast<size_t>(sel)];
                locState_.edit.SetLanguage(locState_.language);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("默认: %s", locState_.edit.DefaultLanguage().c_str());
        } else {
            ImGui::TextDisabled("未加载字符串表");
        }

        static char newKey[128] = {};
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputText("新键", newKey, sizeof(newKey));
        ImGui::SameLine();
        if (ImGui::Button("添加键") && newKey[0] != '\0') {
            locState_.edit.Set(locState_.language, newKey, "");
            newKey[0] = '\0';
        }
        ImGui::Separator();

        ImGui::BeginChild("##loc_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const std::vector<std::string> keys = locState_.edit.Keys();
        for (const std::string& key : keys) {
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s",
                          locState_.edit.GetIn(locState_.language, key).c_str());
            const std::string label = key + "##" + locState_.language;
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                locState_.edit.Set(locState_.language, key, buf);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
