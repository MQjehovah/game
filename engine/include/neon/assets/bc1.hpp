#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace neon::assets {

// BC1/DXT1 (S3TC) runtime texture compression, backed by the vendored
// stb_dxt.h (third_party/stb/stb_dxt.h). BC1 encodes 4x4 pixel blocks into
// 8 bytes: 1/8 the GPU memory of an RGBA8 texture (no mip chain is generated;
// the min filter stays linear). BC1 has NO alpha channel, so only opaque
// images are eligible; alpha-bearing images must stay RGBA8.
constexpr uint32_t kBc1Format = 0x83F1; // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT

// Number of 4x4 blocks after padding to the block grid.
int Bc1BlockWidth(int width);
int Bc1BlockHeight(int height);
// Byte size of the BC1 data for the padded block grid (blocks * 8 bytes).
size_t Bc1ByteSize(int width, int height);

// Compresses an RGBA8 image (alpha channel ignored) to BC1. Blocks that hang
// past the right/bottom edge when width/height are not multiples of 4 are
// padded by clamping the edge pixels (standard practice). Appends the
// compressed bytes to `out`. Returns true on success.
bool Bc1EncodeOpaque(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& out);

} // namespace neon::assets
