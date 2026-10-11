#include "neon/scene/systems/scene_particle_system.hpp"

#include "neon/assets/asset_manager.hpp"

namespace neon::scene {

void SceneParticleSystem::InitParticleTexture(assets::AssetManager& assets,
                                              const std::string& path) {
    if (!textures_.empty()) return;
    assets_ = &assets;
    // Slot 0 is always the default glow sprite (Emit's fallback).
    textures_.push_back(assets.LoadTexture(path));
    texLookup_[path] = 0;
}

uint32_t SceneParticleSystem::ResolveTexture(const std::string& path) {
    if (path.empty()) return 0;
    auto it = texLookup_.find(path);
    if (it != texLookup_.end()) return it->second;
    if (!assets_) return 0;
    gfx::Texture tex = assets_->LoadTexture(path);
    // A failed load yields an invalid texture: fall back to the default glow
    // rather than poisoning every particle with a null sprite.
    if (!tex.Valid()) {
        texLookup_[path] = 0;
        return 0;
    }
    const uint32_t index = static_cast<uint32_t>(textures_.size());
    textures_.push_back(tex);
    texLookup_[path] = index;
    return index;
}

void SceneParticleSystem::Emit(const gfx::EmitterConfig& cfg) {
    gfx::EmitterConfig resolved = cfg;
    resolved.textureIndex = ResolveTexture(cfg.texture);
    particles_.Emit(resolved);
}

} // namespace neon::scene
