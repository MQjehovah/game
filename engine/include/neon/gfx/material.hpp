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
    // Scalar strength multipliers applied in the lit shader: occlusion scales
    // the ambient contribution (0 = ignore the AO map, 1 = full occlusion),
    // emissive scales the emissive texture contribution.
    float aoStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    bool lit = true;
    bool transparent = false;

    static Material Lit(TextureHandle albedo_, const Color& tint_ = Color::White, float shininess_ = 24.0f) {
        Material m;
        m.albedo = albedo_;
        m.tint = tint_;
        m.shininess = shininess_;
        m.lit = true;
        return m;
    }

    static Material Unlit(TextureHandle albedo_, const Color& tint_ = Color::White) {
        Material m;
        m.albedo = albedo_;
        m.tint = tint_;
        m.lit = false;
        return m;
    }
};

} // namespace neon::gfx
