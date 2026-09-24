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
    // Minimum spacing between stored points (world units). Points closer than
    // this are merged; a segment that jumps more than `maxSpacing` is
    // interpolated so a fast projectile still leaves a continuous ribbon
    // instead of a chain of disconnected quads.
    void SetSpacing(float minSpacing, float maxSpacing) {
        minSpacing_ = minSpacing > 0.0f ? minSpacing : 0.01f;
        maxSpacing_ = maxSpacing > minSpacing_ ? maxSpacing : minSpacing_ * 4.0f;
    }
    // Width multiplier applied at the oldest point (ribbon taper).
    void SetTailWidthScale(float scale) { tailWidthScale_ = scale; }

private:
    struct Trail {
        float width = 0.5f;
        Color head{1, 1, 1, 1};
        Color tail{1, 1, 1, 0};
        float pointLife = 0.35f;
        bool ended = false;
        // Fixed-capacity ring buffer, oldest first. A ring keeps AddPoint O(1):
        // the previous vector erased from the front every frame, which both
        // memmoved the whole trail and made long trails visibly stutter.
        std::vector<math::Vec3> pts;
        std::vector<float> ages;
        size_t begin = 0; // ring index of the oldest point
        size_t count = 0;
    };
    std::vector<Trail> trails_;
    std::vector<uint32_t> freeSlots_;
    // Reused ribbon scratch so Draw() does not allocate per frame.
    mutable std::vector<math::Vec3> ribbon_;
    mutable std::vector<Renderer::TrailDraw> drawBatch_;
    mutable std::vector<size_t> drawOffsets_;
    float minSpacing_ = 0.12f;
    float maxSpacing_ = 0.6f;
    float tailWidthScale_ = 0.15f;
};

} // namespace neon::gfx
