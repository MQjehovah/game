#pragma once
#include <cstdint>
#include <vector>
#include "neon/core/rng.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// Spawn volume of a burst. Shapes give the script a single call for the effects
// a MOBA needs constantly (impact rings, cone bursts, ground discs) instead of
// looping Emit() once per particle in Lua.
enum class EmitterShape {
    Point,      // all particles at `position` (sparks)
    Sphere,     // uniform in a ball of `shapeRadius` (explosions)
    Hemisphere, // upper half of that ball (dust, smoke)
    Cone,       // cone around `direction` (flame/beam)
    Ring,       // thin ring on the XZ plane (impact shockwave)
    Disc,       // filled disc on the XZ plane (ground burst)
};

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
    // --- spawn volume -------------------------------------------------------
    EmitterShape shape = EmitterShape::Sphere;
    float shapeRadius = 0.25f;   // ball/ring/disc radius
    float ringThickness = 0.06f; // Ring: radial jitter; Cone: base radius
    float coneAngleDeg = 40.0f;  // Cone: half angle around `direction`
    math::Vec3 direction{0.0f, 1.0f, 0.0f};
    // Velocity cone half angle in degrees around `direction`. 0 = legacy
    // behaviour (isotropic random direction on top of baseVelocity).
    float spreadDeg = 0.0f;
    // --- motion ------------------------------------------------------------
    float drag = 0.0f;           // velocity damping per second (1/s)
    // --- per-particle roll (radians) ---------------------------------------
    float rotStartMin = 0.0f, rotStartMax = 0.0f;
    float rotSpeedMin = 0.0f, rotSpeedMax = 0.0f;
    // --- atlas / flipbook ---------------------------------------------------
    uint32_t uvCols = 1, uvRows = 1;
    float uvFps = 0.0f;          // 0 = static quad over the whole texture
    bool uvLoop = true;
    bool uvRandomStart = true;
    bool uvFlipY = false;        // atlas authored top-down
    // --- lifetime curves ----------------------------------------------------
    float sizeEase = 1.0f;       // exponent applied to t for the size ramp
    float alphaEase = 1.0f;      // exponent for the alpha ramp
    float fadeIn = 0.0f;         // 0..1 fraction of life spent fading in
    // --- ground interaction -------------------------------------------------
    bool collideGround = false;
    float groundY = 0.0f;
    float bounce = 0.0f;         // 0 = stick, 1 = perfectly elastic
    bool dieOnGround = false;
    // --- shading ------------------------------------------------------------
    // HDR multiplier applied on top of the particle colour: > 1 pushes additive
    // particles above the bloom threshold (the "big game" glow).
    float emissive = 1.0f;
};

class ParticleSystem {
public:
    void Emit(const EmitterConfig& config);
    void Update(float dt);
    void Draw(Renderer& renderer, const Texture& texture, float scale = 1.0f);
    void Clear();
    size_t Count() const { return particles_.size(); }
    // Live-particle budget. Reaching it drops *incoming* particles (a burst is
    // clamped) instead of evicting live ones: an effect that is already playing
    // keeps its shape, and the newest call simply spawns fewer.
    void SetBudget(size_t budget) { budget_ = budget ? budget : 1; }
    size_t Budget() const { return budget_; }
    // Particles dropped by the budget since the last ResetStats (diagnostics).
    size_t DroppedByBudget() const { return dropped_; }
    void ResetDropped() { dropped_ = 0; }

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
        float drag;
        float rot;
        float rotSpeed;
        float sizeEase;
        float alphaEase;
        float fadeIn;
        float emissive;
        bool collideGround;
        float groundY;
        float bounce;
        bool dieOnGround;
        uint32_t uvCols;
        uint32_t uvRows;
        float uvFps;
        bool uvLoop;
        bool uvFlipY;
        float uvPhase; // start offset in [0,1) so a burst does not flicker in sync
        bool additive;
    };
    std::vector<Particle> particles_;
    size_t budget_ = 32768;
    size_t dropped_ = 0;
    core::Rng rng_{0x5EEDC0DEull};
};

} // namespace neon::gfx
