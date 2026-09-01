#pragma once

// 地形编辑面板 —— 面板插件化（阶段 2，Task 14，沿用 Task 2-13 样板）：
// 原 EditorApp::BuildTerrainPanel（panels_world.inc）整体迁入本类。
//
// 状态边界：TerrainState（paintMode/brushRadius/...）被视口雕刻交互
// （editor_viewport）与场景塑形（editor_scene）读写，提升为共享结构并仍由
// EditorApp 持有，经 EditorContext::terrain 指针访问；RebuildTerrainMesh 是
// EditorApp 方法（多处共用），经 ctx.rebuildTerrainMesh 回调访问。
// 可见标志在过渡期指向 EditorApp::showTerrain_（阶段 3 收编后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class TerrainPanel : public IPanel {
public:
    explicit TerrainPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "地形编辑"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showTerrain_
};

} // namespace neon::editor
