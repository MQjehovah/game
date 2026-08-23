#include "neon/nav/nav_grid.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace neon::nav {
namespace {

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

} // namespace neon::nav
