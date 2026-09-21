#pragma once
#include <cstdint>
#include <vector>

#include "neon/gfx/color.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// Scriptable ribbon trails: accumulate world-space points per trail and draw
// each as a camera-facing additive ribbon (Renderer::DrawTrail). Points age out
// after `pointLife`, so a trail fades behind a moving projectile. Cheap and
// depth-tested; no per-frame mesh allocation beyond the point buffer.
class TrailSystem {
public:
    // Returns a trail id (>0), or 0 on failure. `head` colours the newest point,
    // `tail` the oldest.
    uint32_t Create(float width, const Color& head, const Color& tail, float pointLife = 0.35f);
    void AddPoint(uint32_t id, const math::Vec3& p);
    // Stops accepting points; the existing ones fade out and the slot frees.
    void End(uint32_t id);
    void Update(float dt);
    void Draw(Renderer& renderer) const;
    void Clear();

private:
    struct Trail {
        float width = 0.5f;
        Color head{1, 1, 1, 1};
        Color tail{1, 1, 1, 0};
        float pointLife = 0.35f;
        bool ended = false;
        std::vector<math::Vec3> pts; // oldest first
        std::vector<float> ages;
    };
    std::vector<Trail> trails_;
    std::vector<uint32_t> freeSlots_;
};

} // namespace neon::gfx
