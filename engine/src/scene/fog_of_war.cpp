#include "neon/scene/fog_of_war.hpp"

#include <algorithm>
#include <cmath>

namespace neon::scene {
namespace {

// Nav-grid ground walkability at a world XZ point. No grid -> always clear (so
// projects without a baked nav grid still get fog, just without occlusion).
bool NavClear(const nav::NavGrid* nav, float x, float z) {
    if (!nav || !nav->Valid()) return true;
    int cx = 0, cy = 0;
    if (!nav->WorldToCell({x, z}, &cx, &cy)) return false;
    return nav->Walkable(cx, cy);
}

} // namespace

void FogOfWar::Setup(int cols, int rows, float cell, float minX, float minZ, float seenAlpha,
                     float unseenAlpha, math::Vec3 color) {
    cols_ = std::max(1, cols);
    rows_ = std::max(1, rows);
    cell_ = cell > 0.01f ? cell : 8.0f;
    minX_ = minX;
    minZ_ = minZ;
    seenAlpha_ = std::clamp(seenAlpha, 0.0f, 1.0f);
    unseenAlpha_ = std::clamp(unseenAlpha, 0.0f, 1.0f);
    color_ = color;
    vis_.assign(static_cast<size_t>(cols_) * rows_, 0);
    seen_.assign(static_cast<size_t>(cols_) * rows_, 0);
    lastMask_.clear();
}

void FogOfWar::Clear() {
    cols_ = rows_ = 0;
    vis_.clear();
    seen_.clear();
    lastMask_.clear();
}

void FogOfWar::BeginFrame() {
    std::fill(vis_.begin(), vis_.end(), static_cast<uint8_t>(0));
}

void FogOfWar::AddSource(float x, float z, float radius, const nav::NavGrid* nav) {
    if (!Configured() || radius <= 0.0f) return;
    const int c0 = static_cast<int>(std::floor((x - radius - minX_) / cell_));
    const int c1 = static_cast<int>(std::floor((x + radius - minX_) / cell_));
    const int r0 = static_cast<int>(std::floor((z - radius - minZ_) / cell_));
    const int r1 = static_cast<int>(std::floor((z + radius - minZ_) / cell_));
    const float r2 = radius * radius;
    for (int cz = r0; cz <= r1; ++cz) {
        if (cz < 0 || cz >= rows_) continue;
        for (int cx = c0; cx <= c1; ++cx) {
            if (cx < 0 || cx >= cols_) continue;
            const float wx = minX_ + (static_cast<float>(cx) + 0.5f) * cell_;
            const float wz = minZ_ + (static_cast<float>(cz) + 0.5f) * cell_;
            const float dx = wx - x, dz = wz - z;
            const float d2 = dx * dx + dz * dz;
            if (d2 > r2) continue;
            // Line of sight from the source to the cell center: every sample
            // between must be walkable.
            if (nav && nav->Valid()) {
                const float d = std::sqrt(d2);
                const int steps = std::max(1, static_cast<int>(std::floor(d / (cell_ * 0.5f))));
                bool clear = true;
                for (int i = 1; i < steps; ++i) {
                    const float t = static_cast<float>(i) / steps;
                    if (!NavClear(nav, x + dx * t, z + dz * t)) { clear = false; break; }
                }
                if (!clear) continue;
            }
            const int idx = Index(cx, cz);
            vis_[idx] = 1;
            seen_[idx] = 1;
        }
    }
}

bool FogOfWar::VisibleAt(float x, float z) const {    if (!Configured()) return true; // no fog grid -> everything visible
    const int cx = static_cast<int>(std::floor((x - minX_) / cell_));
    const int cz = static_cast<int>(std::floor((z - minZ_) / cell_));
    if (cx < 0 || cz < 0 || cx >= cols_ || cz >= rows_) return true;
    return vis_[Index(cx, cz)] != 0;
}

float FogOfWar::CellAlpha(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= cols_ || cz >= rows_) return 0.0f;
    const int i = Index(cx, cz);
    if (vis_[i] != 0) return 0.0f;
    return seen_[i] != 0 ? seenAlpha_ : unseenAlpha_;
}

int FogOfWar::Draw(std::vector<script::Draw2DCmd>& out,
                   const std::function<bool(const math::Vec3&, float&, float&)>& project,
                   float vpW, float vpH) const {
    if (!Configured() || !project) return 0;
    const int vcols = cols_ + 1, vrows = rows_ + 1;
    std::vector<float> vx(static_cast<size_t>(vcols) * vrows);
    std::vector<float> vy(static_cast<size_t>(vcols) * vrows);
    std::vector<uint8_t> ok(static_cast<size_t>(vcols) * vrows, 0);
    for (int vz = 0; vz < vrows; ++vz) {
        for (int gx = 0; gx < vcols; ++gx) {
            const float wx = minX_ + static_cast<float>(gx) * cell_;
            const float wz = minZ_ + static_cast<float>(vz) * cell_;
            float sx = 0.0f, sy = 0.0f;
            const bool o = project(math::Vec3{wx, 0.05f, wz}, sx, sy);
            const int k = vz * vcols + gx;
            vx[k] = sx;
            vy[k] = sy;
            ok[k] = o ? 1 : 0;
        }
    }
    auto vertexAlpha = [&](int gx, int gz) -> float {
        float sum = 0.0f;
        int n = 0;
        for (int dz = -1; dz <= 0; ++dz) {
            for (int dx = -1; dx <= 0; ++dx) {
                const int cx = gx + dx, cz = gz + dz;
                if (cx < 0 || cz < 0 || cx >= cols_ || cz >= rows_) continue;
                sum += CellAlpha(cx, cz);
                ++n;
            }
        }
        return n > 0 ? sum / static_cast<float>(n) : 0.0f;
    };
    const float margin = 64.0f;
    int tris = 0;
    for (int cz = 0; cz < rows_; ++cz) {
        for (int cx = 0; cx < cols_; ++cx) {
            const int v00 = cz * vcols + cx;
            const int v10 = v00 + 1;
            const int v01 = v00 + vcols;
            const int v11 = v01 + 1;
            if (!ok[v00] || !ok[v10] || !ok[v01] || !ok[v11]) continue;
            auto onScreen = [&](int k) {
                return vx[k] >= -margin && vx[k] <= vpW + margin && vy[k] >= -margin &&
                       vy[k] <= vpH + margin;
            };
            if (!onScreen(v00) && !onScreen(v10) && !onScreen(v01) && !onScreen(v11)) continue;
            const float a00 = vertexAlpha(cx, cz);
            const float a10 = vertexAlpha(cx + 1, cz);
            const float a01 = vertexAlpha(cx, cz + 1);
            const float a11 = vertexAlpha(cx + 1, cz + 1);
            if (a00 <= 0.004f && a10 <= 0.004f && a01 <= 0.004f && a11 <= 0.004f) continue;
            auto emit = [&](int ka, int kb, int kc, float aa, float ab, float ac) {
                script::Draw2DCmd c;
                c.kind = script::Draw2DCmd::Kind::TriangleGradient;
                c.x = vx[ka];  c.y = vy[ka];
                c.x2 = vx[kb]; c.y2 = vy[kb];
                c.w = vx[kc];  c.h = vy[kc];
                c.r = color_.x; c.g = color_.y; c.b = color_.z; c.a = aa;
                c.r2 = color_.x; c.g2 = color_.y; c.b2 = color_.z; c.a2 = ab;
                c.r3 = color_.x; c.g3 = color_.y; c.b3 = color_.z; c.a3 = ac;
                out.push_back(std::move(c));
                ++tris;
            };
            emit(v00, v10, v11, a00, a10, a11);
            emit(v00, v11, v01, a00, a11, a01);
        }
    }
    return tris;
}

bool FogOfWar::BuildMask(std::vector<uint8_t>& rgba) {
    if (!Configured()) return false;
    const size_t n = static_cast<size_t>(cols_) * rows_;
    std::vector<uint8_t> mask(n * 4);
    const uint8_t r = static_cast<uint8_t>(color_.x * 255.0f);
    const uint8_t g = static_cast<uint8_t>(color_.y * 255.0f);
    const uint8_t b = static_cast<uint8_t>(color_.z * 255.0f);
    const uint8_t seenA = static_cast<uint8_t>(seenAlpha_ * 255.0f);
    const uint8_t unseenA = static_cast<uint8_t>(unseenAlpha_ * 255.0f);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t a = vis_[i] != 0 ? 0 : (seen_[i] != 0 ? seenA : unseenA);
        // Row flip: the ground plane's V axis runs opposite to the grid's +Z
        // rows, so write rows bottom-up.
        const size_t cz = i / static_cast<size_t>(cols_);
        const size_t cx = i % static_cast<size_t>(cols_);
        const size_t d = (static_cast<size_t>(rows_) - 1 - cz) * static_cast<size_t>(cols_) + cx;
        mask[d * 4 + 0] = r;
        mask[d * 4 + 1] = g;
        mask[d * 4 + 2] = b;
        mask[d * 4 + 3] = a;
    }
    if (mask == lastMask_) return false; // unchanged: skip the GPU upload
    lastMask_ = mask;
    rgba = std::move(mask);
    return true;
}

} // namespace neon::scene
