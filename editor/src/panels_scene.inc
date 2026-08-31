void EditorApp::BuildScenePanel() {
    if (!showHierarchy_) return;
    if (ImGui::Begin("场景", &showHierarchy_)) {
        // Post-process FX toggles (SSAO / volumetric / SSR). Applied to both
        // the editor viewport and the play runtime; intensity sliders expose the
        // per-effect strength.
        if (ImGui::CollapsingHeader("后处理效果##postfx", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("SSAO (环境光遮蔽)", &postSsao_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("强度##ssao", &postSsaoIntensity_, 0.1f, 3.0f, "%.2f");
            ImGui::Checkbox("体积雾光", &postVolumetric_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("强度##vol", &postVolumetricIntensity_, 0.1f, 3.0f, "%.2f");
            ImGui::Checkbox("屏幕空间反射", &postSsr_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("强度##ssr", &postSsrIntensity_, 0.1f, 2.0f, "%.2f");
            ImGui::Separator();
        }
        static int addType = 0;
        // Unity-style: only primitive geometry is created from the toolbar.
        // Helmet / tree / house etc. are resource objects and are dragged in
        // from the 资源 panel (or double-clicked there).
        const char* types[] = {"地形", "方块", "球体", "平面", "相机", "方向光", "点光源"};
        ImGui::SetNextItemWidth(86.0f);
        ImGui::Combo("##addtype", &addType, types, 7);
        ImGui::SameLine();
        if (ImGui::Button("添加")) {
            const char* keys[] = {"terrain", "cube", "sphere", "plane",
                                  "camera", "light:directional", "light:point"};
            AddEntity(keys[addType]);
        }
        ImGui::SameLine();
        ImGui::Button("?");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("模型资源(头盔/松树/房屋等)从 资源 面板双击导入或拖入");
        ImGui::SameLine();
        if (ImGui::Button("复制") && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                &entities_, SelectedIndices()));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("删除") && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ClampSelection();
        }
        ImGui::SameLine();
        // ↑/↓ move the selected entity within its OWN sibling group (the tree
        // groups children by parentId in global-array order, so a global ±1
        // move could silently reorder another parent's children instead).
        auto moveSibling = [this](int dir) {
            if (selection_.size() != 1 || selected_ < 0 ||
                selected_ >= static_cast<int>(entities_.size()))
                return;
            const size_t sel = static_cast<size_t>(selected_);
            const int parentId = entities_[sel].parentId;
            std::vector<size_t> sibs;
            for (size_t i = 0; i < entities_.size(); ++i)
                if (entities_[i].parentId == parentId) sibs.push_back(i);
            const auto it = std::find(sibs.begin(), sibs.end(), sel);
            if (it == sibs.end()) return;
            const size_t pos = static_cast<size_t>(it - sibs.begin());
            const size_t none = static_cast<size_t>(-1);
            const size_t to = dir < 0 ? (pos == 0 ? none : sibs[pos - 1])
                                      : (pos + 1 >= sibs.size() ? none : sibs[pos + 1]);
            if (to == none) return;
            history_.Push(std::make_unique<ReorderEntityCommand>(&entities_, sel, to));
            SetSelection(static_cast<int>(to));
        };
        ImGui::SameLine();
        if (ImGui::Button("↑")) moveSibling(-1);
        ImGui::SameLine();
        if (ImGui::Button("↓")) moveSibling(1);
        ImGui::SameLine();
        if (ImGui::Button("按名称排序")) SortSceneTreeByName();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("对场景树按名称排序（每个父级下递归、可撤销）");
        if (selection_.size() > 1)
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "已选中 %zu 个实体 (Ctrl 加选 / Shift 连选)",
                               selection_.size());
        ImGui::Separator();
        ImGui::BeginChild("##scene_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        // P2-editor UX: entity-name filter — flat filtered list replaces the
        // tree while typing (large scenes stay navigable).
        static char filterBuf[128] = {};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##scene_filter", "过滤实体名...", filterBuf,
                                 sizeof(filterBuf));
        ImGui::Separator();
        const std::string filter = ToLower(filterBuf);
        if (!filter.empty()) {
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& fe = entities_[i];
                if (ToLower(fe.name).find(filter) == std::string::npos) continue;
                char flabel[256];
                std::snprintf(flabel, sizeof(flabel), "%s##filter_%zu", fe.name.c_str(), i);
                if (ImGui::Selectable(flabel, IsSelected(static_cast<int>(i)))) {
                    if (ImGui::GetIO().KeyCtrl)
                        ToggleSelection(static_cast<int>(i));
                    else if (ImGui::GetIO().KeyShift)
                        SelectRangeTo(static_cast<int>(i));
                    else
                        SetSelection(static_cast<int>(i));
                }
            }
            ImGui::EndChild();
            // The asset drop targets below still apply to the filtered view.
        } else {
        // Godot-style scene tree: entities group under their parentId
        // (0 = root). Drag one row onto another to reparent; drag onto the
        // empty area to detach back to root. Cycle / self-parent are rejected.
            // Live ids must be unique and non-zero for the id-based tree and
            // drag guards: mid-session entities (duplicate command copies the
            // source id; asset drops start at 0) get normalized here, every
            // frame, before the tree is built.
            NormalizeEntityIds();
            std::map<int, std::vector<int>> childrenByParent;
            for (size_t i = 0; i < entities_.size(); ++i)
                childrenByParent[entities_[i].parentId].push_back(static_cast<int>(i));
            auto parentIdOf = [this](int id) {
                for (const SceneEntity& e : entities_)
                    if (e.id == id) return e.parentId;
                return 0;
            };
            auto reparent = [this, &parentIdOf](const std::vector<int>& from,
                                 int toParentId, int targetIdx = -1) {
                if (from.empty()) return;
                // Cycle guard: cannot parent an entity under itself or one of
                // its descendants (walk the target's ancestor chain by id).
                std::set<int> draggedIds;
                for (int i : from)
                    if (i >= 0 && i < static_cast<int>(entities_.size()))
                        draggedIds.insert(entities_[static_cast<size_t>(i)].id);
                if (draggedIds.count(toParentId) != 0) {
                    NEON_LOG_INFO("Scene: cannot parent an entity under itself");
                    return;
                }
                int cur = toParentId;
                int guard = 0;
                while (cur != 0 && guard++ <= static_cast<int>(entities_.size())) {
                    if (draggedIds.count(cur) != 0) {
                        NEON_LOG_INFO("Scene: cannot parent under its own descendant (cycle)");
                        return;
                    }
                    cur = parentIdOf(cur);
                }
                std::vector<int> valid;
                for (int i : from) {
                    if (i < 0 || i >= static_cast<int>(entities_.size())) continue;
                    if (entities_[static_cast<size_t>(i)].parentId == toParentId) continue;
                    valid.push_back(i);
                }
                if (valid.empty()) {
                    // 全部拖拽实体都已是 toParentId 的孩子 = 同父拖拽 = 兄弟重排序:
                    // 把拖拽实体移动到目标实体之前 (数组顺序, 一个撤销步骤)。
                    bool allSiblings = targetIdx >= 0 &&
                                       targetIdx < static_cast<int>(entities_.size());
                    for (int i : from)
                        if (i < 0 || i >= static_cast<int>(entities_.size()) ||
                            entities_[static_cast<size_t>(i)].parentId != toParentId)
                            allSiblings = false;
                    if (!allSiblings) {
                        NEON_LOG_INFO("Scene: nothing to reparent (already in place)");
                        return;
                    }
                    std::set<int> fromSet(from.begin(), from.end());
                    std::vector<size_t> newOrder;
                    const size_t targetPos = static_cast<size_t>(targetIdx);
                    bool inserted = false;
                    for (size_t i = 0; i < entities_.size(); ++i) {
                        if (fromSet.count(i) != 0) continue; // 拖拽实体在目标位统一插回
                        if (!inserted && i == targetPos) {
                            for (int fi : from) newOrder.push_back(static_cast<size_t>(fi));
                            inserted = true;
                        }
                        newOrder.push_back(i);
                    }
                    if (!inserted) {
                        for (int fi : from) newOrder.push_back(static_cast<size_t>(fi));
                    }
                    history_.Push(std::make_unique<SortSceneTreeCommand>(
                        &entities_, std::move(newOrder)));
                    return;
                }
                history_.Push(std::make_unique<MultiSetParentCommand>(
                    &entities_, valid, toParentId));
            };
            std::function<void(int)> drawNode = [&](int parentId) {
                const auto it = childrenByParent.find(parentId);
                if (it == childrenByParent.end()) return;
                for (int idx : it->second) {
                    const SceneEntity& e = entities_[static_cast<size_t>(idx)];
                    char label[256];
                    std::snprintf(label, sizeof(label), "%s%s##scene_%d", e.name.c_str(),
                                  e.prefab.empty() ? "" : " (预置体)", idx);
                    const bool hasChildren = childrenByParent.count(e.id) != 0;
                    const bool selected = IsSelected(idx);
                    const bool ctrl = ImGui::GetIO().KeyCtrl;
                    const bool shift = ImGui::GetIO().KeyShift;
                    // P2-editor UX: right-click context menu on any row.
                    auto contextMenu = [&]() {
                        if (ImGui::BeginPopupContextItem(("scene_ctx_" + std::to_string(idx)).c_str())) {
                            if (ImGui::MenuItem("复制")) {
                                std::vector<int> sel = SelectedIndices();
                                if (sel.empty()) sel.push_back(idx);
                                history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                                    &entities_, sel));
                                SetSelection(static_cast<int>(entities_.size()) - 1);
                            }
                            if (ImGui::MenuItem("删除")) {
                                std::vector<int> sel = SelectedIndices();
                                if (sel.empty()) sel.push_back(idx);
                                history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                                    &entities_, sel));
                                ClampSelection();
                            }
                            if (ImGui::MenuItem("保存为预置体")) {
                                const SceneEntity& t = entities_[static_cast<size_t>(idx)];
                                std::snprintf(prefabSaveBuf_, sizeof(prefabSaveBuf_), "%s",
                                              t.name.c_str());
                                prefabSaveTarget_ = idx;
                                prefabSavePrompt_ = true;
                            }
                            ImGui::EndPopup();
                        }
                    };
                    if (hasChildren) {
                        const bool open = ImGui::TreeNodeEx(
                            label, ImGuiTreeNodeFlags_OpenOnArrow |
                                       (selected ? ImGuiTreeNodeFlags_Selected : 0));
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                            if (ctrl)
                                ToggleSelection(idx);
                            else if (shift)
                                SelectRangeTo(idx);
                            else
                                SetSelection(idx);
                        }
                        contextMenu();
                        // P2-editor UX: drag the whole selection to reparent.
                        // The source + target attach to THIS row BEFORE the
                        // children recurse: ImGui binds drag-drop to the LAST
                        // item, and after TreePop that is the deepest child
                        // row, so an expanded parent's own row had no source/
                        // target (multi-level reparent silently failed).
                        if (ImGui::BeginDragDropSource()) {
                            std::vector<int> drag = SelectedIndices();
                            if (drag.empty()) drag.push_back(idx);
                            dragPayload_ = drag;
                            ImGui::SetDragDropPayload("SCENE_ENTITIES",
                                                      dragPayload_.data(),
                                                      static_cast<size_t>(
                                                          dragPayload_.size()) *
                                                          sizeof(int));
                            ImGui::Text("移动 %zu 个实体", dragPayload_.size());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
            {
                ImDrawList* dropDl = ImGui::GetWindowDrawList();
                dropDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                IM_COL32(90, 190, 255, 150), 4.0f, 0, 2.0f);
            }
                            if (const ImGuiPayload* p =
                                    ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                                const int* data = static_cast<const int*>(p->Data);
                                const size_t n = p->DataSize / sizeof(int);
                                reparent(std::vector<int>(data, data + n), e.id, idx);
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (open) {
                            drawNode(e.id);
                            ImGui::TreePop();
                        }
                    } else {
                        if (ImGui::Selectable(label, selected)) {
                            if (ctrl)
                                ToggleSelection(idx);
                            else if (shift)
                                SelectRangeTo(idx);
                            else
                                SetSelection(idx);
                        }
                        contextMenu();
                        if (ImGui::BeginDragDropSource()) {
                            std::vector<int> drag = SelectedIndices();
                            if (drag.empty()) drag.push_back(idx);
                            dragPayload_ = drag;
                            ImGui::SetDragDropPayload("SCENE_ENTITIES",
                                                      dragPayload_.data(),
                                                      static_cast<size_t>(
                                                          dragPayload_.size()) *
                                                          sizeof(int));
                            ImGui::Text("移动 %zu 个实体", dragPayload_.size());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
            {
                ImDrawList* dropDl = ImGui::GetWindowDrawList();
                dropDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                IM_COL32(90, 190, 255, 150), 4.0f, 0, 2.0f);
            }
                            if (const ImGuiPayload* p =
                                    ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                                const int* data = static_cast<const int*>(p->Data);
                                const size_t n = p->DataSize / sizeof(int);
                                reparent(std::vector<int>(data, data + n), e.id, idx);
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                }
            };
            drawNode(0);
            // Detach target: drag an entity here to clear its parent.
            ImGui::TextDisabled("(拖到此处取消父子关系)");
            if (ImGui::BeginDragDropTarget()) {
            {
                ImDrawList* dropDl = ImGui::GetWindowDrawList();
                dropDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                IM_COL32(90, 190, 255, 150), 4.0f, 0, 2.0f);
            }
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                    const int* data = static_cast<const int*>(p->Data);
                    const size_t n = p->DataSize / sizeof(int);
                    reparent(std::vector<int>(data, data + n), 0, -1);
                }
                ImGui::EndDragDropTarget();
        }
        }
        ImGui::EndChild();

        // Drop targets: a model asset adds an entity, a script asset attaches to
        // the selected entity.
        if (ImGui::BeginDragDropTarget()) {
            {
                ImDrawList* dropDl = ImGui::GetWindowDrawList();
                dropDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                IM_COL32(90, 190, 255, 150), 4.0f, 0, 2.0f);
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path) {
                    const std::string prefix =
                        assets::MeshFormatRegistry::Instance().FormatFromExt(path);
                    if (!prefix.empty()) AddEntity(prefix + ":" + std::string(path));
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && *path) AddSpriteEntity(path);
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && *path) {
                    // payload is the prefab file path; extract the template name
                    // (assets/prefabs/<name>.json) and spawn an instance.
                    std::string nm = FileName(path);
                    const size_t dot = nm.find_last_of('.');
                    if (dot != std::string::npos) nm = nm.substr(0, dot);
                    if (!nm.empty()) AddEntity("prefab:" + nm);
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && selected_ >= 0 &&
                    selected_ < static_cast<int>(entities_.size())) {
                    std::vector<SceneScriptFields> newList =
                        entities_[static_cast<size_t>(selected_)].scripts;
                    newList.push_back({"lua", ToProjectRelPath(path, projectDir_), {}});
                    history_.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities_, selected_, ApplyScriptList,
                        entities_[static_cast<size_t>(selected_)].scripts, newList,
                        /*mergeable=*/false));
                }
            }
            ImGui::EndDragDropTarget();
        }
        // "保存为预置体" name prompt (scene right-click): confirm the template
        // name, then save the target entity as assets/prefabs/<name>.json.
        if (prefabSavePrompt_) {
            if (ImGui::Begin("保存为预置体", &prefabSavePrompt_,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
                ImGui::TextDisabled("把选中实体存为可复用的预置体模板");
                ImGui::InputText("名称", prefabSaveBuf_, sizeof(prefabSaveBuf_));
                ImGui::SameLine();
                ImGui::TextDisabled(".json");
                ImGui::Spacing();
                bool ok = false;
                if (ImGui::Button("保存")) ok = true;
                ImGui::SameLine();
                if (ImGui::Button("取消")) { prefabSavePrompt_ = false; prefabSaveTarget_ = -1; }
                if (ok) {
                    SetSelection(prefabSaveTarget_);
                    const std::string nm(prefabSaveBuf_);
                    if (!nm.empty()) {
                        std::string t = nm;
                        const size_t dot = t.find_last_of('.');
                        if (dot != std::string::npos) t = t.substr(0, dot);
                        SavePrefab(t);
                    }
                    prefabSavePrompt_ = false;
                    prefabSaveTarget_ = -1;
                }
            }
            ImGui::End();
        }
    }
    ImGui::End();
}

