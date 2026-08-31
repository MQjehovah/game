void EditorApp::BuildAssetPanel() {
    if (!showAssets_) return;
    if (deleteAssetRequested_) {
        deleteAssetRequested_ = false;
        DeleteSelectedAsset();
    }
    if (ImGui::Begin("资产", &showAssets_)) {
        if (ImGui::SmallButton("刷新")) RefreshAssetDir();
        ImGui::SameLine();
        // 一个入口同时支持文件与目录: 二级菜单选择后, 下一帧开原生对话框
        // (不能在 popup 模态内直接开)。
        static bool pendingFile = false;
        static bool pendingDir = false;
        if (ImGui::SmallButton("浏览导入")) ImGui::OpenPopup("##import_pick");
        if (ImGui::BeginPopup("##import_pick")) {
            if (ImGui::MenuItem("导入文件...")) pendingFile = true;
            if (ImGui::MenuItem("导入目录...")) pendingDir = true;
            ImGui::EndPopup();
        }
        if (pendingFile) {
            pendingFile = false;
            const std::string picked =
                PickImportFile(Window() ? Window()->NativeHandle() : nullptr);
            if (!picked.empty()) ImportAssetFile(picked);
        }
        if (pendingDir) {
            pendingDir = false;
            const std::string picked =
                PickImportDir(Window() ? Window()->NativeHandle() : nullptr);
            if (!picked.empty()) {
                // 整目录递归拷入当前浏览目录, 再刷新列表。
                const std::string dst = assetDir_ + "/" + FileName(picked);
                if (CopyDirRecursive(picked, dst)) RefreshAssetDir();
            }
        }
        ImGui::SameLine();
        static int newKind = -1;
        if (ImGui::SmallButton(newKind >= 0 ? "取消新建" : "新建"))
            newKind = (newKind >= 0) ? -1 : 0;
        ImGui::SameLine();
        if (ImGui::SmallButton(assetGridView_ ? "网格视图" : "列表视图"))
            assetGridView_ = !assetGridView_;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // Unity-style Project filter tabs: 全部 / 模型 / 贴图 / 脚本.
        const char* filters[] = {"全部", "模型", "贴图", "脚本", "材质"};
        for (int f = 0; f < 5; ++f) {
            if (f) ImGui::SameLine();
            if (ImGui::SmallButton(filters[f])) assetFilter_ = f;
            if (assetFilter_ == f) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "✓");
            }
        }
        // New-asset row: type combo + name + create.
        if (newKind >= 0) {
            static const char* kinds[] = {"目录", "Lua 脚本", "JSON 文件", "文本文件",
                                          "材质球", "JS 脚本"};
            static char newName[128] = {};
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##new_kind", &newKind, kinds, 6)) {
                // hint defaults per kind
                if (newKind == 1) std::strncpy(newName, "new_script.lua", sizeof(newName) - 1);
                else if (newKind == 5) std::strncpy(newName, "new_script.js", sizeof(newName) - 1);
                else if (newKind == 2) std::strncpy(newName, "new_data.json", sizeof(newName) - 1);
                else if (newKind == 4) std::strncpy(newName, "new_material.mat.json", sizeof(newName) - 1);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputText("##new_name", newName, sizeof(newName));
            ImGui::SameLine();
            if (ImGui::SmallButton("创建")) {
                if (newName[0] != '\0') {
                    CreateAssetFile(newName, newKind);
                    newName[0] = '\0';
                    newKind = -1;
                }
            }
        }
        ImGui::Separator();

        // Plugin asset sources (素材市场): plugins contribute read-only
        // catalogs; 导入 copies an entry into the current project dir.
        if (pluginMgr_ && !pluginMgr_->AssetSources().empty()) {
            for (editor::PluginAssetSource& s : pluginMgr_->AssetSources()) {
                const std::string header = s.name + " (插件)";
                if (!ImGui::CollapsingHeader(header.c_str())) continue;
                if (!s.host || s.listFn == 0) {
                    ImGui::TextDisabled("源不可用");
                    continue;
                }
                const auto res = s.host->CallCaptured(s.listFn, {});
                if (!res.Ok() || res.Value().type != script::Value::Type::Table) {
                    ImGui::TextDisabled("列表加载失败");
                    continue;
                }
                const script::Value& entries = res.Value();
                for (size_t i = 0; i < entries.table->array.size(); ++i) {
                    const script::Value& it = entries.table->array[i];
                    if (it.type != script::Value::Type::Table) continue;
                    std::string name, type, path;
                    for (const auto& kv : it.table->fields) {
                        if (kv.second.type != script::Value::Type::String) continue;
                        if (kv.first == "name") name = kv.second.str;
                        if (kv.first == "type") type = kv.second.str;
                        if (kv.first == "path") path = kv.second.str;
                    }
                    if (name.empty() || path.empty()) continue;
                    ImGui::BulletText("%s  (%s)", name.c_str(), type.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("导入##" + s.id + std::to_string(i)).c_str())) {
                        if (s.importFn != 0) {
                            const auto imp =
                                s.host->CallCaptured(s.importFn, {script::Value::Str(path)});
                            if (!imp.Ok()) {
                                NEON_LOG_ERROR("插件资产源 '%s' 导入失败: %s", s.id.c_str(),
                                               s.host->LastError().message.c_str());
                            } else if (imp.Value().type == script::Value::Type::String &&
                                       !imp.Value().str.empty()) {
                                NEON_LOG_INFO("插件资产源 '%s' 已导入: %s", s.id.c_str(),
                                              imp.Value().str.c_str());
                            }
                        }
                    }
                }
            }
            ImGui::Separator();
        }

        // 详情面板仅在选中资产后显示 (默认整宽列表).
        const bool showDetail =
            selectedAsset_ >= 0 && selectedAsset_ < static_cast<int>(assetEntries_.size());
        const float detailW = showDetail ? 250.0f : 0.0f;
        const ImVec2 bodyAvail = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##asset_list",
                          ImVec2(bodyAvail.x - detailW - (showDetail ? 8.0f : 0.0f), 0),
                          ImGuiChildFlags_Borders);
        if (assetEntries_.empty()) {
            ImGui::TextWrapped("此目录为空。使用上方 浏览导入 / 浏览目录 添加外部资源，"
                               "或 新建 创建 Lua 脚本 / JSON / 材质球 / 目录。");
        }
        if (assetGridView_) {
            // Thumbnail grid (Unity Project icon mode): a fixed cell per
            // asset with a real preview for textures/models and a colored
            // type tile for directories/scripts/materials/JSON.
            const float cellW = 84.0f;
            const float cellH = 94.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const int cols = std::max(1, static_cast<int>(avail.x / cellW));
            // Parent-directory cell: first slot goes up a level.
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            if (ImGui::Button("⬆ 上级", ImVec2(cellW - 6.0f, cellH - 8.0f))) {
                const std::string parent = ParentPath(assetDir_);
                if (parent != assetDir_) {
                    assetDir_ = parent;
                    RefreshAssetDir();
                }
            }
            int visible = 0;
            for (size_t i = 0; i < assetEntries_.size(); ++i) {
                const AssetEntry& e = assetEntries_[i];
                if (!AssetMatchesFilter(e, assetFilter_)) continue;
                const int col = (visible + 1) % cols;
                const int row = (visible + 1) / cols;
                ++visible;
                ImGui::SetCursorPos(ImVec2(col * cellW, row * cellH));
                const std::string id = "##acell_" + std::to_string(i);
                if (ImGui::InvisibleButton(id.c_str(), ImVec2(cellW - 6.0f, cellH - 8.0f))) {
                    selectedAsset_ = static_cast<int>(i);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("删除")) {
                        selectedAsset_ = static_cast<int>(i);
                        DeleteSelectedAsset();
                    }
                    ImGui::EndPopup();
                }
                // 网格视图拖拽源: 与列表视图一致 (模型→场景, 贴图→材质槽, 脚本→实体)。
                if (!e.isDir) {
                    const char* kind = nullptr;
                    if (IsModelExt(e.name)) kind = "ASSET_MODEL";
                    else if (IsImageExt(e.name)) kind = "ASSET_TEXTURE";
                    else if (IsScriptExt(e.name)) kind = "ASSET_SCRIPT";
                    else if (IsMaterialExt(e.name)) kind = "ASSET_MATERIAL";
                    if (kind && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(kind, e.path.c_str(), e.path.size() + 1);
                        ImGui::Text("%s", e.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
                const bool hovered = ImGui::IsItemHovered();
                const bool dbl = hovered && ImGui::IsMouseDoubleClicked(0);
                if (dbl) {
                    if (e.isDir) {
                        assetDir_ = e.path;
                        RefreshAssetDir();
                    } else {
                        ImportAssetPath(e.path);
                    }
                }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 tl = ImGui::GetItemRectMin();
                const float tw = ImGui::GetItemRectSize().x;
                const ImVec2 thumbTl = {tl.x + 4.0f, tl.y + 2.0f};
                const ImVec2 thumbBr = {tl.x + tw - 4.0f, tl.y + 60.0f};
                const ImVec2 thumbSize = {thumbBr.x - thumbTl.x, thumbBr.y - thumbTl.y};
                // Selection highlight behind the tile.
                if (selectedAsset_ == static_cast<int>(i)) {
                    dl->AddRectFilled(tl, {tl.x + tw, tl.y + cellH - 8.0f},
                                      IM_COL32(60, 100, 160, 70));
                }
                ImTextureID tid = ImTextureID_Invalid;
                bool flipV = false;
                ImU32 tileCol = IM_COL32(70, 70, 80, 255);
                if (e.isDir) {
                    tileCol = IM_COL32(180, 140, 40, 255);
                } else if (IsImageExt(e.name)) {
                    gfx::Texture tex = assetMgr_.LoadTexture(e.path);
                    if (tex.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(tex.Handle());
                } else if (IsModelExt(e.name)) {
                    RequestMeshThumbnail(e.path);
                    auto thumb = meshThumbs_.find(e.path);
                    if (thumb != meshThumbs_.end()) {
                        tid = thumb->second.texId;
                        flipV = true; // FBO color textures are bottom-up
                    }
                    tileCol = IM_COL32(90, 130, 200, 255);
                } else if (IsMaterialExt(e.name)) {
                    RequestMaterialThumbnail(e.path);
                    auto thumb = materialThumbs_.find(e.path);
                    if (thumb != materialThumbs_.end()) {
                        tid = thumb->second.texId;
                        flipV = true;
                    }
                    tileCol = IM_COL32(150, 90, 190, 255);
                } else if (IsScriptExt(e.name)) {
                    tileCol = IM_COL32(80, 160, 80, 255);
                }
                if (tid != ImTextureID_Invalid) {
                    // Square display area: the thumbnail texture is square, so
                    // stretching it into the wider cell makes spheres look
                    // elliptical. Center a ts x ts square inside the cell.
                    const float ts = std::min(thumbBr.x - thumbTl.x,
                                              thumbBr.y - thumbTl.y);
                    const ImVec2 imgTl = {thumbTl.x + (thumbBr.x - thumbTl.x - ts) * 0.5f,
                                          thumbTl.y + (thumbBr.y - thumbTl.y - ts) * 0.5f};
                    const ImVec2 imgBr = {imgTl.x + ts, imgTl.y + ts};
                    dl->AddImage(tid, imgTl, imgBr, ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(1.0f, flipV ? 0.0f : 1.0f));
                    dl->AddRect(imgTl, imgBr, IM_COL32(30, 30, 35, 255));
                } else {
                    dl->AddRectFilled(thumbTl, thumbBr, tileCol);
                    dl->AddRect(thumbTl, thumbBr, IM_COL32(30, 30, 35, 255));
                    const char* tag = e.isDir ? "DIR"
                                      : IsMaterialExt(e.name) ? "MAT"
                                      : IsScriptExt(e.name) ? "LUA"
                                      : IsImageExt(e.name) ? "IMG"
                                      : IsModelExt(e.name) ? "MDL"
                                                           : "FILE";
                    dl->AddText(ImVec2(thumbTl.x + 4.0f, thumbTl.y + thumbSize.y * 0.5f - 8.0f),
                                IM_COL32(255, 255, 255, 220), tag);
                }
                // Name below the thumbnail (truncated to one line).
                const ImVec2 namePos = {tl.x + 3.0f, thumbBr.y + 2.0f};
                dl->PushClipRect(tl, {tl.x + tw, tl.y + cellH - 6.0f}, true);
                dl->AddText(namePos, IM_COL32(220, 225, 235, 255), e.name.c_str());
                dl->PopClipRect();
                // Drag sources work in grid mode too.
                if (!e.isDir && ImGui::BeginDragDropSource()) {
                    const char* kind = IsImageExt(e.name)   ? "ASSET_TEXTURE"
                                       : IsModelExt(e.name) ? "ASSET_MODEL"
                                       : IsScriptExt(e.name) ? "ASSET_SCRIPT"
                                       : IsMaterialExt(e.name) ? "ASSET_MATERIAL"
                                       : (InPrefabsDir() && ToLower(ExtLower(e.name)) == ".json")
                                             ? "ASSET_PREFAB"
                                             : nullptr;
                    if (kind) {
                        ImGui::SetDragDropPayload(kind, e.path.c_str(), e.path.size() + 1);
                        ImGui::Text("%s", e.name.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
            }
            ImGui::Dummy(ImVec2(1.0f, (visible / cols + 1) * cellH + 20.0f));
        } else {
            if (ImGui::Selectable("⬆ 上级目录##up")) {
                const std::string parent = ParentPath(assetDir_);
                if (parent != assetDir_) {
                    assetDir_ = parent;
                    RefreshAssetDir();
                }
            }
            ImGui::Separator();
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            const AssetEntry& e = assetEntries_[i];
            if (!AssetMatchesFilter(e, assetFilter_)) continue;
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##asset_%zu",
                          e.isDir ? "[D] " : "    ", e.name.c_str(), i);
            if (ImGui::Selectable(label, selectedAsset_ == static_cast<int>(i))) {
                selectedAsset_ = static_cast<int>(i);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("删除")) {
                    selectedAsset_ = static_cast<int>(i);
                    DeleteSelectedAsset();
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (e.isDir) {
                    assetDir_ = e.path;
                    RefreshAssetDir();
                } else {
                    ImportAssetPath(e.path);
                }
            }
            if (e.isDir) continue;
            // Drag sources: textures onto material slots, models onto the scene
            // (hierarchy), scripts onto a selected entity.
            if (IsImageExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_TEXTURE", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsModelExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_MODEL", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsScriptExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_SCRIPT", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsMaterialExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_MATERIAL", e.path.c_str(),
                                          e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (InPrefabsDir() && ToLower(ExtLower(e.name)) == ".json" &&
                       ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_PREFAB", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (showDetail) {
        ImGui::BeginChild("##asset_detail", ImVec2(detailW, 0), ImGuiChildFlags_Borders);
        if (selectedAsset_ >= 0 &&
            selectedAsset_ < static_cast<int>(assetEntries_.size())) {
            const AssetEntry& e = assetEntries_[static_cast<size_t>(selectedAsset_)];
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("%s", e.isDir ? "目录" : e.path.c_str());
            if (!e.isDir) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.1f KB", static_cast<double>(e.size) / 1024.0);
                ImGui::Separator();
                ImTextureID tid = ImTextureID_Invalid;
                bool flipV = false;
                if (IsModelExt(e.name)) {
                    if (ImGui::Button("导入到场景")) ImportAssetPath(e.path);
                    // Mesh thumbnail (T4.8): rendered into a small offscreen
                    // target by the frame's OnRender, cached by path+mtime.
                    RequestMeshThumbnail(e.path);
                    auto thumb = meshThumbs_.find(e.path);
                    if (thumb != meshThumbs_.end() &&
                        thumb->second.texId != ImTextureID_Invalid) {
                        tid = thumb->second.texId;
                        flipV = true; // FBO color textures are bottom-up
                    }
                } else if (IsImageExt(e.name)) {
                    if (ImGui::Button("添加精灵")) AddSpriteEntity(e.path);
                    ImGui::SameLine();
                    if (ImGui::Button("预览")) ImportAssetPath(e.path);
                    gfx::Texture tex = assetMgr_.LoadTexture(e.path);
                    if (tex.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(tex.Handle());
                } else if (IsScriptExt(e.name)) {
                    if (ImGui::Button("编辑")) OpenScriptEditor(e.path);
                    ImGui::SameLine();
                    if (ImGui::Button("外部打开")) OpenInExternalEditor(e.path);
                } else if (IsMaterialExt(e.name)) {
                    RequestMaterialThumbnail(e.path);
                    auto thumb = materialThumbs_.find(e.path);
                    if (thumb != materialThumbs_.end() &&
                        thumb->second.texId != ImTextureID_Invalid) {
                        tid = thumb->second.texId;
                        flipV = true;
                    }
                    // Material parameter summary under the sphere preview.
                    std::ifstream in(e.path, std::ios::binary);
                    if (in.is_open()) {
                        std::string text((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
                        std::string err;
                        core::Json root = core::Json::Parse(text, &err);
                        if (root.IsObject()) {
                            ImGui::Separator();
                            ImGui::TextDisabled("颜色: %s",
                                                root.Get("colorHex")
                                                    ? root.Get("colorHex")->GetString("#FFFFFF").c_str()
                                                    : "#FFFFFF");
                            ImGui::TextDisabled("金属度: %.2f  粗糙度: %.2f",
                                                root.Get("metallic")
                                                    ? root.Get("metallic")->GetNumber()
                                                    : 0.0,
                                                root.Get("roughness")
                                                    ? root.Get("roughness")->GetNumber()
                                                    : 0.8);
                            if (root.Get("albedoTex") &&
                                !root.Get("albedoTex")->GetString().empty())
                                ImGui::TextDisabled("贴图: %s",
                                                    root.Get("albedoTex")->GetString().c_str());
                        }
                    }
                }
                const float prevSize = std::min(140.0f, detailW - 24.0f);
                if (tid != ImTextureID_Invalid) {
                    ImGui::Image(tid, ImVec2(prevSize, prevSize),
                                 ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(1.0f, flipV ? 0.0f : 1.0f));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.0fx%.0f", prevSize, prevSize);
                } else if (IsModelExt(e.name)) {
                    ImGui::Dummy(ImVec2(prevSize, prevSize));
                    ImGui::SameLine();
                    ImGui::TextDisabled("生成缩略图中…");
                }
            }
            ImGui::Separator();
            if (ImGui::Button("删除资产", ImVec2(-1.0f, 0.0f))) DeleteSelectedAsset();
        }
        ImGui::EndChild();
        }
    }
    ImGui::End();
}

