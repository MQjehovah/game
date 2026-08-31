void EditorApp::BuildTerrainPanel() {
    if (!showTerrain_) return;
    if (ImGui::Begin("地形编辑", &showTerrain_)) {
        const bool hasTerrain =
            selected_ >= 0 && selected_ < static_cast<int>(entities_.size()) &&
            entities_[static_cast<size_t>(selected_)].meshKey == "terrain";
        if (!hasTerrain) {
            ImGui::TextDisabled("请先选中一个 \"terrain\" 实体");
            ImGui::Checkbox("雕刻模式", &terrain_.paintMode);
            ImGui::End();
            return;
        }
        ImGui::Checkbox("雕刻模式", &terrain_.paintMode);
        ImGui::SameLine();
        ImGui::Checkbox("抬高", &terrain_.raise);
        ImGui::SameLine();
        if (ImGui::Button("降低")) {
            terrain_.raise = false;
        }
        if (ImGui::Button("重置为平地")) {
            SceneEntity& e = entities_[static_cast<size_t>(selected_)];
            e.terrainHeights_.assign(e.terrainHeights_.size(), 0.0f);
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        ImGui::DragFloat("笔刷半径", &terrain_.brushRadius, 0.1f, 0.5f, 30.0f);
        ImGui::DragFloat("笔刷强度", &terrain_.brushStrength, 0.01f, 0.005f, 2.0f);
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        if (ImGui::DragInt("细分", &e.terrainSegments_, 1, 4, 128)) {
            e.terrainSegments_ = std::max(4, std::min(e.terrainSegments_, 128));
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("尺寸", &e.terrainSize_, 1.0f, 4.0f, 500.0f)) {
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("高度缩放", &e.terrainHeightScale_, 0.05f, 0.1f, 10.0f)) {
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("LOD / 植被 (G2-3)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::InputInt("分块数##lod", &e.chunkGridDiv_, 1, 4)) {
                e.chunkGridDiv_ = std::max(0, std::min(e.chunkGridDiv_, 16));
                sceneDirty_ = true;
            }
            if (e.chunkGridDiv_ > 0) {
                if (ImGui::InputInt("LOD 层数##lod", &e.chunkLodLevels_, 1, 1)) {
                    e.chunkLodLevels_ = std::max(1, std::min(e.chunkLodLevels_, 5));
                    sceneDirty_ = true;
                }
                if (ImGui::InputInt("LOD 细分##lod", &e.chunkBaseSubdiv_, 1, 4)) {
                    e.chunkBaseSubdiv_ = std::max(2, std::min(e.chunkBaseSubdiv_, 64));
                    sceneDirty_ = true;
                }
                ImGui::TextDisabled("播放时按分块渲染, 近密远疏 (运行时 LOD)");
            }
            static char vegKeyBuf[128] = {};
            std::snprintf(vegKeyBuf, sizeof(vegKeyBuf), "%s", e.vegMeshKey_.c_str());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("植被网格##lod", vegKeyBuf, sizeof(vegKeyBuf))) {
                e.vegMeshKey_ = vegKeyBuf;
                sceneDirty_ = true;
            }
            int vegCount = static_cast<int>(e.vegCount_);
            if (ImGui::InputInt("植被数量##lod", &vegCount, 1, 20)) {
                e.vegCount_ = static_cast<uint32_t>(std::max(0, vegCount));
                sceneDirty_ = true;
            }
            if (e.vegCount_ > 0) {
                if (ImGui::DragFloat("植被尺寸##lod", &e.vegSize_, 0.05f, 0.1f, 8.0f)) {
                    e.vegSize_ = std::max(0.05f, e.vegSize_);
                    sceneDirty_ = true;
                }
                if (ImGui::DragFloat("Impostor 距离##lod", &e.vegImpostorDistance_, 1.0f, 1.0f, 500.0f)) {
                    e.vegImpostorDistance_ = std::max(1.0f, e.vegImpostorDistance_);
                    sceneDirty_ = true;
                }
                ImGui::TextDisabled("远处植被自动切换为 2D 面片 (Impostor)");
            }
        }
        ImGui::TextDisabled("提示: 雕刻模式下点击/拖拽视口即可塑形");
    }
    ImGui::End();
}

// P1-1 2D tilemap editor: paint cells with a texture path (or click a texture
// asset in the palette); cols/rows/cellSize resize the grid.
void EditorApp::BuildTilemapPanel() {
    if (!showTilemap_) return;
    if (ImGui::Begin("2D 地图", &showTilemap_)) {
        const bool has = selected_ >= 0 && selected_ < static_cast<int>(entities_.size()) &&
                         entities_[static_cast<size_t>(selected_)].meshKey == "tilemap";
        if (!has) {
            ImGui::TextDisabled("请先选中一个 \"tilemap\" 实体 (网格键填 tilemap)");
            ImGui::End();
            return;
        }
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        const size_t need = static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_;
        if (e.tilemapTiles_.size() != need) e.tilemapTiles_.resize(need);
        if (ImGui::DragInt("列", &e.tilemapCols_, 1, 1, 64)) {
            e.tilemapCols_ = std::max(1, std::min(e.tilemapCols_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            sceneDirty_ = true;
        }
        if (ImGui::DragInt("行", &e.tilemapRows_, 1, 1, 64)) {
            e.tilemapRows_ = std::max(1, std::min(e.tilemapRows_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("格大小", &e.tilemapCellSize_, 1.0f, 1.0f, 512.0f)) {
            e.tilemapCellSize_ = std::max(1.0f, e.tilemapCellSize_);
            e.scale = {e.tilemapCellSize_, e.tilemapCellSize_, 1.0f};
            sceneDirty_ = true;
        }
        static char texBuf[512] = {};
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("贴图路径", texBuf, sizeof(texBuf));
        // Palette: texture assets in the current project asset dir.
        if (ImGui::CollapsingHeader("贴图调色板", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginChild("##tile_palette", ImVec2(0, 120), true)) {
                for (const AssetEntry& a : assetEntries_) {
                    if (a.isDir) continue;
                    const std::string lower = ToLower(a.name);
                    if (lower.find(".png") == std::string::npos &&
                        lower.find(".jpg") == std::string::npos)
                        continue;
                    if (ImGui::Button(a.name.c_str())) {
                        std::snprintf(texBuf, sizeof(texBuf), "%s", a.path.c_str());
                    }
                    // P2-editor UX: drag a palette tile straight onto a cell.
                    if (ImGui::BeginDragDropSource()) {
                        tileDragPath_ = a.path;
                        ImGui::SetDragDropPayload("TILE_TEXTURE", tileDragPath_.data(),
                                                  tileDragPath_.size() + 1);
                        ImGui::Text("放置: %s", a.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::TextDisabled("点击格子放置当前贴图; 右键格子弹窗菜单请先清除文本后点击");
        const float cellW = std::max(ImGui::GetContentRegionAvail().x /
                                         static_cast<float>(std::max(e.tilemapCols_, 1)) - 4.0f,
                                     20.0f);
        for (int r = 0; r < e.tilemapRows_; ++r) {
            for (int c = 0; c < e.tilemapCols_; ++c) {
                size_t idx = static_cast<size_t>(r) * e.tilemapCols_ + c;
                ImGui::PushID(static_cast<int>(idx));
                std::string label = e.tilemapTiles_[idx].empty()
                                        ? "·"
                                        : "■";
                if (ImGui::Button(label.c_str(), ImVec2(cellW, 24))) {
                    if (texBuf[0] != '\0') {
                        e.tilemapTiles_[idx] = texBuf;
                        sceneDirty_ = true;
                    } else {
                        e.tilemapTiles_[idx].clear();
                        sceneDirty_ = true;
                    }
                }
                // P2-editor UX: drop a palette tile or an asset texture here.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TILE_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            sceneDirty_ = true;
                        }
                    } else if (const ImGuiPayload* p =
                                   ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            sceneDirty_ = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && !e.tilemapTiles_[idx].empty())
                    ImGui::SetTooltip("%s", e.tilemapTiles_[idx].c_str());
                if ((c + 1) % e.tilemapCols_ != 0) ImGui::SameLine();
                ImGui::PopID();
            }
        }
        if (ImGui::Button("清空地图")) {
            for (std::string& t : e.tilemapTiles_) t.clear();
            sceneDirty_ = true;
        }
    }
    ImGui::End();
}

void EditorApp::BuildPackagePanel() {
    if (!showPackage_) return;
    if (ImGui::Begin("打包", &showPackage_)) {
        ImGui::TextDisabled("项目目录 (game.json + assets/ 内容根：scenes/ prefabs/ scripts/ ui/ locales/)");
        if (ImGui::InputText("##pack_proj", projectDirBuf_, sizeof(projectDirBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            projectDir_ = projectDirBuf_;
            if (projectDir_.empty()) projectDir_ = ".";
            SaveEditorConfig();
        }
        ImGui::TextDisabled("输出目录");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##pack_out", package_.outDirBuf, sizeof(package_.outDirBuf));
        ImGui::Separator();
        // Export presets (Godot-style): edit game.json's "export" block.
        ImGui::TextUnformatted("导出配置 (game.json \"export\")");
        static std::string expProj;
        static char expPlatform[64] = "windows";
        static char expIcon[512] = {};
        static char expDesc[512] = {};
        if (expProj != projectDir_) {
            expProj = projectDir_;
            std::strcpy(expPlatform, "windows");
            expIcon[0] = '\0';
            expDesc[0] = '\0';
            std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                std::string err;
                core::Json root = core::Json::Parse(text, &err);
                if (const core::Json* ex = root.Get("export")) {
                    if (const core::Json* p = ex->Get("platform"))
                        std::strncpy(expPlatform, p->GetString().c_str(),
                                     sizeof(expPlatform) - 1);
                    if (const core::Json* i = ex->Get("icon"))
                        std::strncpy(expIcon, i->GetString().c_str(), sizeof(expIcon) - 1);
                    if (const core::Json* d = ex->Get("description"))
                        std::strncpy(expDesc, d->GetString().c_str(), sizeof(expDesc) - 1);
                }
            }
        }
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputText("平台", expPlatform, sizeof(expPlatform));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("图标", expIcon, sizeof(expIcon));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("说明", expDesc, sizeof(expDesc));
        if (ImGui::Button("保存导出配置")) {
            std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            std::string err;
            core::Json root = core::Json::Parse(text, &err);
            if (!root.IsObject()) {
                NEON_LOG_ERROR("Export: cannot read '%s/game.json'", projectDir_.c_str());
            } else {
                core::Json ex;
                ex.type_ = core::Json::Type::Object;
                auto str = [](const char* s) {
                    core::Json j;
                    j.type_ = core::Json::Type::String;
                    j.string_ = s;
                    return j;
                };
                ex.object_["platform"] = str(expPlatform);
                if (expIcon[0]) ex.object_["icon"] = str(expIcon);
                if (expDesc[0]) ex.object_["description"] = str(expDesc);
                root.object_["export"] = ex;
                std::ofstream out(projectDir_ + "/game.json", std::ios::binary);
                if (out.is_open()) {
                    out << core::JsonWriter::WritePretty(root);
                    NEON_LOG_INFO("Export: preset saved -> %s/game.json", projectDir_.c_str());
                } else {
                    NEON_LOG_ERROR("Export: cannot write '%s/game.json'", projectDir_.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("windows | linux | macos | web");
        ImGui::Separator();
        if (ImGui::Button("一键打包")) RunPackage();
        ImGui::SameLine();
        ImGui::TextDisabled("生成 game.pack + run.bat + neon_game.exe");
        ImGui::Separator();

        if (package_.ran) {
            if (package_.report.ok) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "打包成功");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "打包失败 (%zu 个错误)",
                                   package_.report.errors.size());
            }
            ImGui::TextDisabled("文件: %zu    字节: %zu", package_.report.fileCount,
                                package_.report.bytesWritten);
            if (!package_.report.packPath.empty()) ImGui::TextUnformatted(package_.report.packPath.c_str());
            if (!package_.report.runScriptPath.empty())
                ImGui::TextUnformatted(package_.report.runScriptPath.c_str());
            if (!package_.report.playerPath.empty())
                ImGui::TextUnformatted(package_.report.playerPath.c_str());
            if (!package_.report.errors.empty()) {
                ImGui::TextUnformatted("错误:");
                for (const std::string& e : package_.report.errors)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "- %s", e.c_str());
            }
            if (!package_.report.warnings.empty()) {
                ImGui::TextUnformatted("警告:");
                for (const std::string& w : package_.report.warnings)
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "- %s", w.c_str());
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled(
            "neon_game.exe 为数据驱动播放器（读取 game.pack）。\n"
            "若尚未构建 neon_game，播放器复制会给出警告，打包仍会生成 game.pack 与 run.bat。");
    }
    ImGui::End();
}

