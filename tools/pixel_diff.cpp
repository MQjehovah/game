// Render regression helper: compares a committed golden PNG against a fresh
// --screenshot capture and reports how many pixels differ beyond a per-channel
// tolerance. Exits non-zero when the differing ratio exceeds the threshold so CI
// (or a local script) can gate on it. Decoding goes through the engine's own
// image decoder so the tool sees exactly what a texture load would see.
//
//   neon_pixel_diff <golden.png> <actual.png> [tol=8] [maxRatio=0.01] [diffOut.png]
//
// tol is a 0..255 per-channel delta; maxRatio is the allowed fraction of
// differing pixels (0.01 = 1%). diffOut (optional) writes a heat-map: differing
// pixels in red over a dimmed copy of the actual frame.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "neon/assets/async_loader.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: neon_pixel_diff <golden.png> <actual.png> "
                     "[tol=8] [maxRatio=0.01] [diffOut.png]\n");
        return 2;
    }
    const std::string goldenPath = argv[1];
    const std::string actualPath = argv[2];
    const int tol = argc > 3 ? std::atoi(argv[3]) : 8;
    const double maxRatio = argc > 4 ? std::atof(argv[4]) : 0.01;
    const std::string diffOut = argc > 5 ? argv[5] : std::string();

    const neon::assets::DecodedImage g =
        neon::assets::DecodeImageFile(goldenPath, /*compressBc1=*/false, /*flipVertically=*/false);
    const neon::assets::DecodedImage a =
        neon::assets::DecodeImageFile(actualPath, /*compressBc1=*/false, /*flipVertically=*/false);
    if (g.channels <= 0 || a.channels <= 0) {
        std::fprintf(stderr, "pixel_diff: failed to decode '%s' or '%s'\n", goldenPath.c_str(),
                     actualPath.c_str());
        return 2;
    }
    if (g.width != a.width || g.height != a.height) {
        std::fprintf(stderr, "pixel_diff: size mismatch %dx%d vs %dx%d\n", g.width, g.height,
                     a.width, a.height);
        return 3;
    }

    const size_t pixels = static_cast<size_t>(g.width) * static_cast<size_t>(g.height);
    size_t bad = 0;
    unsigned maxDelta = 0;
    std::vector<uint8_t> diff;
    if (!diffOut.empty()) diff.assign(pixels * 4, 0);

    for (size_t i = 0; i < pixels; ++i) {
        bool differs = false;
        for (int c = 0; c < 4; ++c) {
            const int d = std::abs(static_cast<int>(g.rgba[i * 4 + c]) -
                                   static_cast<int>(a.rgba[i * 4 + c]));
            if (static_cast<unsigned>(d) > maxDelta) maxDelta = static_cast<unsigned>(d);
            if (d > tol) differs = true;
        }
        if (differs) ++bad;
        if (!diffOut.empty()) {
            // Differing pixels glow red over a quarter-brightness actual frame.
            diff[i * 4 + 0] = differs ? 255 : static_cast<uint8_t>(a.rgba[i * 4 + 0] / 4);
            diff[i * 4 + 1] = differs ? 0 : static_cast<uint8_t>(a.rgba[i * 4 + 1] / 4);
            diff[i * 4 + 2] = differs ? 0 : static_cast<uint8_t>(a.rgba[i * 4 + 2] / 4);
            diff[i * 4 + 3] = 255;
        }
    }

    const double ratio = pixels ? static_cast<double>(bad) / static_cast<double>(pixels) : 0.0;
    std::printf(
        "pixel_diff: %zu/%zu px differ (%.4f%%), max delta %u, tol %d, max ratio %.4f%% -> %s\n",
        bad, pixels, ratio * 100.0, maxDelta, tol, maxRatio * 100.0,
        ratio > maxRatio ? "FAIL" : "PASS");
    if (!diffOut.empty())
        stbi_write_png(diffOut.c_str(), g.width, g.height, 4, diff.data(), g.width * 4);
    return ratio > maxRatio ? 1 : 0;
}
