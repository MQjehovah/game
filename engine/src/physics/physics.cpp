#include "neon/physics/physics.hpp"

#include <algorithm>
#include <cmath>

namespace neon::physics {

World::BodyId World::AddSphere(uint64_t owner, const math::Vec3& pos, float radius, bool dynamic) {
    BodyId id;
    if (freeIds_.empty()) {
        id = {nextId_++};
    } else {
        id = freeIds_.back();
        freeIds_.pop_back();
    }
    if (id.id > bodies_.size()) bodies_.resize(id.id);
    Body& b = bodies_[id.id - 1];
    b = Body{};
    b.id = id;
    b.owner = owner;
    b.kind = Body::Kind::Sphere;
    b.pos = pos;
    b.radius = radius;
    b.dynamic = dynamic;
    return id;
}

World::BodyId World::AddBox(uint64_t owner, const math::AABB& box, bool dynamic) {
    BodyId id;
    if (freeIds_.empty()) {
        id = {nextId_++};
    } else {
        id = freeIds_.back();
        freeIds_.pop_back();
    }
    if (id.id > bodies_.size()) bodies_.resize(id.id);
    Body& b = bodies_[id.id - 1];
    b = Body{};
    b.id = id;
    b.owner = owner;
    b.kind = Body::Kind::Box;
    b.pos = box.Center();
    b.box = box;
    b.dynamic = dynamic;
    return id;
}

void World::Remove(BodyId body) {
    Body* b = Find(body);
    if (!b) return;
    b->enabled = false;
    freeIds_.push_back(body);
}

void World::Clear() {
    bodies_.clear();
    freeIds_.clear();
    nextId_ = 1;
    collisions_.clear();
}

size_t World::BodyCount() const {
    size_t n = 0;
    for (const Body& b : bodies_) {
        if (b.enabled && b.id.Valid()) ++n;
    }
    return n;
}

void World::SetPosition(BodyId body, const math::Vec3& pos) {
    if (Body* b = Find(body)) b->pos = pos;
}

math::Vec3 World::GetPosition(BodyId body) const {
    const Body* b = Find(body);
    return b ? b->pos : math::Vec3{};
}

void World::SetVelocity(BodyId body, const math::Vec3& vel) {
    if (Body* b = Find(body)) b->velocity = vel;
}

math::Vec3 World::GetVelocity(BodyId body) const {
    const Body* b = Find(body);
    return b ? b->velocity : math::Vec3{};
}

void World::SetEnabled(BodyId body, bool enabled) {
    if (Body* b = Find(body)) b->enabled = enabled;
}

bool World::IsOnGround(BodyId body) const {
    const Body* b = Find(body);
    return b && b->onGround;
}

void World::Step(float dt, const math::Vec3& gravity) {
    collisions_.clear();

    // Integrate dynamic spheres.
    for (Body& b : bodies_) {
        if (!b.enabled || !b.dynamic || b.kind != Body::Kind::Sphere) continue;
        b.onGround = false;
        b.velocity += gravity * dt;
        b.pos += b.velocity * dt;
        if (b.pos.y < b.radius) {
            b.pos.y = b.radius;
            if (b.velocity.y < 0.0f) b.velocity.y = 0.0f;
            b.onGround = true;
        }
    }

    // Dynamic spheres vs static boxes.
    for (Body& d : bodies_) {
        if (!d.enabled || !d.dynamic || d.kind != Body::Kind::Sphere) continue;
        for (Body& s : bodies_) {
            if (!s.enabled || s.dynamic || s.kind != Body::Kind::Box) continue;
            math::Vec3 closest = math::Clamp(d.pos, s.box.min, s.box.max);
            math::Vec3 delta = d.pos - closest;
            float distSq = delta.LengthSq();
            if (distSq >= d.radius * d.radius) continue;
            float dist = std::sqrt(distSq);
            math::Vec3 normal = dist > 1e-6f ? delta / dist : math::Vec3{0, 1, 0};
            float penetration = d.radius - dist;
            d.pos += normal * penetration;
            float vn = math::Dot(d.velocity, normal);
            if (vn < 0.0f) d.velocity -= normal * vn;
            collisions_.emplace_back(d.owner, s.owner);
        }
    }

    // Dynamic spheres vs dynamic spheres.
    for (size_t i = 0; i < bodies_.size(); ++i) {
        Body& a = bodies_[i];
        if (!a.enabled || !a.dynamic || a.kind != Body::Kind::Sphere) continue;
        for (size_t j = i + 1; j < bodies_.size(); ++j) {
            Body& b = bodies_[j];
            if (!b.enabled || !b.dynamic || b.kind != Body::Kind::Sphere) continue;
            math::Vec3 delta = b.pos - a.pos;
            float dist = delta.Length();
            float minDist = a.radius + b.radius;
            if (dist >= minDist || dist < 1e-6f) continue;
            math::Vec3 normal = delta / dist;
            float overlap = minDist - dist;
            a.pos -= normal * (overlap * 0.5f);
            b.pos += normal * (overlap * 0.5f);
            collisions_.emplace_back(a.owner, b.owner);
        }
    }

    // Refresh ground flags after resolution.
    for (Body& b : bodies_) {
        if (b.enabled && b.dynamic && b.kind == Body::Kind::Sphere) {
            if (b.pos.y <= b.radius + 0.01f) b.onGround = true;
        }
    }
}

bool World::Raycast(const math::Ray& ray, float maxDist, float& outT, uint64_t* hitOwner) const {
    float best = maxDist;
    uint64_t bestOwner = 0;
    bool hit = false;
    for (const Body& b : bodies_) {
        if (!b.enabled) continue;
        float t = 0.0f;
        if (b.kind == Body::Kind::Sphere) {
            if (!math::IntersectRaySphere(ray, b.pos, b.radius, t)) continue;
        } else {
            if (!math::IntersectRayAABB(ray, b.box, t)) continue;
        }
        if (t < best) {
            best = t;
            bestOwner = b.owner;
            hit = true;
        }
    }
    outT = best;
    if (hitOwner) *hitOwner = bestOwner;
    return hit;
}

const World::Body* World::Find(BodyId id) const {
    if (!id.Valid() || id.id > bodies_.size()) return nullptr;
    const Body& b = bodies_[id.id - 1];
    return b.enabled && b.id.id == id.id ? &b : nullptr;
}

World::Body* World::Find(BodyId id) {
    if (!id.Valid() || id.id > bodies_.size()) return nullptr;
    Body& b = bodies_[id.id - 1];
    return b.enabled && b.id.id == id.id ? &b : nullptr;
}

} // namespace neon::physics
