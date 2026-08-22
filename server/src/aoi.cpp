#include "aoi.hpp"

#include <cmath>

namespace neon::server {

// floor-division in double space so float rounding cannot push a boundary
// position (e.g. exactly cellSize) into the wrong cell.
int64_t AoiGrid::CellCoord(float coord, float cellSize) {
    return static_cast<int64_t>(
        std::floor(static_cast<double>(coord) / static_cast<double>(cellSize)));
}

void AoiGrid::SetCellSize(float cellSize) {
    if (cellSize > 0.0f) cellSize_ = cellSize;
}

void AoiGrid::Update(const std::vector<Entry>& entities) {
    cells_.clear();
    // Deduplicate ids: the last entry for an id wins. Sorting by id also gives
    // cells a deterministic ascending-id order regardless of input order.
    std::map<uint64_t, Entry> uniq;
    for (const Entry& e : entities) uniq[e.id] = e;
    for (const auto& kv : uniq) {
        const Entry& e = kv.second;
        cells_[CellCoord(e.z, cellSize_)][CellCoord(e.x, cellSize_)].push_back(e.id);
    }
}

void AoiGrid::Clear() { cells_.clear(); }

std::vector<uint64_t> AoiGrid::InterestSet(float x, float z, int radiusCells) const {
    std::vector<uint64_t> out;
    if (radiusCells < 0) return out;
    const int64_t cz = CellCoord(z, cellSize_);
    const int64_t cx = CellCoord(x, cellSize_);
    for (int64_t dz = -radiusCells; dz <= radiusCells; ++dz) {
        const auto zIt = cells_.find(cz + dz);
        if (zIt == cells_.end()) continue;
        for (int64_t dx = -radiusCells; dx <= radiusCells; ++dx) {
            const auto xIt = zIt->second.find(cx + dx);
            if (xIt == zIt->second.end()) continue;
            out.insert(out.end(), xIt->second.begin(), xIt->second.end());
        }
    }
    return out;
}

} // namespace neon::server
