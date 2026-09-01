#include "panels/terrain_panel.hpp"

// 地形编辑面板实现 = 原 EditorApp::BuildTerrainPanel（panels_world.inc:1-86）
// 方法体逐行迁移：EditorApp 成员（showTerrain_/terrain_/selected_/entities_/
// sceneDirty_ + RebuildTerrainMesh）改本类 visible_ / ctx.terrain / ctx 指针 /
// ctx.rebuildTerrainMesh 回调。行为零变化。SceneEntity 完整类型经 editor.hpp。

#include <algorithm>
#include <cstdio>

#include "editor.hpp"
#include "imgui.h"

namespace neon::editor {

void TerrainPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("地形编辑", visible_)) {
        const int sel = ctx.selected ? *ctx.selected : -1;
        const bool hasTerrain =
            sel >= 0 && sel < static_cast<int>(ctx.entities->size()) &&
            (*ctx.entities)[static_cast<size_t>(sel)].meshKey == "terrain";
        if (!hasTerrain) {
            ImGui::TextDisabled("请先选中一个 \"terrain\" 实体");
            ImGui::Checkbox("雕刻模式", &ctx.terrain->paintMode);
            ImGui::End();
            return;
        }
        ImGui::Checkbox("雕刻模式", &ctx.terrain->paintMode);
        ImGui::SameLine();
        ImGui::Checkbox("抬高", &ctx.terrain->raise);
        ImGui::SameLine();
        if (ImGui::Button("降低")) {
            ctx.terrain->raise = false;
        }
        if (ImGui::Button("重置为平地")) {
            SceneEntity& e = (*ctx.entities)[static_cast<size_t>(sel)];
            e.terrainHeights_.assign(e.terrainHeights_.size(), 0.0f);
            ctx.rebuildTerrainMesh(e);
            *ctx.sceneDirty = true;
        }
        ImGui::DragFloat("笔刷半径", &ctx.terrain->brushRadius, 0.1f, 0.5f, 30.0f);
        ImGui::DragFloat("笔刷强度", &ctx.terrain->brushStrength, 0.01f, 0.005f, 2.0f);
        SceneEntity& e = (*ctx.entities)[static_cast<size_t>(sel)];
        if (ImGui::DragInt("细分", &e.terrainSegments_, 1, 4, 128)) {
            e.terrainSegments_ = std::max(4, std::min(e.terrainSegments_, 128));
            ctx.rebuildTerrainMesh(e);
            *ctx.sceneDirty = true;
        }
        if (ImGui::DragFloat("尺寸", &e.terrainSize_, 1.0f, 4.0f, 500.0f)) {
            ctx.rebuildTerrainMesh(e);
            *ctx.sceneDirty = true;
        }
        if (ImGui::DragFloat("高度缩放", &e.terrainHeightScale_, 0.05f, 0.1f, 10.0f)) {
            ctx.rebuildTerrainMesh(e);
            *ctx.sceneDirty = true;
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("LOD / 植被 (G2-3)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::InputInt("分块数##lod", &e.chunkGridDiv_, 1, 4)) {
                e.chunkGridDiv_ = std::max(0, std::min(e.chunkGridDiv_, 16));
                *ctx.sceneDirty = true;
            }
            if (e.chunkGridDiv_ > 0) {
                if (ImGui::InputInt("LOD 层数##lod", &e.chunkLodLevels_, 1, 1)) {
                    e.chunkLodLevels_ = std::max(1, std::min(e.chunkLodLevels_, 5));
                    *ctx.sceneDirty = true;
                }
                if (ImGui::InputInt("LOD 细分##lod", &e.chunkBaseSubdiv_, 1, 4)) {
                    e.chunkBaseSubdiv_ = std::max(2, std::min(e.chunkBaseSubdiv_, 64));
                    *ctx.sceneDirty = true;
                }
                ImGui::TextDisabled("播放时按分块渲染, 近密远疏 (运行时 LOD)");
            }
            static char vegKeyBuf[128] = {};
            std::snprintf(vegKeyBuf, sizeof(vegKeyBuf), "%s", e.vegMeshKey_.c_str());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("植被网格##lod", vegKeyBuf, sizeof(vegKeyBuf))) {
                e.vegMeshKey_ = vegKeyBuf;
                *ctx.sceneDirty = true;
            }
            int vegCount = static_cast<int>(e.vegCount_);
            if (ImGui::InputInt("植被数量##lod", &vegCount, 1, 20)) {
                e.vegCount_ = static_cast<uint32_t>(std::max(0, vegCount));
                *ctx.sceneDirty = true;
            }
            if (e.vegCount_ > 0) {
                if (ImGui::DragFloat("植被尺寸##lod", &e.vegSize_, 0.05f, 0.1f, 8.0f)) {
                    e.vegSize_ = std::max(0.05f, e.vegSize_);
                    *ctx.sceneDirty = true;
                }
                if (ImGui::DragFloat("Impostor 距离##lod", &e.vegImpostorDistance_, 1.0f, 1.0f, 500.0f)) {
                    e.vegImpostorDistance_ = std::max(1.0f, e.vegImpostorDistance_);
                    *ctx.sceneDirty = true;
                }
                ImGui::TextDisabled("远处植被自动切换为 2D 面片 (Impostor)");
            }
        }
        ImGui::TextDisabled("提示: 雕刻模式下点击/拖拽视口即可塑形");
    }
    ImGui::End();
}

} // namespace neon::editor
