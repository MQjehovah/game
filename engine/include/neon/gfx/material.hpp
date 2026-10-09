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
    // A2: tangent-space normal map. No per-vertex tangents are stored; the lit
    // shader reconstructs a per-fragment TBN from screen-space derivatives of
    // world position + UV (MikkTSpace-quality for the common case, zero layout
    // change, works for every existing mesh). normalScale controls the
    // perturbation strength (1 = authored, <1 flattened, >1 exaggerated).
    TextureHandle normalMap;
    float normalScale = 1.0f;
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
    // Additive blend (glow decals, light pools). Honoured while transparent is
    // also true (depth test on, depth write off).
    bool additive = false;
    // Depth test for this draw (default on). Set false for overlays that must
    // paint over the scene regardless of depth (e.g. the fog-of-war mask).
    bool depthTest = true;
    // Depth-projected decal: the mesh is a flat quad at the decal origin and the
    // shader rebuilds the scene surface from the resolved main-pass depth, so
    // the decal conforms to slopes/stairs instead of floating above them (no
    // z-fighting, no 0.02 lift). Requires a sampleable scene depth; the renderer
    // silently falls back to a plain unlit quad when none is available.
    bool decal = false;
    // Casts a directional-light (CSM) shadow. Default true so trees/props cast;
    // set false on large receivers (ground plane, water, terrain) that would
    // otherwise self-shadow into a black swath.
    bool castShadow = true;
    bool receiveShadow = true;
    // glTF doubleSided: render both faces (disable back-face culling).
    bool doubleSided = false;
    // glTF MASK: discard fragments with albedo.a < alphaCutoff (crisp cutout
    // for foliage/hair cards), instead of soft translucent blending.
    bool alphaTest = false;
    float alphaCutoff = 0.5f;
    // Polygon offset (slope-scaled depth bias): pushes this material's
    // fragments toward/farther-from the camera in depth space so coplanar
    // layered content (decals painted on floors, water sheets laid over a
    // riverbed) resolves deterministically instead of z-fighting. 0 = off;
    // 1 = the classic "lift a coplanar overlay" amount (applied to both the
    // slope factor and the units).
    float polygonOffset = 0.0f;
    // Selection / edge glow: a Fresnel rim term added on top of the lit color
    // (see kLitFragmentShader). `strength` 0 = off; > 1 pushes the silhouette
    // into HDR so the bloom pass turns it into a real glowing outline around the
    // mesh edge (selected unit, target lock, ability preview). `color` tints it.
    Color highlightColor{1.0f, 1.0f, 1.0f, 1.0f};
    float highlightStrength = 0.0f;
    // F6 clearcoat: a second dielectric specular lobe on top of the base BRDF
    // (car paint / varnish / wet surfaces). 0 = off; 1 = full coverage with the
    // base specular attenuated. clearcoatRoughness controls its highlight width.
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.1f;
    // Real water surface: animated procedural wave normals, Fresnel blend between
    // a depth body colour and the IBL/environment reflection, plus a sharp sun
    // highlight. When on, the lit shader replaces the standard PBR result with
    // the water model. The water params below are only read when water == true.
    bool water = false;
    float waterWaveScale = 0.5f;     // spatial frequency of the wave field
    float waterWaveSpeed = 0.8f;     // animation speed
    float waterWaveStrength = 0.6f;  // normal perturbation amount
    float waterSpecular = 2.5f;      // sun highlight intensity
    float waterSpecPower = 220.0f;   // sun highlight sharpness
    Color waterDeep{0.03f, 0.11f, 0.17f, 1.0f};    // deep-water body colour
    Color waterShallow{0.11f, 0.42f, 0.50f, 1.0f}; // grazing/near colour tint
    float waterRefraction = 0.06f;  // screen-space refraction UV strength
    float waterAbsorb = 0.18f;      // Beer-Lambert density (higher = darker/opaque)
    float waterReflection = 0.85f;  // planar-reflection blend weight
    float waterFoam = 1.0f;         // contact/shore foam strength

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
