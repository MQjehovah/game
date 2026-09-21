#include "neon/gfx/particles.hpp"

#include <algorithm>

#include "neon/math/math.hpp"

namespace neon::gfx {

void ParticleSystem::Emit(const EmitterConfig& config) {
    // Reuse capacity across bursts instead of reallocating per Emit (P0-3
    // memory direction: stable arena for the hot particle path).
    if (particles_.empty()) particles_.reserve(2048);
    // Hard cap: bound the per-frame CPU update + instance upload so a runaway
    // emitter (or many overlapping ultimates) cannot collapse the frame. Evict
    // the oldest particles first (they are appended in time order).
    constexpr size_t kMaxParticles = 8192;
    if (particles_.size() + config.count > kMaxParticles) {
        const size_t over = particles_.size() + config.count - kMaxParticles;
        const size_t drop = std::min(over, particles_.size());
        particles_.erase(particles_.begin(), particles_.begin() + static_cast<long>(drop));
    }
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
    // Additive particles are order-independent (blend commutes), so they draw
    // unsorted. Alpha-blended particles (smoke/dust/poison) must draw back-to-
    // front or overlapping quads composite in the wrong order; sort by distance
    // to the camera (farthest first).
    if (alphaPos.size() > 1) {
        const math::Vec3 cam = renderer.CameraPosition();
        std::vector<uint32_t> order(alphaPos.size());
        for (uint32_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const math::Vec3 da = alphaPos[a] - cam;
            const math::Vec3 db = alphaPos[b] - cam;
            return da.LengthSq() > db.LengthSq();
        });
        std::vector<math::Vec3> sp(alphaPos.size());
        std::vector<float> ss(alphaSize.size());
        std::vector<Color> sc(alphaCol.size());
        for (size_t k = 0; k < order.size(); ++k) {
            sp[k] = alphaPos[order[k]];
            ss[k] = alphaSize[order[k]];
            sc[k] = alphaCol[order[k]];
        }
        alphaPos.swap(sp);
        alphaSize.swap(ss);
        alphaCol.swap(sc);
    }
    if (!addPos.empty())
        renderer.DrawBillboards(addPos.data(), addSize.data(), addCol.data(),
                                texture.Handle(), static_cast<uint32_t>(addPos.size()),
                                BlendMode::Additive, /*intensity=*/4.0f);
    if (!alphaPos.empty())
        renderer.DrawBillboards(alphaPos.data(), alphaSize.data(), alphaCol.data(),
                                texture.Handle(), static_cast<uint32_t>(alphaPos.size()),
                                BlendMode::Alpha);
}

void ParticleSystem::Clear() { particles_.clear(); }

} // namespace neon::gfx
