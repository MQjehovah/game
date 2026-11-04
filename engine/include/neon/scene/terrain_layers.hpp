#pragma once

#include <functional>
#include <string>

#include "neon/gfx/backend.hpp"

namespace neon::gfx {
class Renderer;
}

namespace neon::scene {

// The three 2D texture arrays shared by every procedural terrain chunk.
struct TerrainLayerSet {
    gfx::TextureHandle albedo;
    gfx::TextureHandle normal;
    gfx::TextureHandle orm;
    int layers = 0;
    float uvScale[4] = {0.25f, 0.25f, 0.25f, 0.25f};

    bool Valid() const { return albedo.Valid() && layers > 0; }
};

// Loads assets/data/terrain_layers.json, decodes the 2K JPG maps and uploads
// them as three texture arrays. `resolvePath` maps a manifest-relative path
// (e.g. "grass/albedo.jpg") to a filesystem path under the project.
TerrainLayerSet LoadTerrainLayers(
    gfx::Renderer& renderer, const std::string& manifestJson,
    const std::function<std::string(const std::string&)>& resolvePath);

} // namespace neon::scene
