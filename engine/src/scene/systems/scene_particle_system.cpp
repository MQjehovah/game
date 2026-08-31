#include "neon/scene/systems/scene_particle_system.hpp"

#include "neon/assets/asset_manager.hpp"

namespace neon::scene {

void SceneParticleSystem::InitParticleTexture(assets::AssetManager& assets,
                                              const std::string& path) {
    if (particleTex_.Valid()) return;
    particleTex_ = assets.LoadTexture(path);
}

} // namespace neon::scene
