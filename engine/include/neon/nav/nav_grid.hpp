#pragma once

#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"
#include "neon/math/vec2.hpp"

namespace neon::nav {

// A grid-based navigation field (Godot Navigation / Unity NavMesh-style, but
// data-driven): a 2D walkability bitmap in world space. Obstacles are marked
// unwalkable; A* finds the shortest path between world points on the grid.
// The grid is the editor's tool: scenes reference a .navgrid.json asset, the
// runtime queries paths, and the editor visualizes them in the viewport.
class NavGrid {
public:
    // Builds an empty (all-walkable) grid. `origin` is the world position of
    // cell (0,0); `cellSize` is the edge length of one cell.
    static NavGrid Create(int width, int height, float cellSize, math::Vec2 origin = {0, 0});

    bool Valid() const { return width_ > 0 && height_ > 0 && cellSize_ > 0.0f; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    float CellSize() const { return cellSize_; }
    const math::Vec2& Origin() const { return origin_; }

    bool Walkable(int x, int y) const;
    void SetWalkable(int x, int y, bool walkable);
    bool InBounds(int x, int y) const;

    // World -> cell (clamped to the grid) and cell -> world center.
    bool WorldToCell(math::Vec2 world, int* x, int* y) const;
    math::Vec2 CellToWorld(int x, int y) const;

    // A* path from `from` to `to` (world space): the cell centers the path
    // crosses, including the destination cell. Returns an empty vector when
    // either endpoint is unwalkable or no path exists. The path uses 8-way
    // moves (diagonal cost sqrt(2)) and prefers fewer turns.
    std::vector<math::Vec2> FindPath(math::Vec2 from, math::Vec2 to) const;

    // .navgrid.json round trip (used by the editor nav panel and packager).
    core::Result<core::Json> ToJson() const;
    static core::Result<NavGrid> FromJson(const std::string& jsonText);

private:
    int width_ = 0;
    int height_ = 0;
    float cellSize_ = 1.0f;
    math::Vec2 origin_{0, 0};
    std::vector<uint8_t> walkable_; // 1 = walkable
};

} // namespace neon::nav
