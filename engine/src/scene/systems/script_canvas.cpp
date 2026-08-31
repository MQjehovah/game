#include "neon/scene/systems/script_canvas.hpp"

namespace neon::scene {

void ScriptCanvas::Begin() {
    draw2d_.clear();
}

void ScriptCanvas::Flush(gfx::Renderer& renderer, const gfx::Font& font2d) {
    for (const script::Draw2DCmd& c : draw2d_) {
        switch (c.kind) {
            case script::Draw2DCmd::Kind::Rect:
                // Textured quads use downward-v UVs (top row = v0): DrawQuad's
                // DEFAULT is the GL bottom-up convention, which drew every
                // script-canvas DrawSprite upside down.
                renderer.DrawQuad({c.x, c.y}, {c.w, c.h}, {c.r, c.g, c.b, c.a},
                                  c.texture, {0.0f, 0.0f}, {1.0f, 1.0f});
                break;
            case script::Draw2DCmd::Kind::RectOutline:
                renderer.DrawRectOutline({c.x, c.y, c.w, c.h}, c.thickness,
                                         {c.r, c.g, c.b, c.a});
                break;
            case script::Draw2DCmd::Kind::Text:
                if (font2d.Valid())
                    renderer.DrawText(font2d, c.text, {c.x, c.y}, c.size,
                                      {c.r, c.g, c.b, c.a}, c.centerX, c.centerY);
                break;
        }
    }
}

} // namespace neon::scene
