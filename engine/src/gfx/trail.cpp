#include "neon/gfx/trail.hpp"

#include <algorithm>

#include "neon/math/math.hpp"

namespace neon::gfx {

namespace {
constexpr size_t kMaxPoints = 128; // per trail; the oldest point is recycled
}

uint32_t TrailSystem::Create(float width, const Color& head, const Color& tail, float pointLife) {
    Trail t;
    t.width = width;
    t.head = head;
    t.tail = tail;
    t.pointLife = pointLife;
    t.pts.resize(kMaxPoints);
    t.ages.resize(kMaxPoints);
    uint32_t id;
    if (!freeSlots_.empty()) {
        id = freeSlots_.back();
        freeSlots_.pop_back();
        trails_[id - 1] = std::move(t);
    } else {
        trails_.push_back(std::move(t));
        id = static_cast<uint32_t>(trails_.size());
    }
    return id;
}

void TrailSystem::AddPoint(uint32_t id, const math::Vec3& p) {
    if (id == 0 || id > trails_.size()) return;
    Trail& t = trails_[id - 1];
    if (t.ended) return;
    if (t.count > 0) {
        // Newest point = the slot before the oldest one in ring order.
        const size_t newest = (t.begin + t.count - 1) % kMaxPoints;
        const float moved = math::Distance(p, t.pts[newest]);
        if (moved < minSpacing_) return; // too close: merge (no jitter/quads)
        // A long jump (tunnelling projectile / low frame rate) is filled with
        // interpolated points so the ribbon stays continuous.
        if (moved > maxSpacing_) {
            const int steps = static_cast<int>(moved / maxSpacing_);
            for (int s = 1; s < steps; ++s) {
                const float f = static_cast<float>(s) / static_cast<float>(steps);
                const math::Vec3 q = t.pts[newest] + (p - t.pts[newest]) * f;
                const size_t slot = (t.begin + t.count) % kMaxPoints;
                t.pts[slot] = q;
                t.ages[slot] = 0.0f;
                if (t.count < kMaxPoints) {
                    ++t.count;
                } else {
                    t.begin = (t.begin + 1) % kMaxPoints;
                }
            }
        }
    }
    const size_t slot = (t.begin + t.count) % kMaxPoints;
    t.pts[slot] = p;
    t.ages[slot] = 0.0f;
    if (t.count < kMaxPoints) {
        ++t.count;
    } else {
        t.begin = (t.begin + 1) % kMaxPoints; // recycle the oldest
    }
}

void TrailSystem::End(uint32_t id) {
    if (id == 0 || id > trails_.size()) return;
    trails_[id - 1].ended = true;
}

void TrailSystem::Update(float dt) {
    for (size_t i = 0; i < trails_.size(); ++i) {
        Trail& t = trails_[i];
        if (t.count == 0) {
            // Idle slot: recycle when it has already ended (or was never used).
            if (t.ended) {
                freeSlots_.push_back(static_cast<uint32_t>(i + 1));
                t = Trail{};
            }
            continue;
        }
        for (size_t k = 0; k < t.count; ++k)
            t.ages[(t.begin + k) % kMaxPoints] += dt;
        // Drop expired points from the oldest end (the ring keeps this O(dropped)).
        size_t drop = 0;
        while (drop < t.count && t.ages[t.begin] > t.pointLife) {
            t.begin = (t.begin + 1) % kMaxPoints;
            --t.count;
            ++drop;
        }
        if (t.ended && t.count == 0) {
            freeSlots_.push_back(static_cast<uint32_t>(i + 1));
            t = Trail{};
        }
    }
}

void TrailSystem::Draw(Renderer& renderer) const {
    // Every live ribbon is materialised into one contiguous scratch buffer (the
    // ring is not contiguous, and the renderer consumes the pointers in the
    // DrawTrails call), then all of them are submitted as ONE draw call.
    ribbon_.clear();
    drawBatch_.clear();
    drawOffsets_.clear();
    size_t total = 0;
    for (const Trail& t : trails_) {
        if (t.count >= 2) total += t.count;
    }
    ribbon_.reserve(total);
    drawBatch_.reserve(trails_.size());
    drawOffsets_.reserve(trails_.size());
    for (const Trail& t : trails_) {
        if (t.count < 2) continue;
        Renderer::TrailDraw d;
        d.count = static_cast<uint32_t>(t.count);
        d.width = t.width;
        d.tailWidthScale = tailWidthScale_;
        d.head = t.head;
        d.tail = t.tail;
        drawOffsets_.push_back(ribbon_.size());
        drawBatch_.push_back(d);
        for (size_t k = 0; k < t.count; ++k) ribbon_.push_back(t.pts[(t.begin + k) % kMaxPoints]);
    }
    for (size_t i = 0; i < drawBatch_.size(); ++i)
        drawBatch_[i].points = ribbon_.data() + drawOffsets_[i];
    if (!drawBatch_.empty()) renderer.DrawTrails(drawBatch_.data(),
                                                 static_cast<uint32_t>(drawBatch_.size()));
}

void TrailSystem::Clear() {
    for (size_t i = 0; i < trails_.size(); ++i)
        if (trails_[i].count > 0) freeSlots_.push_back(static_cast<uint32_t>(i + 1));
    trails_.clear();
    freeSlots_.clear();
}

} // namespace neon::gfx
