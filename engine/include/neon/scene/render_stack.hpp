#pragma once

// NeonEngine RenderStack (G2-1 / A). A scene-level, reflection-driven description
// of which post-process effects are active and their parameters. It lives in the
// scene layer (not gfx) because it uses the scene reflection system AND consumes
// gfx colour/vec types; the scene runtime reads it and converts it into the gfx
// CompositeParams that PostGraph::Execute consumes. Keeping it a reflectable
// struct means:
//   * the editor property panel is generated automatically (any field is a
//     FieldType::Number / Bool / Color editable via the shared schema editor),
//   * the whole stack JSON round-trips for scene save/load,
//   * a script can read/write a single parameter by name (C7 accessors).
//
// This is the data-driven half of the "open the render pipeline to the front
// end" goal (Infernux renders its RenderStack as an editable asset with an
// inspector; here the same is achieved via the reflection-driven inspector).
// Mapping to PostGraph::FrameParams/CompositeParams is done by the renderer:
//   bloom            -> bloomPass + CompositeParams (uThreshold/uStrength)
//   tonemap/exposure -> CompositeParams::tonemapEnabled / exposure
//   fog              -> CompositeParams::fogColor / fogDensity
//   ssao/vol/ssr     -> FrameParams::ssaoPass/volumetricPass/ssrPass + intensities

#include <utility>

#include "neon/gfx/color.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/component_reflect.hpp"

namespace neon::scene {

// Render settings for a whole scene: the active post effects + their params.
// A scene can carry one of these (scene-level data); the editor inspects it via
// reflection. All fields are scalar/color so a FieldType editor can edit them.
struct RenderStack {
    // Ambient-occlusion / volumetric / screen-space-reflection effect groups.
    bool ssao = false;
    float ssaoIntensity = 1.0f;
    bool volumetric = false;
    float volumetricStrength = 1.0f;
    bool ssr = false;
    float ssrStrength = 1.0f;
    // Bloom plugin stack: bright threshold + add-back strength.
    bool bloom = true;
    float bloomThreshold = 1.0f;
    float bloomStrength = 0.35f;
    // Tone mapping + exposure.
    bool tonemap = true;
    float exposure = 1.0f;
    // Fog (a common "atmosphere effect" in the stack).
    bool fog = false;
    gfx::Color fogColor{1.0f, 1.0f, 1.0f, 1.0f};
    float fogDensity = 0.02f;

    inline static const auto kFields = ReflectFields(
        Field("ssao", "SSAO", FieldType::Bool, &RenderStack::ssao),
        Field("ssaoIntensity", "SSAO 强度", FieldType::Number, &RenderStack::ssaoIntensity,
              1, 0, 10, 0.01),
        Field("volumetric", "体积光", FieldType::Bool, &RenderStack::volumetric),
        Field("volumetricStrength", "体积光强度", FieldType::Number,
              &RenderStack::volumetricStrength, 1, 0, 10, 0.01),
        Field("ssr", "SSR", FieldType::Bool, &RenderStack::ssr),
        Field("ssrStrength", "SSR 强度", FieldType::Number, &RenderStack::ssrStrength, 1, 0, 10, 0.01),
        Field("bloom", "泛光", FieldType::Bool, &RenderStack::bloom),
        Field("bloomThreshold", "泛光阈值", FieldType::Number, &RenderStack::bloomThreshold,
              1, 0, 10, 0.01),
        Field("bloomStrength", "泛光强度", FieldType::Number, &RenderStack::bloomStrength,
              0.35, 0, 10, 0.01),
        Field("tonemap", "色调映射", FieldType::Bool, &RenderStack::tonemap),
        Field("exposure", "曝光", FieldType::Number, &RenderStack::exposure, 1, 0, 10, 0.01),
        Field("fog", "雾", FieldType::Bool, &RenderStack::fog),
        Field("fogColor", "雾色", FieldType::Color, &RenderStack::fogColor),
        Field("fogDensity", "雾密度", FieldType::Number, &RenderStack::fogDensity, 0.02, 0, 1, 0.001));

    core::Json ToJson() const { return kFields.ToJson(*this); }
    bool FromJson(const core::Json& j, std::string* err = nullptr) {
        return kFields.FromJson(j, *this, err);
    }
};

} // namespace neon::scene
