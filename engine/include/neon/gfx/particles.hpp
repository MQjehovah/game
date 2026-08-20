#pragma once
#include <cstdint>
#include <vector>
#include "neon/core/rng.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

struct EmitterConfig {
    uint32_t count = 10;
    math::Vec3 position{};
    math::Vec3 baseVelocity{};
    float speedMin = 1.0f;
    float speedMax = 3.0f;
    float lifeMin = 0.4f;
    float lifeMax = 0.9f;
    float sizeStart = 0.5f;
    float sizeEnd = 0.05f;
    Color colorStart{1.0f, 1.0f, 1.0f, 1.0f};
    Color colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    float gravity = 0.0f;
    bool additive = true;
};

class ParticleSystem {
public:
    void Emit(const EmitterConfig& config);
    void Update(float dt);
    void Draw(Renderer& renderer, const Texture& texture, float scale = 1.0f);
    void Clear();
    size_t Count() const { return particles_.size(); }

private:
    struct Particle {
        math::Vec3 pos;
        math::Vec3 vel;
        float life;
        float maxLife;
        float size;
        float sizeEnd;
        Color color;
        Color colorEnd;
        float gravity;
        bool additive;
    };
    std::vector<Particle> particles_;
    core::Rng rng_{0x5EEDC0DEull};
};

} // namespace neon::gfx
