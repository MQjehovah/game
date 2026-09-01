#include "panels/tilemap_panel.hpp"

// 2D 瓦片地图面板实现 = 原 EditorApp::BuildTilemapPanel（panels_world.inc:90-195）
// 方法体逐行迁移：EditorApp 成员（showTilemap_/selected_/entities_/assetEntries_/
// sceneDirty_/tileDragPath_）改本类 visible_ / ctx 指针 / 本类成员。行为零变化。
// ToLower 以本地副本带入（同其它面板模式）。

#include <algorithm>
#include <cstdio>
#include <string>

#include "editor.hpp"
#include "imgui.h"

namespace neon::editor {

namespace {
std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

void TilemapPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("2D 地图", visible_)) {
        const int sel = ctx.selected ? *ctx.selected : -1;
        const bool has = sel >= 0 && sel < static_cast<int>(ctx.entities->size()) &&
                         (*ctx.entities)[static_cast<size_t>(sel)].meshKey == "tilemap";
        if (!has) {
            ImGui::TextDisabled("请先选中一个 \"tilemap\" 实体 (网格键填 tilemap)");
            ImGui::End();
            return;
        }
        SceneEntity& e = (*ctx.entities)[static_cast<size_t>(sel)];
        const size_t need = static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_;
        if (e.tilemapTiles_.size() != need) e.tilemapTiles_.resize(need);
        if (ImGui::DragInt("列", &e.tilemapCols_, 1, 1, 64)) {
            e.tilemapCols_ = std::max(1, std::min(e.tilemapCols_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            *ctx.sceneDirty = true;
        }
        if (ImGui::DragInt("行", &e.tilemapRows_, 1, 1, 64)) {
            e.tilemapRows_ = std::max(1, std::min(e.tilemapRows_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            *ctx.sceneDirty = true;
        }
        if (ImGui::DragFloat("格大小", &e.tilemapCellSize_, 1.0f, 1.0f, 512.0f)) {
            e.tilemapCellSize_ = std::max(1.0f, e.tilemapCellSize_);
            e.scale = {e.tilemapCellSize_, e.tilemapCellSize_, 1.0f};
            *ctx.sceneDirty = true;
        }
        static char texBuf[512] = {};
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("贴图路径", texBuf, sizeof(texBuf));
        // Palette: texture assets in the current project asset dir.
        if (ImGui::CollapsingHeader("贴图调色板", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginChild("##tile_palette", ImVec2(0, 120), true)) {
                for (const AssetEntry& a : *ctx.assetEntries) {
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
                        *ctx.sceneDirty = true;
                    } else {
                        e.tilemapTiles_[idx].clear();
                        *ctx.sceneDirty = true;
                    }
                }
                // P2-editor UX: drop a palette tile or an asset texture here.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TILE_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            *ctx.sceneDirty = true;
                        }
                    } else if (const ImGuiPayload* p =
                                   ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            *ctx.sceneDirty = true;
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
            *ctx.sceneDirty = true;
        }
    }
    ImGui::End();
}

} // namespace neon::editor
