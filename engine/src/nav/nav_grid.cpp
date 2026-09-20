#include "neon/nav/nav_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

namespace neon::nav {
namespace {

// Rasterization scratch: per cell the lowest and highest world Y of any
// geometry covering it, plus a coverage flag. A cell whose column spans more
// than `clearance` holds a wall/ledge and is blocked by the bake.
struct HeightField {
    float minX = 0.0f;
    float minZ = 0.0f;
    float cell = 1.0f;
    int width = 0;
    int height = 0;
    std::vector<float> lo;
    std::vector<float> hi;
    std::vector<uint8_t> has;

    int Index(int x, int z) const { return z * width + x; }

    void CellOf(float x, float z, int* cx, int* cz) const {
        *cx = static_cast<int>(std::floor((x - minX) / cell));
        *cz = static_cast<int>(std::floor((z - minZ) / cell));
    }

    // Scanline-fills one triangle into the grid, tracking the Y span of every
    // covered cell (conservative: the triangle's full Y range for all its
    // cells, which thickens walls slightly -- desirable for navigation).
    void AddTriangle(const math::Vec3& a, const math::Vec3& b, const math::Vec3& c) {
        const float xs[3] = {a.x, b.x, c.x};
        const float zs[3] = {a.z, b.z, c.z};
        const float ys[3] = {a.y, b.y, c.y};
        const float yLo = std::min({ys[0], ys[1], ys[2]});
        const float yHi = std::max({ys[0], ys[1], ys[2]});
        int cz0 = static_cast<int>(std::floor((std::min({zs[0], zs[1], zs[2]}) - minZ) / cell));
        int cz1 = static_cast<int>(std::floor((std::max({zs[0], zs[1], zs[2]}) - minZ) / cell));
        cz0 = std::max(0, cz0);
        cz1 = std::min(height - 1, cz1);
        for (int cz = cz0; cz <= cz1; ++cz) {
            const float zc = minZ + (static_cast<float>(cz) + 0.5f) * cell;
            float sx[3];
            int n = 0;
            for (int e = 0; e < 3; ++e) {
                const float zi = zs[e], zj = zs[(e + 1) % 3];
                const float xi = xs[e], xj = xs[(e + 1) % 3];
                if ((zi <= zc && zc < zj) || (zj <= zc && zc < zi)) {
                    const float t = (zc - zi) / (zj - zi);
                    sx[n++] = xi + (xj - xi) * t;
                }
            }
            if (n < 2) continue;
            const float xLo = std::min(sx[0], sx[1]);
            const float xHi = std::max(sx[0], sx[1]);
            int cx0 = static_cast<int>(std::floor((xLo - minX) / cell));
            int cx1 = static_cast<int>(std::floor((xHi - minX) / cell));
            cx0 = std::max(0, cx0);
            cx1 = std::min(width - 1, cx1);
            for (int cx = cx0; cx <= cx1; ++cx) {
                const int k = Index(cx, cz);
                has[k] = 1;
                if (yLo < lo[k]) lo[k] = yLo;
                if (yHi > hi[k]) hi[k] = yHi;
            }
        }
    }
};

struct Node {
    int x = 0;
    int y = 0;
    float g = 0.0f; // cost from start
    float f = 0.0f; // g + heuristic
    int px = -1;    // parent cell
    int py = -1;
};

constexpr int kDirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},   {0, -1},
                             {1, 1},  {1, -1}, {-1, 1},  {-1, -1}};

float Heuristic(int x0, int y0, int x1, int y1) {
    // Octile distance: matches 8-way move costs.
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    return static_cast<float>(std::max(dx, dy)) +
           (1.41421356f - 1.0f) * static_cast<float>(std::min(dx, dy));
}

} // namespace

NavGrid NavGrid::Create(int width, int height, float cellSize, math::Vec2 origin) {
    NavGrid g;
    g.width_ = std::max(width, 0);
    g.height_ = std::max(height, 0);
    g.cellSize_ = cellSize > 0.0f ? cellSize : 1.0f;
    g.origin_ = origin;
    g.walkable_.assign(static_cast<size_t>(g.width_) * g.height_, 1);
    return g;
}

bool NavGrid::InBounds(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

bool NavGrid::Walkable(int x, int y) const {
    return InBounds(x, y) && walkable_[static_cast<size_t>(y) * width_ + static_cast<size_t>(x)] != 0;
}

void NavGrid::SetWalkable(int x, int y, bool walkable) {
    if (!InBounds(x, y)) return;
    walkable_[static_cast<size_t>(y) * width_ + static_cast<size_t>(x)] = walkable ? 1 : 0;
}

bool NavGrid::WorldToCell(math::Vec2 world, int* x, int* y) const {
    if (!Valid()) return false;
    const int cx = static_cast<int>(std::floor((world.x - origin_.x) / cellSize_));
    const int cy = static_cast<int>(std::floor((world.y - origin_.y) / cellSize_));
    if (!InBounds(cx, cy)) return false;
    if (x) *x = cx;
    if (y) *y = cy;
    return true;
}

math::Vec2 NavGrid::CellToWorld(int x, int y) const {
    return {origin_.x + (static_cast<float>(x) + 0.5f) * cellSize_,
            origin_.y + (static_cast<float>(y) + 0.5f) * cellSize_};
}

std::vector<math::Vec2> NavGrid::FindPath(math::Vec2 from, math::Vec2 to) const {
    std::vector<math::Vec2> out;
    if (!Valid()) return out;
    int sx = 0, sy = 0, tx = 0, ty = 0;
    if (!WorldToCell(from, &sx, &sy) || !WorldToCell(to, &tx, &ty)) return out;
    if (!Walkable(sx, sy) || !Walkable(tx, ty)) return out;

    const size_t count = static_cast<size_t>(width_) * height_;
    std::vector<Node> nodes(count);
    std::vector<uint8_t> closed(count, 0);
    std::vector<uint8_t> open(count, 0);
    auto idx = [&](int x, int y) { return static_cast<size_t>(y) * width_ + static_cast<size_t>(x); };

    auto cmp = [](const Node* a, const Node* b) { return a->f > b->f; };
    std::priority_queue<Node*, std::vector<Node*>, decltype(cmp)> pq(cmp);
    Node& start = nodes[idx(sx, sy)];
    start.x = sx;
    start.y = sy;
    start.g = 0.0f;
    start.f = Heuristic(sx, sy, tx, ty);
    start.px = sx;
    start.py = sy;
    pq.push(&start);
    open[idx(sx, sy)] = 1;

    bool found = false;
    while (!pq.empty()) {
        Node* cur = pq.top();
        pq.pop();
        if (closed[idx(cur->x, cur->y)]) continue;
        closed[idx(cur->x, cur->y)] = 1;
        open[idx(cur->x, cur->y)] = 0;
        if (cur->x == tx && cur->y == ty) {
            found = true;
            break;
        }
        for (const auto& d : kDirs) {
            const int nx = cur->x + d[0];
            const int ny = cur->y + d[1];
            if (!Walkable(nx, ny) || closed[idx(nx, ny)]) continue;
            // Prevent cutting corners: both orthogonal neighbours must be
            // walkable for a diagonal move.
            if (d[0] != 0 && d[1] != 0 &&
                (!Walkable(cur->x + d[0], cur->y) || !Walkable(cur->x, cur->y + d[1])))
                continue;
            const float step = (d[0] != 0 && d[1] != 0) ? 1.41421356f : 1.0f;
            const float ng = cur->g + step;
            Node& next = nodes[idx(nx, ny)];
            if (open[idx(nx, ny)] && ng >= next.g) continue;
            next.x = nx;
            next.y = ny;
            next.g = ng;
            next.f = ng + Heuristic(nx, ny, tx, ty);
            next.px = cur->x;
            next.py = cur->y;
            if (!open[idx(nx, ny)]) {
                open[idx(nx, ny)] = 1;
                pq.push(&next);
            }
        }
    }
    if (!found) return out;

    // Reconstruct path (start -> ... -> goal), skipping the start cell.
    std::vector<math::Vec2> reversed;
    int cx = tx, cy = ty;
    while (!(cx == sx && cy == sy)) {
        reversed.push_back(CellToWorld(cx, cy));
        const Node& n = nodes[idx(cx, cy)];
        cx = n.px;
        cy = n.py;
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

core::Result<core::Json> NavGrid::ToJson() const {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    auto num = [](double v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = v;
        return j;
    };
    auto str = [](const std::string& s) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = s;
        return j;
    };
    root.object_["width"] = num(width_);
    root.object_["height"] = num(height_);
    root.object_["cellSize"] = num(cellSize_);
    core::Json org;
    org.type_ = core::Json::Type::Array;
    org.array_ = {num(origin_.x), num(origin_.y)};
    root.object_["origin"] = org;
    core::Json rows;
    rows.type_ = core::Json::Type::Array;
    for (int y = 0; y < height_; ++y) {
        std::string row;
        row.reserve(static_cast<size_t>(width_));
        for (int x = 0; x < width_; ++x)
            row += Walkable(x, y) ? '.' : '#';
        rows.array_.push_back(str(row));
    }
    root.object_["rows"] = rows;
    return core::Result<core::Json>::Ok(std::move(root));
}

core::Result<NavGrid> NavGrid::FromJson(const std::string& jsonText) {
    std::string perr;
    core::Json root = core::Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty())
        return core::Result<NavGrid>::Err("nav: JSON parse error: " + perr);
    if (!root.IsObject())
        return core::Result<NavGrid>::Err("nav: nav grid must be a JSON object");
    const int w = root.Get("width") ? root.Get("width")->GetInt(-1) : -1;
    const int h = root.Get("height") ? root.Get("height")->GetInt(-1) : -1;
    const float cell = root.Get("cellSize")
                           ? static_cast<float>(root.Get("cellSize")->GetNumber())
                           : 1.0f;
    if (w <= 0 || h <= 0 || cell <= 0.0f)
        return core::Result<NavGrid>::Err("nav: invalid grid dimensions");
    NavGrid g = Create(w, h, cell);
    if (const core::Json* org = root.Get("origin")) {
        if (org->IsArray() && org->Size() == 2) {
            g.origin_ = {static_cast<float>(org->At(0)->GetNumber()),
                         static_cast<float>(org->At(1)->GetNumber())};
        }
    }
    const core::Json* rows = root.Get("rows");
    if (!rows || !rows->IsArray())
        return core::Result<NavGrid>::Err("nav: missing 'rows' walkability map");
    for (int y = 0; y < h && y < static_cast<int>(rows->Size()); ++y) {
        const core::Json* row = rows->At(static_cast<size_t>(y));
        if (!row || !row->IsString()) continue;
        const std::string& s = row->GetString();
        for (int x = 0; x < w && x < static_cast<int>(s.size()); ++x)
            g.SetWalkable(x, y, s[static_cast<size_t>(x)] == '.');
    }
    return core::Result<NavGrid>::Ok(std::move(g));
}

core::Result<NavGrid> BakeFromTriangles(const math::Vec3* positions, size_t vertexCount,
                                        const uint32_t* indices, size_t indexCount,
                                        const BakeParams& params) {
    if (positions == nullptr || vertexCount == 0)
        return core::Result<NavGrid>::Err("nav bake: no vertices");
    const size_t triCount = indexCount / 3;
    if (triCount == 0) return core::Result<NavGrid>::Err("nav bake: no triangles");
    auto vi = [&](size_t i) -> uint32_t {
        return indices ? indices[i] : static_cast<uint32_t>(i);
    };
    if (indices != nullptr) {
        for (size_t i = 0; i < indexCount; ++i) {
            if (indices[i] >= vertexCount)
                return core::Result<NavGrid>::Err("nav bake: index out of range");
        }
    }

    // World-space AABB over the referenced vertices.
    float minX = positions[vi(0)].x, maxX = minX;
    float minZ = positions[vi(0)].z, maxZ = minZ;
    for (size_t t = 0; t < indexCount; ++t) {
        const math::Vec3& p = positions[vi(t)];
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }

    HeightField field;
    field.minX = minX;
    field.minZ = minZ;
    field.cell = params.cellSize > 0.0f ? params.cellSize : 1.0f;
    field.width = std::max(1, static_cast<int>(std::ceil((maxX - minX) / field.cell)));
    field.height = std::max(1, static_cast<int>(std::ceil((maxZ - minZ) / field.cell)));
    const size_t cells = static_cast<size_t>(field.width) * field.height;
    field.lo.assign(cells, 1e30f);
    field.hi.assign(cells, -1e30f);
    field.has.assign(cells, 0);
    for (size_t t = 0; t < triCount; ++t) {
        field.AddTriangle(positions[vi(t * 3 + 0)], positions[vi(t * 3 + 1)],
                          positions[vi(t * 3 + 2)]);
    }

    // Walkable = geometry present and no tall obstacle over the column.
    std::vector<uint8_t> walk(cells, 0);
    for (size_t k = 0; k < cells; ++k)
        walk[k] = (field.has[k] && (field.hi[k] - field.lo[k]) <= params.clearance) ? 1 : 0;

    // Dilate the blocked set by the agent radius (keep off walls) with a
    // discretized disk.
    const float radiusCells = params.agentRadius / field.cell;
    const int rr = static_cast<int>(std::ceil(radiusCells));
    std::vector<math::Vec2> disk;
    for (int dz = -rr; dz <= rr; ++dz) {
        for (int dx = -rr; dx <= rr; ++dx) {
            if (static_cast<float>(dx * dx + dz * dz) <= radiusCells * radiusCells + 1e-6f)
                disk.push_back({static_cast<float>(dx), static_cast<float>(dz)});
        }
    }
    std::vector<uint8_t> eroded = walk;
    for (int cz = 0; cz < field.height; ++cz) {
        for (int cx = 0; cx < field.width; ++cx) {
            if (walk[field.Index(cx, cz)] != 0) continue;
            for (const math::Vec2& d : disk) {
                const int nx = cx + static_cast<int>(d.x);
                const int nz = cz + static_cast<int>(d.y);
                if (nx >= 0 && nz >= 0 && nx < field.width && nz < field.height)
                    eroded[field.Index(nx, nz)] = 0;
            }
        }
    }

    // Carve forced-walkable anchors (spawns / bases).
    for (const BakeParams::Disk& d : params.forceWalkable) {
        int ccx = 0, ccz = 0;
        field.CellOf(d.center.x, d.center.y, &ccx, &ccz);
        const int rc = static_cast<int>(std::ceil(d.radius / field.cell));
        for (int dz = -rc; dz <= rc; ++dz) {
            for (int dx = -rc; dx <= rc; ++dx) {
                if (dx * dx + dz * dz > rc * rc) continue;
                const int nx = ccx + dx, nz = ccz + dz;
                if (nx >= 0 && nz >= 0 && nx < field.width && nz < field.height)
                    eroded[field.Index(nx, nz)] = 1;
            }
        }
    }

    NavGrid g = NavGrid::Create(field.width, field.height, field.cell, {minX, minZ});
    for (int cz = 0; cz < field.height; ++cz)
        for (int cx = 0; cx < field.width; ++cx)
            g.SetWalkable(cx, cz, eroded[field.Index(cx, cz)] != 0);
    return core::Result<NavGrid>::Ok(std::move(g));
}

} // namespace neon::nav
