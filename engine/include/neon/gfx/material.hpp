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
    // G4 terrain splatmap layers: grass uses a realistic albedo texture, dirt
    // and rock use flat colors. Blended by the vertex splat weights (r=grass,
    // g=dirt, b=rock). Only read when the terrain shader is active.
    TextureHandle grassTex;
    Color dirtColor{0.50f, 0.42f, 0.28f, 1.0f};
    Color rockColor{0.55f, 0.52f, 0.48f, 1.0f};
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    float shininess = 24.0f;
    float metallic = 0.0f;
    float roughness = 0.8f;
    // UV repeat multiplier applied to the material's base UVs (1 = no tiling).
    float uvRepeat = 1.0f;
    // Scalar strength multipliers applied in the lit shader: occlusion scales
    // the ambient contribution (0 = ignore the AO map, 1 = full occlusion),
    // emissive scales the emissive texture contribution.
    float aoStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    bool lit = true;
    bool transparent = false;
    // Casts a directional-light (CSM) shadow. Default true so trees/props cast;
    // set false on large receivers (ground plane, water, terrain) that would
    // otherwise self-shadow into a black swath.
    bool castShadow = true;
    // glTF doubleSided: render both faces (disable back-face culling).
    bool doubleSided = false;
    // glTF MASK: discard fragments with albedo.a < alphaCutoff (crisp cutout
    // for foliage/hair cards), instead of soft translucent blending.
    bool alphaTest = false;
    float alphaCutoff = 0.5f;

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
