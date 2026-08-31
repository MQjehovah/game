#include "neon/scene/systems/ui_system.hpp"

namespace neon::scene {

// 渲染数据驱动 UI（挂在合成帧之上；宿主在 EndScene 之后调用，保证菜单/HUD
// 保持作者色而非被 ACES tone-mapping）。viewportSize 是 live 设计空间视口，
// 布局系统据此自适应。空系统时直接 no-op（与其它转发一致）。
void UiSystem::Draw(gfx::Renderer& renderer, const gfx::Font& font,
                    const ui::UiTextureLoader& loadTexture,
                    const math::Vec2& viewportSize) {
    if (!ui_) return;
    ui_->Draw(renderer, font, loadTexture, viewportSize);
}

} // namespace neon::scene
