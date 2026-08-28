#pragma once
#include "neon/neon.hpp"
#include "neon/anim/anim.hpp"

namespace neon::demo {

struct DemoAssets {
    gfx::Texture glow;
    gfx::Texture ground;
    gfx::Texture crate;
    gfx::Texture pillar;
    gfx::Texture heart;

    gfx::Mesh cube;
    gfx::Mesh sphere;
    gfx::Mesh plane;
    gfx::Mesh cylinder;
    gfx::Mesh playerMesh;
    gfx::Mesh wolfMesh;

    // GPU-skinned demo mesh: a waving flag driven by two bones (see art.cpp).
    gfx::Mesh flagMesh;

    // Real wolf model + animation set (Blender glTF, see
    // projects/default/assets/models/wolf). The mesh is skinned; wolves are drawn with
    // DrawSkinnedMesh using per-wolf bone matrices from wolfAnim.
    assets::GltfAsset wolfGltf;
    anim::AnimSet wolfAnim;

    // Kenney Nature Kit (CC0) models, loaded through the asset pipeline.
    gfx::Mesh kenneyPine;
    gfx::Mesh kenneyOak;
    gfx::Mesh kenneyRock;
    gfx::Mesh kenneyLog;
};

// All visuals are generated procedurally - no binary art assets shipped.
void CreateDemoAssets(gfx::Renderer& renderer, assets::AssetManager& assetMgr, DemoAssets& out);

} // namespace neon::demo
