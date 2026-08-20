#pragma once
#include "neon/gfx/backend.hpp"

namespace neon::gfx {

class Texture {
public:
    Texture() = default;
    Texture(TextureHandle handle, int width, int height)
        : handle_(handle), width_(width), height_(height) {}

    bool Valid() const { return handle_.Valid(); }
    TextureHandle Handle() const { return handle_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    TextureHandle handle_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace neon::gfx
