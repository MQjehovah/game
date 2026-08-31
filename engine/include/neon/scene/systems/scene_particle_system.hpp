#pragma once
#include <string>

#include "neon/gfx/particles.hpp"
#include "neon/gfx/texture.hpp"

namespace neon::assets {
class AssetManager;
}

namespace neon::scene {

// Scene-level particle subsystem: a thin wrapper over gfx::ParticleSystem
// (world-space camera-facing billboards) plus its soft radial glow sprite.
// The texture is created lazily via InitParticleTexture (the project ships one
// at assets/sprites/glow.png; a missing file degrades to a white quad).
class SceneParticleSystem {
public:
    void Emit(const gfx::EmitterConfig& cfg) { particles_.Emit(cfg); }
    void Update(float dt) { particles_.Update(dt); }
    // Loads the soft glow sprite through the runtime's AssetManager. `path`
    // must be the fully-resolved texture path (assetBaseDir prefix applied by
    // the caller). No-op when already initialized.
    void InitParticleTexture(assets::AssetManager& assets, const std::string& path);
    // Draws live particles only when both the texture and particles exist
    // (inside the HDR target so bright particles bloom).
    void Draw(gfx::Renderer& renderer) {
        if (particleTex_.Valid() && particles_.Count() > 0)
            particles_.Draw(renderer, particleTex_);
    }
    // Drops live particles and the texture (Stop lifecycle).
    void Reset() {
        particles_.Clear();
        particleTex_ = {};
    }
    gfx::ParticleSystem& Particles() { return particles_; }

private:
    gfx::ParticleSystem particles_; // world-space billboard particles (scripts)
    gfx::Texture particleTex_;      // soft radial glow sprite for the particles
};

} // namespace neon::scene
