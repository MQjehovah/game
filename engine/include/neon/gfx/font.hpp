#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include "neon/gfx/backend.hpp"
#include "neon/math/vec2.hpp"

namespace neon::gfx {

// Decodes one UTF-8 codepoint, advancing `it`; returns 0 at end/invalid.
inline int32_t DecodeUTF8Next(const char*& it, const char* end) {
    if (it >= end) return 0;
    uint8_t b = static_cast<uint8_t>(*it);
    int32_t cp = 0;
    int len = 0;
    if (b < 0x80) {
        cp = b;
        len = 1;
    } else if ((b & 0xE0) == 0xC0) {
        cp = b & 0x1F;
        len = 2;
    } else if ((b & 0xF0) == 0xE0) {
        cp = b & 0x0F;
        len = 3;
    } else if ((b & 0xF8) == 0xF0) {
        cp = b & 0x07;
        len = 4;
    } else {
        ++it;
        return 0;
    }
    if (it + len > end) {
        it = end;
        return 0;
    }
    for (int k = 1; k < len; ++k) {
        cp = (cp << 6) | (static_cast<uint8_t>(it[k]) & 0x3F);
    }
    it += len;
    return cp;
}

class Font {
public:
    Font() = default;

    bool Valid() const { return atlas_.Valid(); }
    TextureHandle Atlas() const { return atlas_; }

    // Baked metrics
    int AtlasWidth() const { return atlasW_; }
    int AtlasHeight() const { return atlasH_; }
    float Ascent() const { return ascent_; }
    float Descent() const { return descent_; }
    float LineHeight(float size) const { return (ascent_ - descent_) * (size / static_cast<float>(bakedSize_)); }
    int BakedSize() const { return bakedSize_; }

    // layout helper: advance x by glyph width + tracking
    math::Vec2 Measure(const std::string& text, float size) const;
    float CharAdvance(int32_t codepoint, float size) const;

private:
    friend class Renderer;

    TextureHandle atlas_;
    int atlasW_ = 0;
    int atlasH_ = 0;
    int bakedSize_ = 24;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    struct Glyph {
        float xoff, yoff, xoff2, yoff2; // pixel quad relative to cursor
        float u0, v0, u1, v1; // atlas uv
        float advance;
    } glyphs_[95]; // ASCII 32..126
    std::unordered_map<int32_t, Glyph> extraGlyphs_;

    const Glyph* FindGlyph(int32_t codepoint) const {
        if (codepoint >= 32 && codepoint <= 126) return &glyphs_[codepoint - 32];
        auto it = extraGlyphs_.find(codepoint);
        return it != extraGlyphs_.end() ? &it->second : nullptr;
    }
};

} // namespace neon::gfx
