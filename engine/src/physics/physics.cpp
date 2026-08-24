#include "neon/physics/physics.hpp"

#include <algorithm>
#include <cmath>

namespace neon::physics {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float ShapeVolume(bool sphere, float radius, const math::Vec3& halfExtents) {
    if (sphere) return 4.0f / 3.0f * kPi * radius * radius * radius;
    return 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
}

// Closest point on an AABB to p.
math::Vec3 ClosestPointOnBox(const math::AABB& box, const math::Vec3& p) {
    return {std::fmax(box.min.x, std::fmin(p.x, box.max.x)),
            std::fmax(box.min.y, std::fmin(p.y, box.max.y)),
            std::fmax(box.min.z, std::fmin(p.z, box.max.z))};
}

// AABB vs AABB: contact normal points from A to B; penetration > 0 when
// overlapping. The axis with the smallest overlap wins (min translation).
bool BoxBoxContact(const math::AABB& A, const math::AABB& B, math::Vec3& normal,
                   float& penetration) {
    if (!A.Intersects(B)) return false;

    const math::Vec3 centerA = A.Center();
    const math::Vec3 centerB = B.Center();
    const math::Vec3 delta = centerB - centerA;

    const float overlapX = A.Extents().x + B.Extents().x - std::fabs(delta.x);
    const float overlapY = A.Extents().y + B.Extents().y - std::fabs(delta.y);
    const float overlapZ = A.Extents().z + B.Extents().z - std::fabs(delta.z);

    if (overlapX <= 0.0f || overlapY <= 0.0f || overlapZ <= 0.0f) return false;

    if (overlapX < overlapY && overlapX < overlapZ) {
        normal = {delta.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
        penetration = overlapX;
    } else if (overlapY < overlapZ) {
        normal = {0.0f, delta.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
        penetration = overlapY;
    } else {
        normal = {0.0f, 0.0f, delta.z >= 0.0f ? 1.0f : -1.0f};
        penetration = overlapZ;
    }
    return true;
}

// Sphere vs AABB: normal from the box toward the sphere center.
bool SphereBoxContact(const math::Vec3& spherePos, float radius, const math::AABB& box,
                      math::Vec3& normal, float& penetration) {
    const math::Vec3 closest = ClosestPointOnBox(box, spherePos);
    const math::Vec3 delta = spherePos - closest;
    const float distSq = delta.LengthSq();
    if (distSq >= radius * radius) return false;
    const float dist = std::sqrt(distSq);
    if (dist > 1e-6f) {
        normal = delta / dist;
        penetration = radius - dist;
    } else {
        // Center inside the box: push out along the axis of least penetration.
        const float left = spherePos.x - box.min.x;
        const float right = box.max.x - spherePos.x;
        const float bottom = spherePos.y - box.min.y;
        const float top = box.max.y - spherePos.y;
        const float back = spherePos.z - box.min.z;
        const float front = box.max.z - spherePos.z;
        const float m = std::fmin(std::fmin(left, right), std::fmin(std::fmin(bottom, top),
                                                                    std::fmin(back, front)));
        if (m == left) { normal = {-1, 0, 0}; penetration = radius + left; }
        else if (m == right) { normal = {1, 0, 0}; penetration = radius + right; }
        else if (m == bottom) { normal = {0, -1, 0}; penetration = radius + bottom; }
        else if (m == top) { normal = {0, 1, 0}; penetration = radius + top; }
        else if (m == back) { normal = {0, 0, -1}; penetration = radius + back; }
        else { normal = {0, 0, 1}; penetration = radius + front; }
    }
    return true;
}

} // namespace

World::BodyId World::AddSphere(uint64_t owner, const math::Vec3& pos, float radius,
                               bool dynamic, const RigidBodyDesc& desc) {
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
    b.kind = Body::Kind::Sphere;
    b.pos = pos;
    b.radius = std::fmax(radius, 1e-4f);
    InitBody(b, owner, dynamic, desc);
    return id;
}

World::BodyId World::AddBox(uint64_t owner, const math::Vec3& center,
                            const math::Vec3& halfExtents, bool dynamic,
                            const RigidBodyDesc& desc) {
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
    b.kind = Body::Kind::Box;
    b.pos = center;
    b.halfExtents = {std::fmax(halfExtents.x, 1e-4f), std::fmax(halfExtents.y, 1e-4f),
                     std::fmax(halfExtents.z, 1e-4f)};
    InitBody(b, owner, dynamic, desc);
    return id;
}

World::BodyId World::AddBox(uint64_t owner, const math::AABB& box, bool dynamic,
                            const RigidBodyDesc& desc) {
    return AddBox(owner, box.Center(), box.Extents(), dynamic, desc);
}

World::BodyId World::AddCharacter(uint64_t owner, const math::Vec3& pos, float radius,
                                  float halfHeight, const RigidBodyDesc& desc) {
    // The deterministic custom world has no kinematic character controller;
    // only the Jolt backend implements characters.
    (void)owner;
    (void)pos;
    (void)radius;
    (void)halfHeight;
    (void)desc;
    return {};
}

void World::SetCharacterMove(BodyId body, const math::Vec3& move) {
    (void)body;
    (void)move;
}

math::Vec3 World::GetCharacterMove(BodyId body) const {
    (void)body;
    return {};
}

void World::InitBody(Body& b, uint64_t owner, bool dynamic, const RigidBodyDesc& desc) {
    b.owner = owner;
    b.dynamic = dynamic;
    b.layer = desc.layer;
    b.mask = desc.mask;
    b.restitution = math::Clamp(desc.restitution, 0.0f, 1.0f);
    b.friction = std::fmax(desc.friction, 0.0f);
    b.linearDamping = std::fmax(desc.linearDamping, 0.0f);
    b.gravityScale = std::fmax(desc.gravityScale, 0.0f);
    if (dynamic) {
        b.mass = desc.mass > 0.0f
                     ? desc.mass
                     : ShapeVolume(b.kind == Body::Kind::Sphere, b.radius, b.halfExtents);
        b.invMass = 1.0f / std::fmax(b.mass, 1e-6f);
    } else {
        b.mass = 0.0f;
        b.invMass = 0.0f;
    }
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

void World::SetMass(BodyId body, float mass) {
    Body* b = Find(body);
    if (!b) return;
    if (b->dynamic) {
        b->mass = std::fmax(mass, 1e-4f);
        b->invMass = 1.0f / b->mass;
    }
}

void World::SetRestitution(BodyId body, float restitution) {
    if (Body* b = Find(body)) b->restitution = math::Clamp(restitution, 0.0f, 1.0f);
}

void World::SetFriction(BodyId body, float friction) {
    if (Body* b = Find(body)) b->friction = std::fmax(friction, 0.0f);
}

void World::SetLinearDamping(BodyId body, float damping) {
    if (Body* b = Find(body)) b->linearDamping = std::fmax(damping, 0.0f);
}

void World::SetGravityScale(BodyId body, float scale) {
    if (Body* b = Find(body)) b->gravityScale = std::fmax(scale, 0.0f);
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
    dt = std::fmax(dt, 0.0f);

    // Integrate dynamic bodies: gravity (per-body scale), exponential damping,
    // then position. Deterministic: bodies integrate in insertion order.
    for (Body& b : bodies_) {
        if (!b.enabled || !b.dynamic) continue;
        b.onGround = false;
        b.velocity += gravity * (dt * b.gravityScale);
        if (b.linearDamping > 0.0f) {
            const float decay = 1.0f / (1.0f + b.linearDamping * dt);
            b.velocity.x *= decay;
            b.velocity.y *= decay;
            b.velocity.z *= decay;
        }
        b.pos += b.velocity * dt;
    }

    // Ground plane (y = 0): clamp the shape bottom; reflect the downward
    // velocity by the body's restitution (small impacts rest instead of
    // micro-bouncing).
    for (Body& b : bodies_) {
        if (!b.enabled || !b.dynamic) continue;
        if (b.Bottom() < 0.0f) {
            b.pos.y -= b.Bottom();
            if (b.velocity.y < 0.0f) {
                if (b.restitution > 0.0f && -b.velocity.y > 0.3f) {
                    b.velocity.y = -b.velocity.y * b.restitution;
                } else {
                    b.velocity.y = 0.0f;
                }
            }
            b.onGround = true;
        }
    }

    // Collision detection + resolution for every pair once (i < j). Static
    // pairs are skipped; static bodies never move or receive impulses.
    for (size_t i = 0; i < bodies_.size(); ++i) {
        Body& a = bodies_[i];
        if (!a.enabled) continue;
        for (size_t j = i + 1; j < bodies_.size(); ++j) {
            Body& b = bodies_[j];
            if (!b.enabled || (!a.dynamic && !b.dynamic)) continue;
            SolvePair(a, b);
        }
    }

    // Refresh ground flags after resolution (box tops are not "ground", but a
    // body pressed onto the y=0 plane by another body still counts).
    for (Body& b : bodies_) {
        if (b.enabled && b.dynamic && b.Bottom() <= 0.001f) b.onGround = true;
    }
}

void World::SolvePair(Body& a, Body& b) {
    // Collision layer/mask filter (Bullet-style): both directions must match.
    if ((a.mask & b.layer) == 0 || (b.mask & a.layer) == 0) return;
    math::Vec3 normal{0, 1, 0};
    float penetration = 0.0f;
    bool contact = false;

    if (a.kind == Body::Kind::Sphere && b.kind == Body::Kind::Sphere) {
        const math::Vec3 delta = b.pos - a.pos;
        const float distSq = delta.LengthSq();
        const float minDist = a.radius + b.radius;
        if (distSq < minDist * minDist) {
            const float dist = std::sqrt(distSq);
            if (dist > 1e-6f) {
                normal = delta / dist;
            } else {
                normal = {0, 1, 0};
            }
            penetration = minDist - dist;
            contact = true;
        }
    } else if (a.kind == Body::Kind::Sphere && b.kind == Body::Kind::Box) {
        contact = SphereBoxContact(a.pos, a.radius, b.Box(), normal, penetration);
        // SphereBoxContact returns box->sphere; flip to a->b.
        normal = -normal;
    } else if (a.kind == Body::Kind::Box && b.kind == Body::Kind::Sphere) {
        // SphereBoxContact(sphere=b, box=a) already returns box(a)->sphere(b).
        contact = SphereBoxContact(b.pos, b.radius, a.Box(), normal, penetration);
    } else {
        contact = BoxBoxContact(a.Box(), b.Box(), normal, penetration);
    }
    if (!contact || penetration <= 0.0f) return;

    // Full positional correction (mass weighted). Slop-free so resting contact
    // is exact and stable at the fixed-step resolution used by the runtime.
    const float totalInv = a.invMass + b.invMass;
    if (totalInv > 1e-8f) {
        a.pos -= normal * (penetration * (a.invMass / totalInv));
        b.pos += normal * (penetration * (b.invMass / totalInv));
    }

    // Normal impulse: j = -(1+e) * vn / (invMa + invMb); then the tangent
    // impulse for friction, clamped by Coulomb's law.
    const math::Vec3 relVel = b.velocity - a.velocity;
    const float vn = math::Dot(relVel, normal);
    if (vn < 0.0f) {
        const float e = a.restitution * b.restitution; // combined restitution
        const float jn = -(1.0f + e) * vn / totalInv;
        const math::Vec3 tangent = (relVel - normal * vn).Normalized();
        float jt = 0.0f;
        if (tangent.LengthSq() > 1e-6f) {
            const float vt = math::Dot(relVel, tangent);
            jt = -vt / totalInv;
            const float maxFriction = jn * std::fmax(a.friction, b.friction);
            jt = math::Clamp(jt, -maxFriction, maxFriction);
            a.velocity -= tangent * (jt * a.invMass);
            b.velocity += tangent * (jt * b.invMass);
        }
        a.velocity -= normal * (jn * a.invMass);
        b.velocity += normal * (jn * b.invMass);
    }

    // Collision event: dynamic owner first (old API contract), insertion order
    // preserved when both are dynamic.
    if (a.dynamic && !b.dynamic) {
        collisions_.emplace_back(a.owner, b.owner);
    } else if (!a.dynamic && b.dynamic) {
        collisions_.emplace_back(b.owner, a.owner);
    } else {
        collisions_.emplace_back(a.owner, b.owner);
    }
}

bool World::Raycast(const math::Ray& ray, float maxDist, float& outT,
                    uint64_t* hitOwner) const {
    float best = maxDist;
    uint64_t bestOwner = 0;
    bool hit = false;
    for (const Body& b : bodies_) {
        if (!b.enabled) continue;
        float t = 0.0f;
        if (b.kind == Body::Kind::Sphere) {
            if (!math::IntersectRaySphere(ray, b.pos, b.radius, t)) continue;
        } else {
            if (!math::IntersectRayAABB(ray, b.Box(), t)) continue;
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

std::vector<World::DebugBody> World::DebugBodies() const {
    std::vector<DebugBody> out;
    out.reserve(bodies_.size());
    for (const Body& b : bodies_) {
        if (!b.enabled) continue;
        DebugBody d;
        d.kind = b.kind == Body::Kind::Sphere ? ShapeKind::Sphere : ShapeKind::Box;
        d.pos = b.pos;
        d.radius = b.radius;
        d.halfExtents = b.halfExtents;
        d.dynamic = b.dynamic;
        out.push_back(d);
    }
    return out;
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
