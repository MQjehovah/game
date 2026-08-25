#include "neon/gfx/particles.hpp"

#include <algorithm>

#include "neon/math/math.hpp"

namespace neon::gfx {

void ParticleSystem::Emit(const EmitterConfig& config) {
    // Reuse capacity across bursts instead of reallocating per Emit (P0-3
    // memory direction: stable arena for the hot particle path).
    if (particles_.empty()) particles_.reserve(2048);
    for (uint32_t i = 0; i < config.count; ++i) {
        Particle p;
        p.pos = config.position;
        p.vel = config.baseVelocity + rng_.OnUnitSphere() *
                                          rng_.Range(config.speedMin, config.speedMax);
        p.life = p.maxLife = rng_.Range(config.lifeMin, config.lifeMax);
        p.size = config.sizeStart;
        p.sizeEnd = config.sizeEnd;
        p.color = config.colorStart;
        p.colorEnd = config.colorEnd;
        p.gravity = config.gravity;
        p.additive = config.additive;
        particles_.push_back(p);
    }
}

void ParticleSystem::Update(float dt) {
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.life -= dt;
        p.vel.y += p.gravity * dt;
        p.pos += p.vel * dt;
        if (p.life <= 0.0f) {
            // Swap-with-last removal keeps the buffer compact without the
            // erase/remove_if reallocation churn.
            particles_[i] = particles_.back();
            particles_.pop_back();
            continue;
        }
        ++i;
    }
}

void ParticleSystem::Draw(Renderer& renderer, const Texture& texture, float scale) {
    // G1-5: batch every particle into ONE instanced 3D billboard draw per blend
    // mode (additive vs alpha) instead of one screen-space call per particle.
    // The billboards are world-sized, camera-facing and depth-tested, so they
    // occlude/are occluded by the scene (the old path was 2D overlay, unaware
    // of scene depth).
    std::vector<math::Vec3> addPos, alphaPos;
    std::vector<float> addSize, alphaSize;
    std::vector<Color> addCol, alphaCol;
    addPos.reserve(particles_.size());
    alphaPos.reserve(particles_.size());
    for (const Particle& p : particles_) {
        const float t01 = 1.0f - p.life / p.maxLife;
        const float size = math::Lerp(p.size, p.sizeEnd, t01) * scale;
        Color c{math::Lerp(p.color.r, p.colorEnd.r, t01),
                math::Lerp(p.color.g, p.colorEnd.g, t01),
                math::Lerp(p.color.b, p.colorEnd.b, t01),
                math::Lerp(p.color.a, p.colorEnd.a, t01)};
        if (p.additive) {
            addPos.push_back(p.pos);
            addSize.push_back(size);
            addCol.push_back(c);
        } else {
            alphaPos.push_back(p.pos);
            alphaSize.push_back(size);
            alphaCol.push_back(c);
        }
    }
    if (!addPos.empty())
        renderer.DrawBillboards(addPos.data(), addSize.data(), addCol.data(),
                                texture.Handle(), static_cast<uint32_t>(addPos.size()),
                                BlendMode::Additive);
    if (!alphaPos.empty())
        renderer.DrawBillboards(alphaPos.data(), alphaSize.data(), alphaCol.data(),
                                texture.Handle(), static_cast<uint32_t>(alphaPos.size()),
                                BlendMode::Alpha);
}

void ParticleSystem::Clear() { particles_.clear(); }

} // namespace neon::gfx
