#include "neon/gfx/trail.hpp"

namespace neon::gfx {

namespace {
constexpr size_t kMaxPoints = 64; // per trail; oldest dropped beyond this
}

uint32_t TrailSystem::Create(float width, const Color& head, const Color& tail, float pointLife) {
    Trail t;
    t.width = width;
    t.head = head;
    t.tail = tail;
    t.pointLife = pointLife;
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
    t.pts.push_back(p);
    t.ages.push_back(0.0f);
    if (t.pts.size() > kMaxPoints) {
        t.pts.erase(t.pts.begin());
        t.ages.erase(t.ages.begin());
    }
}

void TrailSystem::End(uint32_t id) {
    if (id == 0 || id > trails_.size()) return;
    trails_[id - 1].ended = true;
}

void TrailSystem::Update(float dt) {
    for (size_t i = 0; i < trails_.size(); ++i) {
        Trail& t = trails_[i];
        if (t.pts.empty()) {
            // Idle slot: recycle when it has already ended (or was never used).
            if (t.ended) {
                freeSlots_.push_back(static_cast<uint32_t>(i + 1));
                t = Trail{};
            }
            continue;
        }
        for (float& a : t.ages) a += dt;
        // Drop expired points (they are all oldest-first).
        size_t drop = 0;
        while (drop < t.ages.size() && t.ages[drop] > t.pointLife) ++drop;
        if (drop > 0) {
            t.pts.erase(t.pts.begin(), t.pts.begin() + static_cast<long>(drop));
            t.ages.erase(t.ages.begin(), t.ages.begin() + static_cast<long>(drop));
        }
        if (t.ended && t.pts.empty()) {
            freeSlots_.push_back(static_cast<uint32_t>(i + 1));
            t = Trail{};
        }
    }
}

void TrailSystem::Draw(Renderer& renderer) const {
    for (const Trail& t : trails_) {
        if (t.pts.size() < 2) continue;
        renderer.DrawTrail(t.pts.data(), static_cast<uint32_t>(t.pts.size()), t.width, t.head,
                           t.tail);
    }
}

void TrailSystem::Clear() {
    for (size_t i = 0; i < trails_.size(); ++i)
        if (!trails_[i].pts.empty()) freeSlots_.push_back(static_cast<uint32_t>(i + 1));
    trails_.clear();
    freeSlots_.clear();
}

} // namespace neon::gfx
