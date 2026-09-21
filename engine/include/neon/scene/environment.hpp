#pragma once

#include <string>

#include "neon/core/json.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {

// Reads environment fields (ambient light, sky/atmosphere, skybox, fog, exposure
// and an optional `resource` path) from a JSON object into `out`. Unknown keys
// are ignored and missing keys keep `out`'s current values, so this one reader
// serves the scene's top-level `environment` block, an entity's `environment`
// component and standalone `environments/*.env.json` resources (the reusable
// asset a WorldEnvironment node references). Returns false only when the value
// is not an object.
bool EnvironmentFromJson(const core::Json& json, SceneEnvironment& out, std::string* err);

// Writes the environment fields into `obj` (must be an object). Shares the field
// set with EnvironmentFromJson so the two directions cannot drift apart.
void EnvironmentToJson(const SceneEnvironment& env, core::Json& obj);

} // namespace neon::scene
