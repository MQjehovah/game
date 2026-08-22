#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace neon::server {

// Nine-grid area-of-interest index (T6.5). Bins entity positions into a uniform
// grid of fixed-size cells and answers "which entity ids are in the (2r+1)^2
// cells around a focus" queries. Pure, deterministic and testable: no sockets,
// no clocks, no world dependency.
//
// CELL MATH: the cell coordinate of an axis is floor(coord / cellSize), so
// every position belongs to exactly ONE cell (a position exactly on a cell
// boundary floors into the higher-indexed cell). Negative positions floor to
// negative cells, so the world extends unbounded in every direction.
//
// DETERMINISM: Update() rebuilds the index from scratch every call (no stale
// state survives between updates). InterestSet() visits cells in row-major
// order (cz, then cx) and, within a cell, entity ids in ascending id order.
// Identical Update() inputs therefore produce identical, order-stable interest
// sets — which keeps per-client replication streams reproducible.
class AoiGrid {
public:
    // One indexed entity: a stable id + the x/z position to bin. The grid is
    // horizontal, so y is not part of the interest computation.
    struct Entry {
        uint64_t id = 0;
        float x = 0.0f;
        float z = 0.0f;
    };

    static constexpr float kDefaultCellSize = 32.0f; // world units per cell edge

    // Cell edge length in world units. Must be > 0; values <= 0 are ignored
    // (the grid keeps its previous size). Default 32.
    void SetCellSize(float cellSize);
    float CellSize() const { return cellSize_; }

    // Rebuilds the cell -> entity-ids index from `entities`. Within a cell the
    // ids are in ascending order (deterministic). Duplicate ids are allowed:
    // the LAST entry for an id wins (the server's per-tick collection already
    // yields unique ids, but the grid stays robust standalone).
    void Update(const std::vector<Entry>& entities);

    // Empties the index (equivalent to Update with an empty vector).
    void Clear();

    // Ids of the entities in the (2*radiusCells+1)^2 cells centered on the
    // cell containing (x, z). radiusCells == 0 returns just the focus cell.
    // Negative radii return an empty set. The result is deterministic:
    // row-major cell order, then ascending id order.
    std::vector<uint64_t> InterestSet(float x, float z, int radiusCells) const;

    // Cell coordinate of a position: floor(coord / cellSize). Public so tests
    // can verify the boundary/negative-floor semantics directly.
    static int64_t CellCoord(float coord, float cellSize);

private:
    float cellSize_ = kDefaultCellSize;
    // cells_[cz][cx] -> entity ids in that cell (ascending id order).
    std::map<int64_t, std::map<int64_t, std::vector<uint64_t>>> cells_;
};

} // namespace neon::server
