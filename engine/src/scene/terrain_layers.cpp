#include "neon/scene/terrain_layers.hpp"

#include <vector>

#include "neon/assets/image_decode.hpp"
#include "neon/core/json.hpp"
#include "neon/core/log.hpp"
#include "neon/gfx/renderer.hpp"

namespace neon::scene {
namespace {

bool AppendMap(const core::Json& layer, const char* key,
               const std::function<std::string(const std::string&)>& resolve,
               std::vector<uint8_t>& out, int& width, int& height) {
    const core::Json* path = layer.Get(key);
    if (path == nullptr || !path->IsString()) return false;
    const std::string full = resolve(path->GetString());
    assets::DecodedImage img = assets::DecodeImageFile(full, false);
    if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) {
        NEON_LOG_ERROR("terrain layers: failed to decode '%s'", full.c_str());
        return false;
    }
    if (width == 0) {
        width = img.width;
        height = img.height;
    } else if (img.width != width || img.height != height) {
        NEON_LOG_ERROR("terrain layers: '%s' is %dx%d, expected %dx%d", full.c_str(),
                       img.width, img.height, width, height);
        return false;
    }
    out.insert(out.end(), img.rgba.begin(), img.rgba.end());
    return true;
}

} // namespace

TerrainLayerSet LoadTerrainLayers(
    gfx::Renderer& renderer, const std::string& manifestJson,
    const std::function<std::string(const std::string&)>& resolvePath) {
    TerrainLayerSet set;
    std::string err;
    const core::Json root = core::Json::Parse(manifestJson, &err);
    const core::Json* layers = root.Get("layers");
    if (layers == nullptr || !layers->IsArray() || layers->Size() == 0) {
        NEON_LOG_WARN("terrain layers: invalid manifest ('%s')", err.c_str());
        return set;
    }

    const int count = static_cast<int>(layers->Size());
    std::vector<uint8_t> albedo;
    std::vector<uint8_t> normal;
    std::vector<uint8_t> orm;
    int width = 0, height = 0;

    for (const core::Json& layer : layers->Items()) {
        if (!AppendMap(layer, "albedo", resolvePath, albedo, width, height) ||
            !AppendMap(layer, "normal", resolvePath, normal, width, height) ||
            !AppendMap(layer, "orm", resolvePath, orm, width, height)) {
            return set;
        }
    }

    gfx::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.layers = count;
    desc.mipmaps = true;
    desc.filter = gfx::Filter::Linear;
    desc.wrap = gfx::Wrap::Repeat;

    desc.rgba = albedo.data();
    set.albedo = renderer.CreateTexture(desc).Handle();
    desc.rgba = normal.data();
    set.normal = renderer.CreateTexture(desc).Handle();
    desc.rgba = orm.data();
    set.orm = renderer.CreateTexture(desc).Handle();
    set.layers = count;

    for (int i = 0; i < 4 && i < count; ++i) {
        const core::Json* item = layers->At(static_cast<size_t>(i));
        if (item == nullptr) continue;
        if (const core::Json* s = item->Get("uvScale"))
            set.uvScale[i] = static_cast<float>(s->GetNumber(0.25));
    }

    NEON_LOG_CAT(core::LogCategory::Gfx, core::LogLevel::Info,
                 "terrain layers: loaded %d at %dx%d", count, width, height);
    return set;
}

} // namespace neon::scene
