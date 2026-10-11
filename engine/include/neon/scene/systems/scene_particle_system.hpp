#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "neon/gfx/particles.hpp"
#include "neon/gfx/texture.hpp"

namespace neon::assets {
class AssetManager;
}

namespace neon::scene {

// Scene-level particle subsystem: a thin wrapper over gfx::ParticleSystem
// (world-space camera-facing billboards) plus a per-emitter sprite cache. Every
// emitter may name its own sprite via EmitterConfig::texture (e.g. a spark,
// smoke, slash or flipbook sheet); emitters sharing a path batch into one draw.
// The default sprite is the project's assets/sprites/glow.png (a missing file
// degrades to a white quad).
class SceneParticleSystem {
public:
    void Emit(const gfx::EmitterConfig& cfg);
    void Update(float dt) { particles_.Update(dt); }
    // Loads the default soft glow sprite through the runtime's AssetManager.
    // `path` must be the fully-resolved texture path (assetBaseDir prefix
    // applied by the caller). No-op when already initialized.
    void InitParticleTexture(assets::AssetManager& assets, const std::string& path);
    // Draws live particles only when textures and particles exist (inside the
    // HDR target so bright particles bloom).
    void Draw(gfx::Renderer& renderer) {
        if (!textures_.empty() && particles_.Count() > 0) particles_.Draw(renderer, textures_);
    }
    // Drops live particles and the sprite cache (Stop lifecycle).
    void Reset() {
        particles_.Clear();
        textures_.clear();
        texLookup_.clear();
    }
    gfx::ParticleSystem& Particles() { return particles_; }

private:
    // Resolves an EmitterConfig::texture path to a slot in textures_ (loading
    // and appending on first use). Returns 0 (the default glow) on failure.
    uint32_t ResolveTexture(const std::string& path);

    gfx::ParticleSystem particles_; // world-space billboard particles (scripts)
    assets::AssetManager* assets_ = nullptr;
    std::vector<gfx::Texture> textures_;              // [0] = default glow sprite
    std::unordered_map<std::string, uint32_t> texLookup_;
};

} // namespace neon::scene
