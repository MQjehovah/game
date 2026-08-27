#pragma once

#include <cstdint>
#include <vector>

#include "neon/assets/async_loader.hpp"

namespace neon::assets {

// Pure decode of already-read image bytes (stb_image native channel count +
// optional BC1). Shared by the runtime AssetManager (sync + async load) and the
// offline AssetImporter (G5-4-3 pre-bake), so a baked texture is byte-identical
// to what the runtime would have compressed itself. Loading in the image's
// NATIVE channel count distinguishes opaque images (gray / RGB) from
// alpha-bearing ones (gray+alpha / RGBA): BC1 has no alpha, so only opaque
// images are compressed; alpha images stay RGBA8.
DecodedImage DecodeImageBytes(const std::vector<uint8_t>& bytes, bool compressBc1,
                              bool flipVertically);

} // namespace neon::assets
