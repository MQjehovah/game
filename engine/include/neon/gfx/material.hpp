#pragma once
#include "neon/gfx/backend.hpp"
#include "neon/gfx/color.hpp"

namespace neon::gfx {

struct Material {
    ShaderHandle shader;
    TextureHandle albedo;
    TextureHandle metallicRoughness; // G=roughness, B=metallic
    TextureHandle occlusion;         // R channel
    TextureHandle emissive;
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    float shininess = 24.0f;
    float metallic = 0.0f;
    float roughness = 0.8f;
    bool lit = true;
    bool transparent = false;

    static Material Lit(TextureHandle albedo_, const Color& tint_ = Color::White, float shininess_ = 24.0f) {
        return {ShaderHandle{}, albedo_, {}, {}, {}, tint_, shininess_, 0.0f, 0.8f, true, false};
    }

    static Material Unlit(TextureHandle albedo_, const Color& tint_ = Color::White) {
        return {ShaderHandle{}, albedo_, {}, {}, {}, tint_, 0.0f, 0.0f, 0.8f, false, false};
    }
};

} // namespace neon::gfx
