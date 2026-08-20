#pragma once

// Umbrella header: include everything the engine exposes.
#include "neon/math/math.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/transform.hpp"

#include "neon/core/log.hpp"
#include "neon/core/time.hpp"
#include "neon/core/rng.hpp"
#include "neon/core/config.hpp"
#include "neon/core/app.hpp"

#include "neon/ecs/world.hpp"

#include "neon/platform/window.hpp"
#include "neon/platform/input.hpp"

#include "neon/gfx/color.hpp"
#include "neon/gfx/backend.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/gfx/shader.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/particles.hpp"

#include "neon/audio/audio.hpp"
#include "neon/physics/physics.hpp"
#include "neon/ui/ui.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/scene/scene.hpp"
