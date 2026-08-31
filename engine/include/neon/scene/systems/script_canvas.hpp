#pragma once
#include <cstddef>
#include <vector>

#include "neon/gfx/renderer.hpp"
#include "neon/script/bindings.hpp"

namespace neon::scene {

// 脚本 2D 画布：on_render 产生的立即模式绘制命令（DrawRect/DrawRectOutline/
// DrawText bindings），flush 进 renderer 的 2D overlay。纯机械拆分自
// GameRuntime 的 draw2d_ + FlushDraw2D/FlushCanvas（Task 10）：命令容器 +
// flush 逻辑迁入此类，GameRuntime 保留 on_render 接线与 FlushCanvas 转发。
class ScriptCanvas {
public:
    void Begin(); // 清空本帧命令（原在 on_render 前）
    void Add(const script::Draw2DCmd& cmd) { draw2d_.push_back(cmd); }
    void Flush(gfx::Renderer& renderer, const gfx::Font& font2d);
    bool Empty() const { return draw2d_.empty(); }
    size_t Count() const { return draw2d_.size(); }
    // 暴露命令容器指针供 scriptCtx_.draw2d 接线（on_render 期间非空；脚本
    // binding 直接 push 进本容器）。
    std::vector<script::Draw2DCmd>* Commands() { return &draw2d_; }

private:
    std::vector<script::Draw2DCmd> draw2d_;
};

} // namespace neon::scene
