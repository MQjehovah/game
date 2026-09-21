#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "neon/math/vec3.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/script/bindings.hpp"

namespace neon::scene {

// Engine-owned fog-of-war mask. A coarse visibility grid the script fills with
// observer sources each tick; the engine runs the line-of-sight test against
// the scene nav grid and renders a SOFT mask (per-vertex alpha gradient) into
// the on_render 2D canvas. Game rules that stay in the script: which team sees
// for which observer, vision radii, brush/grass hiding.
//
// The mask is drawn as world-projected gradient triangles, so a cell edge fades
// over one cell instead of showing the hard black squares a per-cell quad
// overlay produces.
class FogOfWar {
public:
    void Setup(int cols, int rows, float cell, float minX, float minZ, float seenAlpha,
               float unseenAlpha, math::Vec3 color);
    void Clear();
    bool Configured() const { return cols_ > 0 && rows_ > 0; }

    // Clears the current-frame visible set (keeps `seen`: the explored memory).
    // Call once per tick before AddSource.
    void BeginFrame();
    // Reveals cells within `radius` of (x, z) whose line of sight is clear.
    // A null/invalid nav grid means "no occluders".
    void AddSource(float x, float z, float radius, const nav::NavGrid* nav);
    bool VisibleAt(float x, float z) const;

    // Emits the soft mask into `out` using `project` (world -> design pixels).
    // Returns the number of triangles emitted.
    int Draw(std::vector<script::Draw2DCmd>& out,
             const std::function<bool(const math::Vec3&, float&, float&)>& project, float vpW,
             float vpH) const;

private:
    int Index(int cx, int cz) const { return cz * cols_ + cx; }
    float CellAlpha(int cx, int cz) const;

    int cols_ = 0, rows_ = 0;
    float cell_ = 8.0f, minX_ = 0.0f, minZ_ = 0.0f;
    float seenAlpha_ = 0.5f, unseenAlpha_ = 0.95f;
    math::Vec3 color_{0.02f, 0.02f, 0.05f};
    std::vector<uint8_t> vis_, seen_;
};

} // namespace neon::scene
