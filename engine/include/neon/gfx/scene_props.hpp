#pragma once

#include <string>
#include <vector>

#include "neon/gfx/mesh.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"

namespace neon::gfx {

class Renderer;

// Procedural, vertex-colored scene props shared by the editor and the data-
// driven runtime (playtest). Meshes carry baked vertex colors: the lit shader
// multiplies uTint * vColor, so a single mesh can mix e.g. a brown trunk with a
// green canopy regardless of the entity's material tint.

// Rolling green heightfield with a shallow lake carved in the SW corner.
Mesh MakeTerrainMesh(Renderer& renderer, int segments = 48, float size = 60.0f,
                     const std::string& name = "terrain");

// Pine tree: box trunk + two stacked green cones.
Mesh MakeTreeMesh(Renderer& renderer, const std::string& name = "tree");

// Farmhouse: tan cube body + reddish pyramid roof (4-segment cone).
Mesh MakeHouseMesh(Renderer& renderer, const std::string& name = "house");

// Simple villager: colored tunic (box) + skin-tone head (sphere).
Mesh MakeNPCMesh(Renderer& renderer, const math::Vec4& tunic,
                 const std::string& name = "npc");

// Low green bush / shrub.
Mesh MakeBushMesh(Renderer& renderer, const std::string& name = "bush");

// Playable hero: blue/gold armor figure with a sword.
Mesh MakeHeroMesh(Renderer& renderer, const std::string& name = "hero");

// Hostile wolf (low-poly): gray body, head, legs, tail.
Mesh MakeWolfMesh(Renderer& renderer, const std::string& name = "wolf");

// Small glowing fireball projectile.
Mesh MakeFireballMesh(Renderer& renderer, const std::string& name = "fireball");

// Stylised grass tuft: crossed blades with a dark-base -> light-tip gradient,
// vertex-coloured (no texture). Instanced by the terrain vegetation scatter to
// carpet large areas of ground with grass.
Mesh MakeGrassMesh(Renderer& renderer, const std::string& name = "grass");

} // namespace neon::gfx
