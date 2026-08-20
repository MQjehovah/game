#pragma once
#include <string>
#include "neon/gfx/backend.hpp"

namespace neon::gfx {

class Shader {
public:
    Shader() = default;
    Shader(ShaderHandle handle, std::string name) : handle_(handle), name_(std::move(name)) {}

    bool Valid() const { return handle_.Valid(); }
    ShaderHandle Handle() const { return handle_; }
    const std::string& Name() const { return name_; }

private:
    ShaderHandle handle_;
    std::string name_;
};

} // namespace neon::gfx
