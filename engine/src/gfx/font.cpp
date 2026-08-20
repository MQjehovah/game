#include "neon/gfx/font.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "neon/core/log.hpp"
#include "neon/gfx/renderer.hpp"

namespace neon::gfx {

math::Vec2 Font::Measure(const std::string& text, float size) const {
    if (!Valid()) return {};
    float scale = size / static_cast<float>(bakedSize_);
    float x = 0.0f, maxX = 0.0f, y = 0.0f;
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
        int32_t cp = DecodeUTF8Next(it, end);
        if (cp == 0) continue;
        if (cp == '\n') {
            x = 0.0f;
            y += LineHeight(size);
            continue;
        }
        const Glyph* g = FindGlyph(cp);
        if (!g) continue;
        x += g->advance * scale;
        maxX = std::max(maxX, x);
    }
    return {maxX, y + LineHeight(size)};
}

float Font::CharAdvance(int32_t codepoint, float size) const {
    const Glyph* g = FindGlyph(codepoint);
    return g ? g->advance * size / static_cast<float>(bakedSize_) : 0.0f;
}

} // namespace neon::gfx

namespace neon::gfx {

Font Renderer::CreateFontFromMemory(const uint8_t* data, size_t size, int pixelHeight) {
    return CreateFontFromMemoryWithCodepoints(data, size, pixelHeight, nullptr, 0);
}

Font Renderer::CreateFontFromMemoryWithCodepoints(const uint8_t* data, size_t size,
                                                  int pixelHeight, const int32_t* codepoints,
                                                  int codepointCount) {
    Font font;
    if (!data || size == 0) {
        NEON_LOG_ERROR("Font: empty font data");
        return font;
    }
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, 0)) {
        // Try TTC collections (first face).
        int offset = stbtt_GetFontOffsetForIndex(data, 0);
        if (offset < 0 || !stbtt_InitFont(&info, data, offset)) {
            NEON_LOG_ERROR("Font: failed to parse font data");
            return font;
        }
    }

    const int atlasW = codepointCount > 0 ? 2048 : 1024;
    const int atlasH = codepointCount > 0 ? 1024 : 512;
    // stb packs into a 1-channel buffer; oversampling prefilters assume
    // stride == width, so we expand to RGBA afterwards.
    std::vector<unsigned char> gray(static_cast<size_t>(atlasW) * atlasH, 0);
    stbtt_pack_context packContext;
    if (!stbtt_PackBegin(&packContext, gray.data(), atlasW, atlasH, atlasW, 1, nullptr)) {
        NEON_LOG_ERROR("Font: stbtt_PackBegin failed");
        return font;
    }
    stbtt_PackSetOversampling(&packContext, 2, 2);
    // Pack ASCII + all extra codepoints in a single stbtt_PackFontRanges call.
    std::vector<stbtt_packedchar> asciiChars(95);
    std::vector<stbtt_packedchar> extraChars(static_cast<size_t>(std::max(codepointCount, 0)));
    std::vector<int> extraCps(codepoints, codepoints + std::max(codepointCount, 0));
    std::vector<stbtt_pack_range> ranges;
    ranges.push_back({static_cast<float>(pixelHeight), 32, nullptr, 95, asciiChars.data()});
    if (codepointCount > 0) {
        ranges.push_back({static_cast<float>(pixelHeight), 0, extraCps.data(), codepointCount,
                          extraChars.data()});
    }
    int packResult = stbtt_PackFontRanges(&packContext, data, 0, ranges.data(),
                                          static_cast<int>(ranges.size()));
    if (packResult == 0) {
        NEON_LOG_WARN("Font: stbtt_PackFontRanges reported failures for some glyphs");
    }
    stbtt_PackEnd(&packContext);

    // Expand grayscale coverage into RGBA (alpha = coverage).
    std::vector<unsigned char> atlas(static_cast<size_t>(atlasW) * atlasH * 4, 0);
    size_t covered = 0;
    for (size_t i = 0; i < gray.size(); ++i) {
        unsigned char v = gray[i];
        atlas[i * 4] = v;
        atlas[i * 4 + 1] = v;
        atlas[i * 4 + 2] = v;
        atlas[i * 4 + 3] = v;
        if (v > 0) ++covered;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(pixelHeight));
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);

    font.atlasW_ = atlasW;
    font.atlasH_ = atlasH;
    font.bakedSize_ = pixelHeight;
    font.ascent_ = static_cast<float>(ascent) * scale;
    font.descent_ = static_cast<float>(descent) * scale;
    for (int i = 0; i < 95; ++i) {
        const stbtt_packedchar& c = asciiChars[i];
        font.glyphs_[i] = {c.xoff,
                           c.yoff,
                           c.xoff2,
                           c.yoff2,
                           c.x0 / static_cast<float>(atlasW),
                           c.y0 / static_cast<float>(atlasH),
                           c.x1 / static_cast<float>(atlasW),
                           c.y1 / static_cast<float>(atlasH),
                           c.xadvance};
    }
    for (int i = 0; i < codepointCount; ++i) {
        const stbtt_packedchar& pc = extraChars[i];
        if (pc.x0 == 0 && pc.x1 == 0 && pc.y0 == 0 && pc.y1 == 0) continue;
        font.extraGlyphs_[codepoints[i]] = {
            pc.xoff, pc.yoff, pc.xoff2, pc.yoff2,
            pc.x0 / static_cast<float>(atlasW), pc.y0 / static_cast<float>(atlasH),
            pc.x1 / static_cast<float>(atlasW), pc.y1 / static_cast<float>(atlasH),
            pc.xadvance};
    }

    TextureDesc desc;
    desc.width = atlasW;
    desc.height = atlasH;
    desc.rgba = atlas.data();
    desc.filter = Filter::Linear;
    font.atlas_ = backend_->CreateTexture(desc);
    NEON_LOG_INFO("Font: baked %dpx atlas %dx%d covered=%zu tex=%u", pixelHeight, atlasW, atlasH,
                  covered, font.atlas_.id);
    return font;
}

} // namespace neon::gfx
