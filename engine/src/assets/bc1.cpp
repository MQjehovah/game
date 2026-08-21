#include "neon/assets/bc1.hpp"

#include <cstring>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace neon::assets {

int Bc1BlockWidth(int width) { return (width + 3) / 4; }
int Bc1BlockHeight(int height) { return (height + 3) / 4; }

size_t Bc1ByteSize(int width, int height) {
    return static_cast<size_t>(Bc1BlockWidth(width)) *
           static_cast<size_t>(Bc1BlockHeight(height)) * 8;
}

bool Bc1EncodeOpaque(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& out) {
    if (!rgba || width <= 0 || height <= 0) return false;
    const int bw = Bc1BlockWidth(width);
    const int bh = Bc1BlockHeight(height);
    out.resize(Bc1ByteSize(width, height));

    uint8_t block[64];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    int sx = bx * 4 + px;
                    int sy = by * 4 + py;
                    if (sx >= width) sx = width - 1;
                    if (sy >= height) sy = height - 1;
                    const uint8_t* src = rgba + (static_cast<size_t>(sy) * width + sx) * 4;
                    uint8_t* dst = block + (py * 4 + px) * 4;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            }
            // alpha=0 -> opaque BC1 (4-color mode, no 1-bit alpha); HIGHQUAL
            // runs the extra refinement pass for better quality on the 1/8
            // memory budget. Deterministic for identical input.
            stb_compress_dxt_block(out.data() + (static_cast<size_t>(by) * bw + bx) * 8, block,
                                   0, STB_DXT_HIGHQUAL);
        }
    }
    return true;
}

} // namespace neon::assets
