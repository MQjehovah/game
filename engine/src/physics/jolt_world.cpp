#include "neon/physics/jolt_world.hpp"

#ifdef NEON_ENABLE_JOLT

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceMask.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterMask.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterMask.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <map>

#include "neon/core/log.hpp"

namespace neon::physics {

namespace {

// Live JoltWorld count for this process. Jolt's type registration is a global
// (JPH::Factory::sInstance): the engine's lifecycle creates a new world while
// an older one may still be alive (GameRuntime::Start replaces physics_), so
// ~Impl must only UnregisterTypes when the LAST world goes away - otherwise
// destroying one world clears the type info the surviving world still needs.
int g_joltWorldCount = 0;

constexpr uint32_t kMaxBodies = 2048;
constexpr uint32_t kMaxBodyPairs = 32768;
constexpr uint32_t kMaxContactConstraints = 8192;
// Per-step scratch: Jolt allocates its contact-constraint and body-pair buffers
// from the temp allocator; with a 60k-contact world that needs ~13MB, so give
// the allocator generous headroom.
constexpr size_t kTempAllocatorBytes = 32u * 1024u * 1024u;

// Layer/mask bits: Jolt's ObjectLayerPairFilterMask splits the 16-bit layer
// into 8 group bits + 8 mask bits (Bullet semantics). The engine's RigidBodyDesc
// allows arbitrary 32-bit layer/mask; the Jolt backend clamps to 8 bits.
constexpr uint32_t kMaxLayerMask = 0xFFu;

JPH::EMotionType ToMotionType(bool dynamic) {
    return dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
}

JPH::Vec3 ToJolt(const math::Vec3& v) { return {v.x, v.y, v.z}; }
math::Vec3 FromJolt(const JPH::RVec3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
JPH::RVec3 ToRVec3(const math::Vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }

// Per-shape inertia diagonal for a given mass (used by SetMass).
JPH::Vec3 InertiaDiagonal(World::ShapeKind kind, float mass, float radius,
                          const math::Vec3& halfExtents) {
    if (kind == World::ShapeKind::Sphere) {
        const float i = 0.4f * mass * radius * radius;
        return {i, i, i};
    }
    const float hx = halfExtents.x, hy = halfExtents.y, hz = halfExtents.z;
    const float m12 = mass / 12.0f;
    return {m12 * (hy * hy + hz * hz) * 4.0f, m12 * (hx * hx + hz * hz) * 4.0f,
            m12 * (hx * hx + hy * hy) * 4.0f};
}

// Accept-everything filters for the virtual character sweep (layer/mask
// filtering happens through the object-layer filters instead).
class AllBodyFilter : public JPH::BodyFilter {
public:
    bool ShouldCollide(const JPH::BodyID&) const override { return true; }
    bool ShouldCollideLocked(const JPH::Body&) const override { return true; }
};

class AllShapeFilter : public JPH::ShapeFilter {
public:
    bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&) const override { return true; }
    bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape*,
                       const JPH::SubShapeID&) const override {
        return true;
    }
};

class ExcludeBodyFilter : public JPH::BodyFilter {
public:
    explicit ExcludeBodyFilter(JPH::BodyID exclude) : exclude_(exclude) {}
    bool ShouldCollide(const JPH::BodyID& inBodyID) const override {
        return inBodyID != exclude_;
    }
    bool ShouldCollideLocked(const JPH::Body& inBody) const override {
        return inBody.GetID() != exclude_;
    }

private:
    JPH::BodyID exclude_;
};

} // namespace

// Contact listener: collects owner pairs for Collisions() and tracks grounded
// bodies (a body rests on something when its contact normal is vertical).
class JoltContactListener : public JPH::ContactListener {
public:
    JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        Record(a, b, manifold.mWorldSpaceNormal);
    }

    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
        Record(a, b, manifold.mWorldSpaceNormal);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair&) override {}

    std::vector<std::pair<uint64_t, uint64_t>> collisions;

    void Reset() {
        collisions.clear();
    }

private:
    void Record(const JPH::Body& a, const JPH::Body& b, const JPH::Vec3& normal) {
        const uint64_t oa = a.GetUserData();
        const uint64_t ob = b.GetUserData();
        if (oa != 0 && ob != 0 && oa != ob) {
            // Dynamic body first (matches the custom world's contract).
            const bool aDynamic = a.GetMotionType() == JPH::EMotionType::Dynamic;
            const bool bDynamic = b.GetMotionType() == JPH::EMotionType::Dynamic;
            if (aDynamic && !bDynamic)
                collisions.push_back({oa, ob});
            else if (bDynamic && !aDynamic)
                collisions.push_back({ob, oa});
            else
                collisions.push_back({oa, ob});
        }
        (void)normal;
    }
};

struct JoltWorld::Impl {
    Impl() {
        if (JPH::Factory::sInstance == nullptr) JPH::Factory::sInstance = new JPH::Factory();
        if (g_joltWorldCount == 0) JPH::RegisterTypes();
        ++g_joltWorldCount;
        // Broadphase: layer 0 = static group 0 (non-moving), layer 1 = the rest.
        bpInterface.ConfigureLayer(JPH::BroadPhaseLayer(0), 1u << 0, 0u);
        bpInterface.ConfigureLayer(JPH::BroadPhaseLayer(1), kMaxLayerMask & ~(1u << 0), 0u);
        vsBpFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterMask>(bpInterface);
        physics.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContactConstraints, bpInterface,
                     *vsBpFilter, layerFilter);
        physics.SetContactListener(&listener);
        AddImplicitGround();
    }

    ~Impl() {
        --g_joltWorldCount;
        if (g_joltWorldCount == 0) JPH::UnregisterTypes();
    }

    void AddImplicitGround() {
        // Implicit y=0 ground plane, matching the custom world's built-in
        // ground: a huge static box whose top sits at y=0. The ground uses
        // group 1 (group 0 is the "no bits" group and can never collide under
        // the Bullet-style group&mask rule) with a full mask; bodies opt out
        // via their own mask.
        JPH::Ref<JPH::BoxShape> groundShape = new JPH::BoxShape(JPH::Vec3(1000.0f, 50.0f, 1000.0f));
        JPH::BodyCreationSettings ground(
            groundShape, JPH::RVec3(0.0f, -50.0f, 0.0f), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            JPH::ObjectLayerPairFilterMask::sGetObjectLayer(1, kMaxLayerMask));
        ground.mRestitution = 0.0f;
        ground.mFriction = 0.4f;
        ground.mLinearDamping = 0.0f;
        JPH::BodyID groundId = physics.GetBodyInterface().CreateAndAddBody(
            ground, JPH::EActivation::DontActivate);
        physics.GetBodyInterface().SetUserData(groundId, 0);
        ++bodyCount;
    }

    JPH::BodyInterface& Bodies() { return physics.GetBodyInterface(); }

    JPH::BodyID Find(BodyId id) const {
        auto it = idMap.find(id.id);
        return it == idMap.end() ? JPH::BodyID() : it->second;
    }

    JPH::BroadPhaseLayerInterfaceMask bpInterface{2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterMask> vsBpFilter;
    JPH::ObjectLayerPairFilterMask layerFilter;
    JPH::PhysicsSystem physics;
    JPH::TempAllocatorImpl tempAllocator{kTempAllocatorBytes};
    JPH::JobSystemSingleThreaded jobSystem{1024};
    JoltContactListener listener;

    std::map<uint32_t, JPH::BodyID> idMap;
    std::map<uint32_t, bool> enabled;       // our BodyId -> active in the system
    std::map<uint32_t, World::ShapeKind> shapes;
    std::map<uint32_t, float> radii;
    std::map<uint32_t, math::Vec3> halfExtents;
    std::map<uint32_t, uint64_t> owners;
    std::map<uint32_t, std::unique_ptr<JPH::CharacterVirtual>> characters;
    std::map<uint32_t, JPH::ObjectLayer> charLayers;
    std::map<uint32_t, math::Vec3> charMove;
    std::map<uint32_t, bool> charOnGround;
    uint32_t nextId = 1;
    size_t bodyCount = 0;
    bool broadphaseDirty = false;
    std::vector<std::pair<uint64_t, uint64_t>> collisions;
};

JoltWorld::JoltWorld() : impl_(std::make_unique<Impl>()) {}
JoltWorld::~JoltWorld() = default;

World::BodyId JoltWorld::AddSphere(uint64_t owner, const math::Vec3& pos, float radius,
                                   bool dynamic, const RigidBodyDesc& desc) {
    if (!impl_ || radius <= 0.0f) return {};
    JPH::Ref<JPH::SphereShape> shape = new JPH::SphereShape(radius);
    JPH::BodyCreationSettings settings(
        shape, ToRVec3(pos), JPH::Quat::sIdentity(), ToMotionType(dynamic),
        JPH::ObjectLayerPairFilterMask::sGetObjectLayer(desc.layer & kMaxLayerMask,
                                                        desc.mask & kMaxLayerMask));
    settings.mRestitution = desc.restitution;
    settings.mFriction = desc.friction;
    settings.mLinearDamping = desc.linearDamping;
    settings.mGravityFactor = desc.gravityScale;
    const float volume = 4.0f / 3.0f * 3.14159265358979323846f * radius * radius * radius;
    if (dynamic && desc.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    } else if (dynamic) {
        // Match the custom world: density 1 -> mass == volume.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = volume;
    }
    JPH::BodyID bid = impl_->Bodies().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (bid == JPH::BodyID()) return {};
    impl_->Bodies().SetUserData(bid, owner);
    const BodyId id{impl_->nextId++};
    impl_->broadphaseDirty = true;
    impl_->idMap[id.id] = bid;
    impl_->enabled[id.id] = true;
    impl_->shapes[id.id] = ShapeKind::Sphere;
    impl_->radii[id.id] = radius;
    impl_->owners[id.id] = owner;
    ++impl_->bodyCount;
    return id;
}

World::BodyId JoltWorld::AddBox(uint64_t owner, const math::Vec3& center,
                                const math::Vec3& halfExtents, bool dynamic,
                                const RigidBodyDesc& desc) {
    if (!impl_ || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
        return {};
    JPH::Ref<JPH::BoxShape> shape = new JPH::BoxShape(ToJolt(halfExtents));
    JPH::BodyCreationSettings settings(
        shape, ToRVec3(center), JPH::Quat::sIdentity(), ToMotionType(dynamic),
        JPH::ObjectLayerPairFilterMask::sGetObjectLayer(desc.layer & kMaxLayerMask,
                                                        desc.mask & kMaxLayerMask));
    settings.mRestitution = desc.restitution;
    settings.mFriction = desc.friction;
    settings.mLinearDamping = desc.linearDamping;
    settings.mGravityFactor = desc.gravityScale;
    const float volume = 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
    if (dynamic && desc.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    } else if (dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = volume;
    }
    JPH::BodyID bid = impl_->Bodies().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (bid == JPH::BodyID()) return {};
    impl_->Bodies().SetUserData(bid, owner);
    const BodyId id{impl_->nextId++};
    impl_->broadphaseDirty = true;
    impl_->idMap[id.id] = bid;
    impl_->enabled[id.id] = true;
    impl_->shapes[id.id] = ShapeKind::Box;
    impl_->halfExtents[id.id] = halfExtents;
    impl_->owners[id.id] = owner;
    ++impl_->bodyCount;
    return id;
}

World::BodyId JoltWorld::AddBox(uint64_t owner, const math::AABB& box, bool dynamic,
                                const RigidBodyDesc& desc) {
    return AddBox(owner, box.Center(), box.Extents(), dynamic, desc);
}

World::BodyId JoltWorld::AddCharacter(uint64_t owner, const math::Vec3& pos, float radius,
                                      float halfHeight, const RigidBodyDesc& desc) {
    if (!impl_ || radius <= 0.0f || halfHeight <= 0.0f) return {};
    JPH::CharacterVirtualSettings settings;
    settings.mShape = new JPH::CapsuleShape(halfHeight, radius);
    settings.mMass = desc.mass > 0.0f ? desc.mass : 70.0f;
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    const JPH::ObjectLayer layer = JPH::ObjectLayerPairFilterMask::sGetObjectLayer(
        desc.layer & kMaxLayerMask, desc.mask & kMaxLayerMask);
    auto character = std::make_unique<JPH::CharacterVirtual>(
        &settings, ToRVec3(pos), JPH::Quat::sIdentity(), owner, &impl_->physics);
    const BodyId id{impl_->nextId++};
    impl_->characters[id.id] = std::move(character);
    impl_->charLayers[id.id] = layer;
    impl_->charMove[id.id] = {};
    impl_->charOnGround[id.id] = false;
    impl_->owners[id.id] = owner;
    impl_->broadphaseDirty = true;
    return id;
}

void JoltWorld::SetCharacterMove(BodyId body, const math::Vec3& move) {
    if (!impl_) return;
    auto it = impl_->charMove.find(body.id);
    if (it != impl_->charMove.end()) it->second = move;
}

math::Vec3 JoltWorld::GetCharacterMove(BodyId body) const {
    if (!impl_) return {};
    auto it = impl_->charMove.find(body.id);
    return it == impl_->charMove.end() ? math::Vec3{} : it->second;
}

void JoltWorld::Remove(BodyId body) {
    if (!impl_) return;
    if (impl_->characters.erase(body.id)) {
        impl_->charLayers.erase(body.id);
        impl_->charMove.erase(body.id);
        impl_->charOnGround.erase(body.id);
        impl_->owners.erase(body.id);
        if (impl_->bodyCount > 0) --impl_->bodyCount;
        return;
    }
    const JPH::BodyID bid = impl_->Find(body);
    if (bid == JPH::BodyID()) return;
    impl_->Bodies().RemoveBody(bid);
    impl_->Bodies().DestroyBody(bid);
    impl_->idMap.erase(body.id);
    impl_->enabled.erase(body.id);
    impl_->shapes.erase(body.id);
    impl_->radii.erase(body.id);
    impl_->halfExtents.erase(body.id);
    impl_->owners.erase(body.id);
    if (impl_->bodyCount > 0) --impl_->bodyCount;
}

void JoltWorld::Clear() {
    if (!impl_) return;
    JPH::BodyIDVector all;
    impl_->physics.GetBodies(all);
    // Jolt requires bodies to leave the broadphase BEFORE they are destroyed
    // (DestroyBody asserts "not in broadphase"); the implicit ground and any
    // scene bodies are still active when Clear runs. Remove them all first,
    // then destroy - Release builds compile the assert out and corrupt state,
    // which shows up later as an access violation.
    impl_->Bodies().RemoveBodies(all.data(), static_cast<int>(all.size()));
    impl_->Bodies().DestroyBodies(all.data(), static_cast<int>(all.size()));
    impl_->idMap.clear();
    impl_->enabled.clear();
    impl_->shapes.clear();
    impl_->radii.clear();
    impl_->halfExtents.clear();
    impl_->owners.clear();
    impl_->characters.clear();
    impl_->charLayers.clear();
    impl_->charMove.clear();
    impl_->charOnGround.clear();
    impl_->bodyCount = 0;
    impl_->collisions.clear();
    impl_->broadphaseDirty = true;
    impl_->AddImplicitGround();
}

void JoltWorld::SetPosition(BodyId body, const math::Vec3& pos) {
    if (!impl_) return;
    auto cit = impl_->characters.find(body.id);
    if (cit != impl_->characters.end()) {
        JPH::CharacterVirtual* c = cit->second.get();
        c->SetPosition(ToRVec3(pos));
        return;
    }
    const JPH::BodyID bid = impl_->Find(body);
    if (bid != JPH::BodyID()) impl_->Bodies().SetPosition(bid, ToRVec3(pos),
                                                          JPH::EActivation::Activate);
}

math::Vec3 JoltWorld::GetPosition(BodyId body) const {
    if (!impl_) return {};
    auto cit = impl_->characters.find(body.id);
    if (cit != impl_->characters.end()) return FromJolt(cit->second->GetPosition());
    const JPH::BodyID bid = impl_->Find(body);
    return bid == JPH::BodyID() ? math::Vec3{} : FromJolt(impl_->Bodies().GetPosition(bid));
}

void JoltWorld::SetVelocity(BodyId body, const math::Vec3& vel) {
    if (!impl_) return;
    auto cit = impl_->characters.find(body.id);
    if (cit != impl_->characters.end()) {
        cit->second->SetLinearVelocity(ToJolt(vel));
        return;
    }
    const JPH::BodyID bid = impl_->Find(body);
    if (bid != JPH::BodyID()) impl_->Bodies().SetLinearVelocity(bid, ToJolt(vel));
}

math::Vec3 JoltWorld::GetVelocity(BodyId body) const {
    if (!impl_) return {};
    auto cit = impl_->characters.find(body.id);
    if (cit != impl_->characters.end()) return FromJolt(cit->second->GetLinearVelocity());
    const JPH::BodyID bid = impl_->Find(body);
    return bid == JPH::BodyID() ? math::Vec3{} : FromJolt(impl_->Bodies().GetLinearVelocity(bid));
}

void JoltWorld::SetMass(BodyId body, float mass) {
    if (!impl_ || mass <= 0.0f) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid == JPH::BodyID()) return;
    JPH::BodyLockWrite lock(impl_->physics.GetBodyLockInterface(), bid);
    if (!lock.Succeeded()) return;
    JPH::Body& b = lock.GetBody();
    if (b.GetMotionType() != JPH::EMotionType::Dynamic) return;
    const auto kind = impl_->shapes.find(body.id);
    const float radius = impl_->radii.count(body.id) ? impl_->radii[body.id] : 1.0f;
    const math::Vec3 he = impl_->halfExtents.count(body.id) ? impl_->halfExtents[body.id]
                                                             : math::Vec3{1, 1, 1};
    const World::ShapeKind sk = kind == impl_->shapes.end() ? World::ShapeKind::Sphere
                                                             : kind->second;
    JPH::MassProperties mp;
    mp.mMass = mass;
    mp.mInertia = JPH::Mat44::sScale(InertiaDiagonal(sk, mass, radius, he));
    b.GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, mp);
}

void JoltWorld::SetRestitution(BodyId body, float restitution) {
    if (!impl_) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid != JPH::BodyID()) impl_->Bodies().SetRestitution(bid, restitution);
}

void JoltWorld::SetFriction(BodyId body, float friction) {
    if (!impl_) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid != JPH::BodyID()) impl_->Bodies().SetFriction(bid, friction);
}

void JoltWorld::SetLinearDamping(BodyId body, float damping) {
    if (!impl_) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid == JPH::BodyID()) return;
    JPH::BodyLockWrite lock(impl_->physics.GetBodyLockInterface(), bid);
    if (!lock.Succeeded()) return;
    JPH::MotionProperties* mp = lock.GetBody().GetMotionProperties();
    if (mp) mp->SetLinearDamping(damping);
}

void JoltWorld::SetGravityScale(BodyId body, float scale) {
    if (!impl_) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid != JPH::BodyID()) impl_->Bodies().SetGravityFactor(bid, scale);
}

void JoltWorld::SetEnabled(BodyId body, bool enabled) {
    if (!impl_) return;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid == JPH::BodyID()) return;
    auto it = impl_->enabled.find(body.id);
    if (it == impl_->enabled.end()) return;
    if (it->second == enabled) return;
    it->second = enabled;
    if (enabled)
        impl_->Bodies().AddBody(bid, JPH::EActivation::Activate);
    else
        impl_->Bodies().RemoveBody(bid);
    impl_->broadphaseDirty = true;
}

bool JoltWorld::IsOnGround(BodyId body) const {
    if (!impl_) return false;
    auto cit = impl_->charOnGround.find(body.id);
    if (cit != impl_->charOnGround.end()) return cit->second;
    const JPH::BodyID bid = impl_->Find(body);
    if (bid == JPH::BodyID()) return false;
    JPH::RVec3 pos;
    {
        JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(), bid);
        if (!lock.Succeeded()) return false;
        pos = lock.GetBody().GetPosition();
    } // Release the body lock before the probe: CastRay takes its own
      // BroadPhaseQuery + PerBody locks, and Jolt asserts when the same thread
      // re-enters a lock of equal priority (deadlock guard).
    // Short downward probe from just above the body's bottom; a hit within the
    // probe distance means the body rests on something. (Contact listeners are
    // unreliable for settled bodies, so this ray probe is the ground truth.)
    float bottom = 0.5f;
    auto shapeIt = impl_->shapes.find(body.id);
    if (shapeIt != impl_->shapes.end() && shapeIt->second == World::ShapeKind::Sphere) {
        bottom = impl_->radii.count(body.id) ? impl_->radii[body.id] : 0.5f;
    } else if (impl_->halfExtents.count(body.id)) {
        bottom = impl_->halfExtents[body.id].y;
    }
    constexpr float kProbe = 0.25f;
    JPH::RRayCast probe(JPH::RVec3(pos.GetX(), pos.GetY() - bottom + 0.05f, pos.GetZ()),
                        JPH::Vec3(0.0f, -1.0f, 0.0f));
    JPH::RayCastResult hit;
    ExcludeBodyFilter exclude(bid);
    const JPH::ObjectLayer rayLayer =
        JPH::ObjectLayerPairFilterMask::sGetObjectLayer(1, kMaxLayerMask);
    JPH::DefaultBroadPhaseLayerFilter bpFilter(*impl_->vsBpFilter, rayLayer);
    JPH::DefaultObjectLayerFilter objFilter(impl_->layerFilter, rayLayer);
    if (!impl_->physics.GetNarrowPhaseQuery().CastRay(probe, hit, bpFilter, objFilter, exclude))
        return false;
    // The probe ray is unit-length; any hit within ~0.35 below the body's
    // bottom means it rests on something.
    return hit.mFraction < kProbe + 0.1f;
}

void JoltWorld::Step(float dt, const math::Vec3& gravity) {
    if (!impl_ || dt <= 0.0f) return;
    const JPH::Vec3 jgravity = ToJolt(gravity);
    impl_->listener.Reset();
    impl_->physics.SetGravity(jgravity);
    // Static bodies only become visible to ray / shape queries after the
    // broadphase query tree is rebuilt (OptimizeBroadPhase). Rebuild lazily
    // whenever a body was added since the last step so runtime raycasts are
    // always correct without paying the cost every frame.
    if (impl_->broadphaseDirty) {
        impl_->physics.OptimizeBroadPhase();
        impl_->broadphaseDirty = false;
    }
    impl_->physics.Update(dt, 1, &impl_->tempAllocator, &impl_->jobSystem);
    // Virtual characters: drive them with the requested velocity, then update
    // ground state from the sweep results.
    for (auto& kv : impl_->characters) {
        JPH::CharacterVirtual* c = kv.second.get();
        const JPH::ObjectLayer layer = impl_->charLayers[kv.first];
        c->SetLinearVelocity(ToJolt(impl_->charMove[kv.first]));
        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mStickToFloorStepDown = {0.0f, -0.5f, 0.0f};
        settings.mWalkStairsStepUp = {0.0f, 0.4f, 0.0f};
        JPH::DefaultBroadPhaseLayerFilter bpFilter(*impl_->vsBpFilter, layer);
        JPH::DefaultObjectLayerFilter objFilter(impl_->layerFilter, layer);
        AllBodyFilter bodyFilter;
        AllShapeFilter shapeFilter;
        c->ExtendedUpdate(dt, jgravity, settings, bpFilter, objFilter, bodyFilter, shapeFilter,
                          impl_->tempAllocator);
        impl_->charOnGround[kv.first] =
            c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    }
    // The interface contract matches custom World::Step: Collisions() returns
    // the pairs of THIS step (custom world clears at the start of every Step,
    // physics.cpp:250). Appending here used to grow without bound since no
    // caller ever invoked ClearCollisions() (A7).
    impl_->collisions = impl_->listener.collisions;
    collisions_ = std::move(impl_->collisions);
}

size_t JoltWorld::BodyCount() const {
    return impl_ ? impl_->bodyCount + impl_->characters.size() : 0;
}

bool JoltWorld::Raycast(const math::Ray& ray, float maxDist, float& outT,
                        uint64_t* hitOwner) const {
    if (!impl_ || maxDist <= 0.0f) return false;
    // Rebuild the broadphase query tree first if bodies were added since the
    // last step (raycasts are often issued outside a Step).
    if (impl_->broadphaseDirty) {
        impl_->physics.OptimizeBroadPhase();
        impl_->broadphaseDirty = false;
    }
    // Jolt ray lengths are the direction vector: fraction 1 == origin + dir.
    // Scale the engine's normalized direction by maxDist so outT maps back.
    JPH::RRayCast jray(ToRVec3(ray.origin), ToJolt(ray.dir) * maxDist);
    // The ray is treated as an object on group 1 with a full mask so it can
    // hit every body that collides with group 1.
    const JPH::ObjectLayer rayLayer =
        JPH::ObjectLayerPairFilterMask::sGetObjectLayer(1, kMaxLayerMask);
    JPH::DefaultBroadPhaseLayerFilter bpFilter(*impl_->vsBpFilter, rayLayer);
    JPH::DefaultObjectLayerFilter objFilter(impl_->layerFilter, rayLayer);
    AllBodyFilter bodyFilter;
    JPH::RayCastResult hit;
    if (!impl_->physics.GetNarrowPhaseQuery().CastRay(jray, hit, bpFilter, objFilter,
                                                      bodyFilter))
        return false;
    JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded()) return false;
    outT = hit.mFraction * maxDist;
    if (hitOwner) *hitOwner = lock.GetBody().GetUserData();
    return true;
}

std::vector<World::DebugBody> JoltWorld::DebugBodies() const {
    std::vector<World::DebugBody> out;
    if (!impl_) return out;
    for (const auto& kv : impl_->characters) {
        World::DebugBody db;
        db.kind = World::ShapeKind::Sphere;
        db.pos = FromJolt(kv.second->GetPosition());
        db.radius = 0.5f;
        db.dynamic = true;
        out.push_back(db);
    }
    for (const auto& kv : impl_->idMap) {
        const JPH::BodyID bid = kv.second;
        JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(), bid);
        if (!lock.Succeeded()) continue;
        const JPH::Body& b = lock.GetBody();
        const math::Vec3 pos = FromJolt(b.GetPosition());
        const bool dynamic = b.GetMotionType() == JPH::EMotionType::Dynamic;
        auto shapeIt = impl_->shapes.find(kv.first);
        if (shapeIt != impl_->shapes.end() && shapeIt->second == World::ShapeKind::Sphere) {
            World::DebugBody db;
            db.kind = World::ShapeKind::Sphere;
            db.pos = pos;
            db.radius = impl_->radii.count(kv.first) ? impl_->radii[kv.first] : 1.0f;
            db.dynamic = dynamic;
            out.push_back(db);
        } else {
            World::DebugBody db;
            db.kind = World::ShapeKind::Box;
            db.pos = pos;
            db.halfExtents = impl_->halfExtents.count(kv.first)
                                 ? impl_->halfExtents[kv.first]
                                 : math::Vec3{1, 1, 1};
            db.dynamic = dynamic;
            out.push_back(db);
        }
    }
    return out;
}

} // namespace neon::physics

#endif // NEON_ENABLE_JOLT
