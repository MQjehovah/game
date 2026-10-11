#include "neon/gfx/particles.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "neon/math/math.hpp"

namespace neon::gfx {

void ParticleSystem::Emit(const EmitterConfig& config) {
    // Reuse capacity across bursts instead of reallocating per Emit (P0-3
    // memory direction: stable arena for the hot particle path).
    if (particles_.empty()) particles_.reserve(2048);
    // Budget: bound the per-frame CPU update + instance upload so a runaway
    // emitter (or many overlapping ultimates) cannot collapse the frame. The
    // *incoming* burst is clamped rather than evicting live particles, so an
    // effect that is already playing never visibly pops out of existence.
    uint32_t spawn = config.count;
    if (particles_.size() + spawn > budget_) {
        const size_t room = particles_.size() < budget_ ? budget_ - particles_.size() : 0;
        spawn = static_cast<uint32_t>(room);
    }
    dropped_ += config.count - spawn;
    // Spawn volume + velocity distribution, resolved per emitter.
    const float spreadCos = std::cos(config.spreadDeg * 3.14159265358979f / 180.0f);
    math::Vec3 axis = config.direction.LengthSq() > 1e-8f ? config.direction.Normalized()
                                                          : math::Vec3{0.0f, 1.0f, 0.0f};
    // Any two vectors perpendicular to the axis, for cone/ring placement.
    math::Vec3 tangentA = math::Cross(axis, {0.0f, 1.0f, 0.0f});
    if (tangentA.LengthSq() < 1e-6f) tangentA = math::Cross(axis, {1.0f, 0.0f, 0.0f});
    tangentA = tangentA.Normalized();
    const math::Vec3 tangentB = math::Cross(axis, tangentA).Normalized();
    for (uint32_t i = 0; i < spawn; ++i) {
        Particle p;
        // --- position --------------------------------------------------------
        switch (config.shape) {
            case EmitterShape::Point:
                p.pos = config.position;
                break;
            case EmitterShape::Sphere:
            case EmitterShape::Hemisphere: {
                math::Vec3 d = rng_.OnUnitSphere();
                if (config.shape == EmitterShape::Hemisphere) d.y = std::fabs(d.y);
                p.pos = config.position + d * (config.shapeRadius * std::cbrt(rng_.Float()));
                break;
            }
            case EmitterShape::Cone: {
                const float ang = config.coneAngleDeg * 3.14159265358979f / 180.0f;
                const float t = std::sqrt(rng_.Float());
                const float phi = rng_.Float() * 6.28318530717959f;
                const float r = std::tan(ang) * t;
                p.pos = config.position +
                        (axis + (tangentA * std::cos(phi) + tangentB * std::sin(phi)) * r) *
                            (config.ringThickness * rng_.Float());
                break;
            }
            case EmitterShape::Ring:
            case EmitterShape::Disc: {
                const float phi = rng_.Float() * 6.28318530717959f;
                const float r =
                    config.shape == EmitterShape::Ring
                        ? config.shapeRadius + rng_.Range(-config.ringThickness,
                                                          config.ringThickness)
                        : config.shapeRadius * std::sqrt(rng_.Float());
                p.pos = config.position +
                        (tangentA * std::cos(phi) + tangentB * std::sin(phi)) * r;
                break;
            }
        }
        // --- velocity --------------------------------------------------------
        const float speed = rng_.Range(config.speedMin, config.speedMax);
        if (config.spreadDeg > 0.0f) {
            // Random direction inside the cone around `direction`.
            math::Vec3 d;
            do {
                d = rng_.OnUnitSphere();
            } while (math::Dot(d, axis) < spreadCos);
            p.vel = config.baseVelocity + d * speed;
        } else {
            p.vel = config.baseVelocity + rng_.OnUnitSphere() * speed;
        }
        p.life = p.maxLife = rng_.Range(config.lifeMin, config.lifeMax);
        p.size = config.sizeStart;
        p.sizeEnd = config.sizeEnd;
        p.color = config.colorStart;
        p.colorEnd = config.colorEnd;
        p.gravity = config.gravity;
        p.drag = config.drag;
        p.rot = rng_.Range(config.rotStartMin, config.rotStartMax);
        p.rotSpeed = rng_.Range(config.rotSpeedMin, config.rotSpeedMax);
        p.sizeEase = config.sizeEase;
        p.alphaEase = config.alphaEase;
        p.fadeIn = config.fadeIn;
        p.emissive = config.emissive;
        p.collideGround = config.collideGround;
        p.groundY = config.groundY;
        p.bounce = config.bounce;
        p.dieOnGround = config.dieOnGround;
        p.uvCols = config.uvCols ? config.uvCols : 1;
        p.uvRows = config.uvRows ? config.uvRows : 1;
        p.uvFps = config.uvFps;
        p.uvLoop = config.uvLoop;
        p.uvFlipY = config.uvFlipY;
        p.uvPhase = config.uvRandomStart ? rng_.Float() : 0.0f;
        p.additive = config.additive;
        p.texIndex = config.textureIndex;
        particles_.push_back(p);
    }
}

void ParticleSystem::Update(float dt) {
    for (size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.life -= dt;
        p.rot += p.rotSpeed * dt;
        if (p.drag > 0.0f) {
            // Frame-rate independent damping: v *= 1 / (1 + drag*dt).
            p.vel = p.vel * (1.0f / (1.0f + p.drag * dt));
        }
        p.vel.y += p.gravity * dt;
        p.pos += p.vel * dt;
        if (p.collideGround && p.pos.y < p.groundY) {
            p.pos.y = p.groundY;
            if (p.dieOnGround) {
                p.life = 0.0f;
            } else {
                // Reflect the vertical component and damp the rest, so ground
                // bursts scatter along the surface instead of sinking.
                p.vel.y = -p.vel.y * p.bounce;
                p.vel.x *= p.bounce > 0.0f ? 0.6f : 0.0f;
                p.vel.z *= p.bounce > 0.0f ? 0.6f : 0.0f;
            }
        }
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

namespace {
// Per-(texture, blend) instance arrays built once per Draw call.
struct ParticleBucket {
    std::vector<math::Vec3> pos;
    std::vector<float> size;
    std::vector<Color> col;
    std::vector<float> rot;
    std::vector<math::Vec4> uv;
};
} // namespace

void ParticleSystem::Draw(Renderer& renderer, const Texture& texture, float scale) {
    if (particles_.empty() || !texture.Valid()) return;
    const std::vector<Texture> one{texture};
    Draw(renderer, one, scale);
}

void ParticleSystem::Draw(Renderer& renderer, const std::vector<Texture>& textures, float scale) {
    // G1-5: batch every particle into ONE instanced 3D billboard draw per
    // (sprite, blend mode) instead of one screen-space call per particle. The
    // billboards are world-sized, camera-facing and depth-tested, so they
    // occlude/are occluded by the scene. A scene can now mix several sprites
    // (glow / spark / smoke / slash) via EmitterConfig::texture.
    if (particles_.empty() || textures.empty()) return;
    std::map<uint32_t, ParticleBucket> add, alpha;
    for (const Particle& p : particles_) {
        const float age01 = 1.0f - p.life / p.maxLife;
        // Lifetime ramps with independent easing: the shape of the size/alpha
        // curve is what sells most ability effects (a linear fade reads cheap).
        const float tSize = std::pow(age01, p.sizeEase);
        const float tAlpha = std::pow(age01, p.alphaEase);
        const float size = math::Lerp(p.size, p.sizeEnd, tSize) * scale;
        float alphaBase = math::Lerp(p.color.a, p.colorEnd.a, tAlpha);
        if (p.fadeIn > 0.0f && age01 < p.fadeIn) {
            alphaBase *= age01 / p.fadeIn; // quick bloom-in instead of a pop
        }
        Color c{math::Lerp(p.color.r, p.colorEnd.r, tAlpha),
                math::Lerp(p.color.g, p.colorEnd.g, tAlpha),
                math::Lerp(p.color.b, p.colorEnd.b, tAlpha),
                alphaBase};
        c.r *= p.emissive;
        c.g *= p.emissive;
        c.b *= p.emissive;
        // Flipbook frame -> atlas cell. Static emitters (uvFps == 0 or a 1x1
        // atlas) get the whole texture and skip the per-instance UV work.
        math::Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
        if (p.uvFps > 0.0f && (p.uvCols > 1 || p.uvRows > 1)) {
            const uint32_t frames = p.uvCols * p.uvRows;
            float phase = p.uvPhase + age01 * p.maxLife * p.uvFps;
            uint32_t frame = static_cast<uint32_t>(phase);
            if (p.uvLoop) {
                frame %= frames;
            } else {
                frame = frame < frames ? frame : frames - 1;
            }
            const uint32_t col = frame % p.uvCols;
            const uint32_t row = frame / p.uvCols;
            const uint32_t rowY = p.uvFlipY ? (p.uvRows - 1 - row) : row;
            uv = {static_cast<float>(col) / static_cast<float>(p.uvCols),
                  static_cast<float>(rowY) / static_cast<float>(p.uvRows),
                  1.0f / static_cast<float>(p.uvCols),
                  1.0f / static_cast<float>(p.uvRows)};
        }
        ParticleBucket& b = p.additive ? add[p.texIndex] : alpha[p.texIndex];
        b.pos.push_back(p.pos);
        b.size.push_back(size);
        b.col.push_back(c);
        b.rot.push_back(p.rot);
        b.uv.push_back(uv);
    }
    auto handle_for = [&](uint32_t ti) -> TextureHandle {
        if (ti >= textures.size()) ti = 0;
        return textures[ti].Handle();
    };
    // Additive particles are order-independent (blend commutes), so they draw
    // unsorted, one draw per sprite.
    for (auto it = add.begin(); it != add.end(); ++it) {
        ParticleBucket& b = it->second;
        if (b.pos.empty()) continue;
        renderer.DrawBillboards(b.pos.data(), b.size.data(), b.col.data(), handle_for(it->first),
                                static_cast<uint32_t>(b.pos.size()), BlendMode::Additive,
                                /*intensity=*/4.0f, b.rot.data(), b.uv.data());
    }
    // Alpha-blended particles (smoke/dust/poison) must draw back-to-front or
    // overlapping quads composite in the wrong order; sort by distance to the
    // camera (farthest first) within each sprite bucket.
    const math::Vec3 cam = renderer.CameraPosition();
    for (auto it = alpha.begin(); it != alpha.end(); ++it) {
        ParticleBucket& b = it->second;
        if (b.pos.empty()) continue;
        if (b.pos.size() > 1) {
            std::vector<uint32_t> order(b.pos.size());
            for (uint32_t k = 0; k < order.size(); ++k) order[k] = k;
            // Stable: equidistant particles (a ring burst) must not reshuffle
            // every frame -- an unstable sort makes them flicker as they swap.
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t c) {
                const math::Vec3 da = b.pos[a] - cam;
                const math::Vec3 dc = b.pos[c] - cam;
                return da.LengthSq() > dc.LengthSq();
            });
            ParticleBucket s;
            s.pos.resize(b.pos.size());
            s.size.resize(b.size.size());
            s.col.resize(b.col.size());
            s.rot.resize(b.rot.size());
            s.uv.resize(b.uv.size());
            for (size_t k = 0; k < order.size(); ++k) {
                s.pos[k] = b.pos[order[k]];
                s.size[k] = b.size[order[k]];
                s.col[k] = b.col[order[k]];
                s.rot[k] = b.rot[order[k]];
                s.uv[k] = b.uv[order[k]];
            }
            b.pos.swap(s.pos);
            b.size.swap(s.size);
            b.col.swap(s.col);
            b.rot.swap(s.rot);
            b.uv.swap(s.uv);
        }
        renderer.DrawBillboards(b.pos.data(), b.size.data(), b.col.data(), handle_for(it->first),
                                static_cast<uint32_t>(b.pos.size()), BlendMode::Alpha,
                                /*intensity=*/1.0f, b.rot.data(), b.uv.data());
    }
}

void ParticleSystem::Clear() { particles_.clear(); }

} // namespace neon::gfx
