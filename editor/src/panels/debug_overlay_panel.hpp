#pragma once

// 调试覆盖层面板 —— 面板插件化（阶段 2，Task 10，沿用 Task 2-9 样板）：
// 原 EditorApp::BuildDebugOverlayPanel（ImGui 面板，editor_plugins.cpp）+ 图层
// 探针缓存状态（debugProbeField_/Bounds_/Res_/Dirty_）整体迁入本类。
//
// 本面板有两部分：
// - Draw(ctx)：F3 面板（4 个图层开关 checkbox）。开关状态（debugColliders 等）
//   被视口（画物理线框，editor_viewport:241）与 DrawOverlay（画图层）共用，
//   仍由 EditorApp 持有，经 EditorContext::debug* 指针访问；
// - DrawOverlay(ctx, cam)：把图层画进视口（音频衰减球/导航格/光照探针），由
//   EditorApp::DrawDebugOverlay 转发（editor_viewport:375 在主场景后调用）。
//   探针字段缓存（field/bounds/res/dirty）是本面板私有状态，迁入本类。
//
// 可见标志在过渡期指向 EditorApp::showDebugOverlay_（F3 切换 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <vector>

#include "editor_context.hpp"
#include "neon/gfx/light_probe.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {
class Camera;
}

namespace neon::editor {

class DebugOverlayPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showDebugOverlay_）。
    explicit DebugOverlayPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "调试覆盖层"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

    // 画视口图层（音频衰减球 / 导航格 / 光照探针）。由 EditorApp::DrawDebugOverlay
    // 转发（editor_viewport 在主场景之后调用）。
    void DrawOverlay(EditorContext& ctx, const gfx::Camera& cam);

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showDebugOverlay_
    // 光照探针图层缓存（原 EditorApp::debugProbeField_ 等私有状态，迁入本类）。
    std::vector<gfx::IrradianceProbe> probeField_;
    math::AABB probeBounds_{};
    int probeRes_ = 4;
    bool probeDirty_ = true;
};

} // namespace neon::editor
