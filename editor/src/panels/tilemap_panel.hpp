#pragma once

// 2D 瓦片地图面板 —— 面板插件化（阶段 2，Task 14，沿用 Task 2-13 样板）：
// 原 EditorApp::BuildTilemapPanel（panels_world.inc）整体迁入本类。
//
// 状态边界：无共享持久状态（选中实体的 tilemap* 字段 + ctx.assetEntries 调色板），
// 面板私有状态 tileDragPath_ 与函数内 static texBuf 迁入本类。
// 可见标志在过渡期指向 EditorApp::showTilemap_（阶段 3 收编后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <string>

#include "editor_context.hpp"

namespace neon::editor {

class TilemapPanel : public IPanel {
public:
    explicit TilemapPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "2D 地图"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showTilemap_
    std::string tileDragPath_;  // 调色板格子拖拽的贴图路径（原 EditorApp::tileDragPath_）
};

} // namespace neon::editor
