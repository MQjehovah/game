#pragma once
#include "neon/neon.hpp"

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

    // Kenney Nature Kit (CC0) models, loaded through the asset pipeline.
    gfx::Mesh kenneyPine;
    gfx::Mesh kenneyOak;
    gfx::Mesh kenneyRock;
    gfx::Mesh kenneyLog;
};

// All visuals are generated procedurally - no binary art assets shipped.
void CreateDemoAssets(gfx::Renderer& renderer, assets::AssetManager& assetMgr, DemoAssets& out);

} // namespace neon::demo
