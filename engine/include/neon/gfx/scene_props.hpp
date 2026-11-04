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

// Textured stone farmhouse: walls with a door opening (front) and two window
// openings (sides). Vertices are white (baked tint) and carry per-face 0..1 UVs
// so an entity's albedo texture tiles across them (uvRepeat per metre). Winding
// is outside-in so the lit shader's back-face culling keeps the exterior.
Mesh MakeHouseMesh(Renderer& renderer, const std::string& name = "house");

// Tiled gable roof (two sloped panels + end triangles + a chimney) for a
// MakeHouseMesh. Separate meshKey so it gets its own albedo (roof tiles).
Mesh MakeRoofMesh(Renderer& renderer, const std::string& name = "house_roof");

// Simple villager: colored tunic (box) + skin-tone head (sphere).
Mesh MakeNPCMesh(Renderer& renderer, const math::Vec4& tunic,
                 const std::string& name = "npc");

// Low green bush / shrub.
Mesh MakeBushMesh(Renderer& renderer, const std::string& name = "bush");

// Playable hero: blue/gold armor figure with a sword.
Mesh MakeHeroMesh(Renderer& renderer, const std::string& name = "hero");

// Xianxia placeholders: a tall pine, a weathered rock spire, a stone lantern
// and a robed cultivator. All are vertex-coloured and origin-at-feet so scene
// entities can place them directly on the terrain.
Mesh MakePineMesh(Renderer& renderer, const std::string& name = "pine");
Mesh MakeBroadleafMesh(Renderer& renderer, const std::string& name = "broadleaf");
// Coherent low-poly tree: tapered trunk, branches and faceted leaf clusters.
// `pink` swaps the leaf palette for a xianxia blossom tree.
Mesh MakeStylizedTreeMesh(Renderer& renderer, const std::string& name, bool pink);
Mesh MakeBlossomMesh(Renderer& renderer, const std::string& name = "blossom");
// Textured leaf-card canopy for the sculpted island tree trunk (alpha-tested).
Mesh MakeLeafCanopyMesh(Renderer& renderer, const std::string& name = "leaf_canopy");
// Craggy cliff face laid over the right-hand terrain slope; noise-displaced
// grid with UVs so a rock texture and normal map hide the smooth splat wall.
Mesh MakeCliffFaceMesh(Renderer& renderer, const std::string& name = "cliff_face");
// Flat XZ cloud-sea card (two triangles) for filling the valley below the path.
Mesh MakeCloudPlaneMesh(Renderer& renderer, const std::string& name = "cloud_sea");
// Distant jagged mountain wall; use a few scaled/offset copies for haze layers.
Mesh MakeMountainRidgeMesh(Renderer& renderer, const std::string& name = "mountain_ridge");
Mesh MakeRockPillarMesh(Renderer& renderer, const std::string& name = "rock_pillar");
Mesh MakeBoulderMesh(Renderer& renderer, const std::string& name = "boulder");
Mesh MakeStoneLanternMesh(Renderer& renderer, const std::string& name = "stone_lantern");
Mesh MakeCultivatorMesh(Renderer& renderer, const std::string& name = "cultivator");
// Ground cover: a fan of drooping fern fronds and an autumn-red shrub.
Mesh MakeFernMesh(Renderer& renderer, const std::string& name = "fern");
Mesh MakeRedShrubMesh(Renderer& renderer, const std::string& name = "red_shrub");
// A single-draw stone-slab path (individual box slabs with visible seams).
Mesh MakeStonePathMesh(Renderer& renderer, const std::string& name = "stone_path");

// Hostile wolf (low-poly): gray body, head, legs, tail.
Mesh MakeWolfMesh(Renderer& renderer, const std::string& name = "wolf");

// Small glowing fireball projectile.
Mesh MakeFireballMesh(Renderer& renderer, const std::string& name = "fireball");

// Stylised grass tuft: crossed blades with a dark-base -> light-tip gradient,
// vertex-coloured (no texture). Instanced by the terrain vegetation scatter to
// carpet large areas of ground with grass.
Mesh MakeGrassMesh(Renderer& renderer, const std::string& name = "grass");

} // namespace neon::gfx
