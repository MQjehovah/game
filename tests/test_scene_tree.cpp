#include <string>

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Builds the scene JSON for a hierarchy:
//   root (1,2,3) -> child (10,0,0) -> grand (0,5,0)
//   root -> deep0 (1,0,0) -> deep1 ... -> deep11   (13 levels total)
std::string HierarchyScene() {
    std::string s = R"({"entities":[
      {"name":"root","components":{"transform":{"pos":[1,2,3]}}},
      {"name":"child","components":{"transform":{"pos":[10,0,0],"parent":"root"}}},
      {"name":"grand","components":{"transform":{"pos":[0,5,0],"parent":"child"}}})";
    for (int i = 0; i < 12; ++i) {
        s += R"(,{"name":"deep)" + std::to_string(i) + R"(","components":{"transform":{"pos":[1,0,0],"parent":")";
        s += (i == 0 ? std::string("root") : std::string("deep") + std::to_string(i - 1));
        s += "\"}}}";
    }
    s += "]}";
    return s;
}

// Column-vector convention: the translation is the last column of the
// row-major matrix (m[3], m[7], m[11]).
math::Vec3 TranslationOf(const math::Mat4& m) { return {m.m[3], m.m[7], m.m[11]}; }

} // namespace

// Children / descendants traversal matches the authored tree.
TEST(SceneTreeTraversal) {
    scene::GameRuntime runtime;
    CHECK(runtime.Start(HierarchyScene(), scene::GameRuntimeConfig{}).Ok());

    const ecs::Entity root = runtime.FindNamedEntity("root");
    const ecs::Entity child = runtime.FindNamedEntity("child");
    const ecs::Entity grand = runtime.FindNamedEntity("grand");
    CHECK(root.IsValid());
    CHECK(child.IsValid());
    CHECK(grand.IsValid());

    const std::vector<ecs::Entity> children = runtime.GetChildren(root);
    CHECK_EQ(children.size(), 2u); // child + deep0
    bool hasChild = false, hasDeep = false;
    for (const ecs::Entity& e : children) {
        if (e == child) hasChild = true;
        if (e == runtime.FindNamedEntity("deep0")) hasDeep = true;
    }
    CHECK(hasChild);
    CHECK(hasDeep);

    const std::vector<ecs::Entity> desc = runtime.GetDescendants(root);
    CHECK_EQ(desc.size(), 14u); // child, grand, deep0..deep11
    CHECK_EQ(runtime.GetChildren(child).size(), 1u);
    CHECK_EQ(runtime.GetChildren(grand).size(), 0u);
}

// World-transform cache composes parent chains correctly, including a 13-level
// chain that the old 8-level LocalToWorld cap would have truncated.
TEST(SceneTreeWorldTransformCache) {
    scene::GameRuntime runtime;
    CHECK(runtime.Start(HierarchyScene(), scene::GameRuntimeConfig{}).Ok());
    runtime.RebuildWorldTransforms();

    const ecs::Entity root = runtime.FindNamedEntity("root");
    const ecs::Entity child = runtime.FindNamedEntity("child");
    const ecs::Entity grand = runtime.FindNamedEntity("grand");
    const ecs::Entity deep11 = runtime.FindNamedEntity("deep11");

    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(root)).x, 1.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(root)).y, 2.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(child)).x, 11.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(child)).y, 2.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(grand)).x, 11.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(grand)).y, 7.0f, 1e-5f);
    // 1 (root) + 12 * 1 = 13; the old 8-level cap would return 9.
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(deep11)).x, 13.0f, 1e-5f);

    // Mutations are reflected after a rebuild (scripts/tweens/physics mutate
    // between frames; Draw rebuilds once per frame).
    scene::SceneTransform* t = runtime.World().Get<scene::SceneTransform>(child);
    CHECK(t != nullptr);
    t->pos = {100.0f, 0.0f, 0.0f};
    runtime.RebuildWorldTransforms();
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(grand)).x, 101.0f, 1e-5f);
}

// parentId resolution is exact even when two entities share a name (the old
// name-based lookup was ambiguous and could self-parent / cycle).
TEST(SceneTreeParentByIdWithDuplicateNames) {
    const char* scene = R"({"entities":[
      {"id":10,"name":"dup","components":{"transform":{"pos":[0,0,0]}}},
      {"id":20,"name":"dup","components":{"transform":{"pos":[5,0,0],"parentId":10}}}
    ]})";
    scene::GameRuntime runtime;
    CHECK(runtime.Start(scene, scene::GameRuntimeConfig{}).Ok());

    const ecs::Entity first = runtime.FindNamedEntity("dup");
    CHECK(first.IsValid());
    const std::vector<ecs::Entity> children = runtime.GetChildren(first);
    CHECK_EQ(children.size(), 1u);
    // The child's world = parent(0,0,0) + local(5,0,0).
    runtime.RebuildWorldTransforms();
    const ecs::Entity child = children[0];
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(child)).x, 5.0f, 1e-5f);
    CHECK_NEAR(TranslationOf(runtime.CachedLocalToWorld(child)).y, 0.0f, 1e-5f);
}

// A parentId cycle is rejected at Start (no infinite loop, precise error).
TEST(SceneTreeParentIdCycleRejected) {
    const char* scene = R"({"entities":[
      {"id":1,"name":"a","components":{"transform":{"pos":[0,0,0],"parentId":2}}},
      {"id":2,"name":"b","components":{"transform":{"pos":[1,0,0],"parentId":1}}}
    ]})";
    scene::GameRuntime runtime;
    const core::Status st = runtime.Start(scene, scene::GameRuntimeConfig{});
    CHECK(!st.Ok());
    CHECK(st.Error().find("cycle") != std::string::npos);
    CHECK(!runtime.Running());
}

// Self-parenting (parentId == own id) is rejected.
TEST(SceneTreeParentIdSelfRejected) {
    const char* scene = R"({"entities":[
      {"id":1,"name":"a","components":{"transform":{"pos":[0,0,0],"parentId":1}}}
    ]})";
    scene::GameRuntime runtime;
    const core::Status st = runtime.Start(scene, scene::GameRuntimeConfig{});
    CHECK(!st.Ok());
    CHECK(st.Error().find("cycle") != std::string::npos);
}
