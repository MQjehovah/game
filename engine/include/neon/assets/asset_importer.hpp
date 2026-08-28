#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neon::assets {

// G5-4-3 offline asset importer / baker. Pre-bakes BC1-compressed textures for
// a project so the runtime uploads them without a decode+compress hitch at
// load (the async path still decodes; the bake makes it a cache hit).
//
// ImportProjectTextures scans <project>/assets for image files, decodes each
// with the SAME DecodeImageBytes the runtime uses (so the bake is byte-identical
// to runtime compression), and writes opaque ones to
// <project>/.neon/imported/<project-relative-path>.nbc1 (magic "NBC1", u32 width,
// u32 height, then BC1 blocks). Alpha-bearing images are skipped — they stay
// RGBA8 at runtime. The runtime prefers the bake via
// AssetManager::SetTextureBakeDir.

struct ImportReport {
    size_t bakedCount = 0;    // opaque images baked to BC1
    size_t skippedCount = 0;  // alpha-bearing / unsupported, stay source
    std::vector<std::string> errors;
};

// Bakes <project>/assets images into <project>/.neon/imported. Idempotent.
ImportReport ImportProjectTextures(const std::string& projectDir);

// .nbc1 cache I/O (magic "NBC1" + u32 w + u32 h + BC1 blocks).
bool ReadBakedTexture(const std::string& path, int& width, int& height,
                      std::vector<uint8_t>& bc1);
bool WriteBakedTexture(const std::string& path, int width, int height,
                       const std::vector<uint8_t>& bc1);

} // namespace neon::assets
