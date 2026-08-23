#include "neon/gfx/font.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "neon/core/log.hpp"
#include "neon/gfx/renderer.hpp"

namespace neon::gfx {

namespace {

// Dynamic font atlas: one RGBA texture that starts with the statically baked
// glyphs and grows on demand (glyphs rasterized via stb_truetype are appended
// below the static region and uploaded with UpdateTextureRegion).
constexpr int kAtlasW = 2048;
constexpr int kAtlasH = 2048;
constexpr int kGlyphPad = 1;

} // namespace

struct Font::FontFace {
    std::vector<uint8_t> bytes;
    stbtt_fontinfo info{};
    IRenderBackend* backend = nullptr;
    int bakedSize = 24;
    float scale = 1.0f;
    std::vector<unsigned char> atlasPixels; // RGBA8, kAtlasW x kAtlasH
    int cursorX = 0;
    int cursorY = 0;
    int rowH = 0;
    bool full = false;
};

bool Font::EnsureGlyph(int32_t codepoint) {
    if (FindGlyph(codepoint) || !face_ || codepoint <= 0 || face_->full) return false;
    FontFace& f = *face_;
    const float scale = f.scale;
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp =
        stbtt_GetCodepointBitmap(&f.info, scale, scale, codepoint, &w, &h, &xoff, &yoff);
    if (!bmp || w <= 0 || h <= 0) {
        if (bmp) stbtt_FreeBitmap(bmp, nullptr);
        return false;
    }
    const int gw = w + kGlyphPad * 2;
    const int gh = h + kGlyphPad * 2;
    if (f.cursorX + gw > kAtlasW) {
        f.cursorX = 0;
        f.cursorY += f.rowH + kGlyphPad;
        f.rowH = 0;
    }
    if (f.cursorY + gh > kAtlasH) {
        stbtt_FreeBitmap(bmp, nullptr);
        f.full = true;
        NEON_LOG_WARN("Font: dynamic atlas full (%dx%d), further glyphs skipped",
                      kAtlasW, kAtlasH);
        return false;
    }
    const int px = f.cursorX;
    const int py = f.cursorY;
    for (int yy = 0; yy < h; ++yy) {
        const unsigned char* src = bmp + static_cast<size_t>(yy) * w;
        unsigned char* dst = &f.atlasPixels[static_cast<size_t>((py + kGlyphPad + yy) * kAtlasW +
                                                                (px + kGlyphPad)) *
                                            4];
        for (int xx = 0; xx < w; ++xx) {
            const unsigned char v = src[xx];
            dst[xx * 4 + 0] = v;
            dst[xx * 4 + 1] = v;
            dst[xx * 4 + 2] = v;
            dst[xx * 4 + 3] = v;
        }
    }
    stbtt_FreeBitmap(bmp, nullptr);

    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f.info, codepoint, &advance, &lsb);

    Glyph g;
    g.xoff = static_cast<float>(xoff - kGlyphPad);
    g.yoff = static_cast<float>(yoff - kGlyphPad);
    g.xoff2 = static_cast<float>(xoff - kGlyphPad + gw);
    g.yoff2 = static_cast<float>(yoff - kGlyphPad + gh);
    g.u0 = static_cast<float>(px) / kAtlasW;
    g.v0 = static_cast<float>(py) / kAtlasH;
    g.u1 = static_cast<float>(px + gw) / kAtlasW;
    g.v1 = static_cast<float>(py + gh) / kAtlasH;
    g.advance = static_cast<float>(advance) * scale;

    // Upload the new glyph's region (RGBA row by row).
    if (f.backend && atlas_.Valid()) {
        std::vector<unsigned char> upload(static_cast<size_t>(gw) * gh * 4);
        for (int yy = 0; yy < gh; ++yy) {
            const unsigned char* src =
                &f.atlasPixels[static_cast<size_t>((py + yy) * kAtlasW + px) * 4];
            std::memcpy(&upload[static_cast<size_t>(yy) * gw * 4], src,
                        static_cast<size_t>(gw) * 4);
        }
        f.backend->UpdateTextureRegion(atlas_, px, py, gw, gh, upload.data());
    }

    f.cursorX += gw + kGlyphPad;
    f.rowH = std::max(f.rowH, gh);
    extraGlyphs_[codepoint] = g;
    return true;
}

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
        if (!g) {
            // Dynamic glyphs: rasterize on demand so layout matches rendering.
            const_cast<Font*>(this)->EnsureGlyph(cp);
            g = FindGlyph(cp);
        }
        if (!g) continue;
        x += g->advance * scale;
        maxX = std::max(maxX, x);
    }
    return {maxX, y + LineHeight(size)};
}

float Font::CharAdvance(int32_t codepoint, float size) const {
    const Glyph* g = FindGlyph(codepoint);
    if (!g) {
        const_cast<Font*>(this)->EnsureGlyph(codepoint);
        g = FindGlyph(codepoint);
    }
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
    font.face_ = std::make_shared<Font::FontFace>();
    Font::FontFace& f = *font.face_;
    f.bytes.assign(data, data + size);
    f.backend = backend_.get();
    f.bakedSize = pixelHeight;
    if (!stbtt_InitFont(&f.info, f.bytes.data(), 0)) {
        // Try TTC collections (first face).
        const int offset = stbtt_GetFontOffsetForIndex(f.bytes.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&f.info, f.bytes.data(), offset)) {
            NEON_LOG_ERROR("Font: failed to parse font data");
            font.face_.reset();
            return font;
        }
    }
    f.scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(pixelHeight));

    // stbtt_PackFontRanges is a GRAYSCALE API: glyph pixels are consecutive
    // bytes at (y*stride + x). Feeding it the RGBA atlas directly makes it
    // write each glyph row into 1/4 the pixels (the R,G,B,A channels), so the
    // UV rects point at bytes that belong to other glyphs and text renders
    // garbled/invisible. Pack into a proper 1-byte staging buffer first, then
    // expand to RGBA for the GPU atlas and the dynamic-glyph upload path.
    f.atlasPixels.assign(static_cast<size_t>(kAtlasW) * kAtlasH * 4, 0);
    std::vector<unsigned char> gray(static_cast<size_t>(kAtlasW) * kAtlasH, 0);
    stbtt_pack_context packContext;
    if (!stbtt_PackBegin(&packContext, gray.data(), kAtlasW, kAtlasH, kAtlasW, 1, nullptr)) {
        NEON_LOG_ERROR("Font: stbtt_PackBegin failed");
        font.face_.reset();
        return font;
    }
    // 1x oversampling: the full GB2312 first-level set (~3800 glyphs) must fit
    // in the atlas, and dynamic glyphs (which stb_truetype rasterizes at 1x)
    // stay consistent with the baked ones. Matches ImGui's CJK atlas settings.
    stbtt_PackSetOversampling(&packContext, 1, 1);
    std::vector<stbtt_packedchar> asciiChars(95);
    std::vector<stbtt_packedchar> extraChars(static_cast<size_t>(std::max(codepointCount, 0)));
    std::vector<int> extraCps(codepoints, codepoints + std::max(codepointCount, 0));
    std::vector<stbtt_pack_range> ranges;
    ranges.push_back({static_cast<float>(pixelHeight), 32, nullptr, 95, asciiChars.data()});
    if (codepointCount > 0) {
        ranges.push_back({static_cast<float>(pixelHeight), 0, extraCps.data(), codepointCount,
                          extraChars.data()});
    }
    if (stbtt_PackFontRanges(&packContext, f.bytes.data(), 0, ranges.data(),
                             static_cast<int>(ranges.size())) == 0) {
        NEON_LOG_WARN("Font: stbtt_PackFontRanges reported failures for some glyphs");
    }
    stbtt_PackEnd(&packContext);

    // Expand the packed grayscale glyphs into RGBA (dynamic glyphs write RGBA
    // too), so both paths share the same atlas layout.
    size_t covered = 0;
    for (size_t yy = 0; yy < static_cast<size_t>(kAtlasH); ++yy) {
        for (size_t xx = 0; xx < static_cast<size_t>(kAtlasW); ++xx) {
            const unsigned char v = gray[yy * kAtlasW + xx];
            unsigned char* dst = &f.atlasPixels[(yy * kAtlasW + xx) * 4];
            dst[0] = v;
            dst[1] = v;
            dst[2] = v;
            dst[3] = v;
            if (v > 0) ++covered;
        }
    }

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &lineGap);

    font.atlasW_ = kAtlasW;
    font.atlasH_ = kAtlasH;
    font.bakedSize_ = pixelHeight;
    font.ascent_ = static_cast<float>(ascent) * f.scale;
    font.descent_ = static_cast<float>(descent) * f.scale;
    int maxPackedY = 0;
    int missingBaked = 0;
    for (int i = 0; i < 95; ++i) {
        const stbtt_packedchar& c = asciiChars[i];
        maxPackedY = std::max(maxPackedY, static_cast<int>(c.y1));
        font.glyphs_[i] = {c.xoff,
                           c.yoff,
                           c.xoff2,
                           c.yoff2,
                           c.x0 / static_cast<float>(kAtlasW),
                           c.y0 / static_cast<float>(kAtlasH),
                           c.x1 / static_cast<float>(kAtlasW),
                           c.y1 / static_cast<float>(kAtlasH),
                           c.xadvance};
    }
    for (int i = 0; i < codepointCount; ++i) {
        const stbtt_packedchar& pc = extraChars[i];
        if (pc.x0 == 0 && pc.x1 == 0 && pc.y0 == 0 && pc.y1 == 0) {
            ++missingBaked;
            continue;
        }
        maxPackedY = std::max(maxPackedY, static_cast<int>(pc.y1));
        font.extraGlyphs_[codepoints[i]] = {
            pc.xoff, pc.yoff, pc.xoff2, pc.yoff2,
            pc.x0 / static_cast<float>(kAtlasW), pc.y0 / static_cast<float>(kAtlasH),
            pc.x1 / static_cast<float>(kAtlasW), pc.y1 / static_cast<float>(kAtlasH),
            pc.xadvance};
    }

    // Dynamic glyphs append BELOW the statically packed rows (never at a fixed
    // offset that would overwrite baked glyphs).
    f.cursorX = 0;
    f.cursorY = maxPackedY + kGlyphPad;
    f.rowH = 0;

    TextureDesc desc;
    desc.width = kAtlasW;
    desc.height = kAtlasH;
    desc.rgba = f.atlasPixels.data();
    desc.filter = Filter::Linear;
    font.atlas_ = backend_->CreateTexture(desc);
    NEON_LOG_INFO("Font: baked %dpx atlas %dx%d covered=%zu tex=%u packY=%d missing=%d",
                  pixelHeight, kAtlasW, kAtlasH, covered, font.atlas_.id, maxPackedY,
                  missingBaked);
    return font;
}

} // namespace neon::gfx
