// Task 14: BtRuntime standalone unit tests (the behavior-tree subsystem split
// out of GameRuntime). Covers inline-tree attach + per-tick driving +
// blackboard observability (BlackboardValue/ActivePath), the "bt:<name>"
// named-reference path through the injected reader (incl. missing/empty/unsafe
// refs), dead-entity compaction and Clear().
#include <cstdint>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/systems/bt_runtime.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// blackboard_set + blackboard_cmp: writes then reads the entity blackboard.
const char* kBlackboardTree = R"({
  "root":{"type":"sequence","children":[
    {"type":"blackboard_set","args":{"key":"gold","value":100}},
    {"type":"blackboard_cmp","args":{"key":"gold","op":">=","value":50}}
  ]}
})";

} // namespace

TEST(BtRuntimeAttachTickBlackboard) {
    ecs::World world;
    const ecs::Entity ent = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent, scene::SceneBehaviorTree{kBlackboardTree});

    scene::BtRuntime bt;
    bt.Configure({});
    script::ScriptContext ctx;
    scene::BtRuntime::Hosts hosts;
    bt.AttachAll(world, ctx, hosts);

    CHECK_EQ(bt.Count(), 1u);
    CHECK_EQ(bt.Trees().size(), 1u);
    // Nothing ticked yet: no blackboard value, no active path.
    CHECK(bt.ActivePath(ent).empty());
    CHECK(bt.BlackboardValue(ent, "gold").type == script::Value::Type::Nil);
    // No tree attached to a different entity -> Nil / "".
    CHECK(bt.BlackboardValue(ecs::Entity{}, "gold").type == script::Value::Type::Nil);
    CHECK(bt.ActivePath(ecs::Entity{}).empty());

    bt.Tick(0.016f, world, ctx);

    script::Value gold = bt.BlackboardValue(ent, "gold");
    CHECK(gold.type == script::Value::Type::Number);
    CHECK_EQ(gold.number, 100.0);

    bt.Clear();
    CHECK_EQ(bt.Count(), 0u);
    CHECK(bt.ActivePath(ent).empty());
}

TEST(BtRuntimeNamedReferenceResolution) {
    ecs::World world;
    const ecs::Entity ent = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent, scene::SceneBehaviorTree{"bt:guard"});

    // The reader receives the full path (scriptBaseDir + assets/behaviors/...)
    // exactly like GameRuntime::ReadScript; an unknown path returns empty.
    scene::BtRuntime bt;
    std::vector<std::string> seen;
    bt.Configure({"proj", [&seen](const std::string& path) {
                     seen.push_back(path);
                     return path == "proj/assets/behaviors/guard.bt.json"
                                ? std::string(kBlackboardTree)
                                : std::string{};
                 }});
    script::ScriptContext ctx;
    scene::BtRuntime::Hosts hosts;
    bt.AttachAll(world, ctx, hosts);

    CHECK_EQ(bt.Count(), 1u);
    CHECK_EQ(seen.size(), 1u);
    CHECK_EQ(seen[0], std::string("proj/assets/behaviors/guard.bt.json"));

    bt.Tick(0.016f, world, ctx);
    CHECK(bt.BlackboardValue(ent, "gold").type == script::Value::Type::Number);
}

TEST(BtRuntimeNamedReferenceSkipped) {
    ecs::World world;
    // Missing file, empty reference, unsafe name: all three must be skipped.
    const ecs::Entity ent1 = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent1, scene::SceneBehaviorTree{"bt:missing"});
    const ecs::Entity ent2 = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent2, scene::SceneBehaviorTree{"bt:"});
    const ecs::Entity ent3 = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent3, scene::SceneBehaviorTree{"bt:../../secret"});

    scene::BtRuntime bt;
    bt.Configure({"proj", [](const std::string&) { return std::string{}; }});
    script::ScriptContext ctx;
    scene::BtRuntime::Hosts hosts;
    bt.AttachAll(world, ctx, hosts);

    CHECK_EQ(bt.Count(), 0u);
}

TEST(BtRuntimeCompactsDeadEntities) {
    ecs::World world;
    const ecs::Entity ent = world.Create();
    world.Add<scene::SceneBehaviorTree>(ent, scene::SceneBehaviorTree{kBlackboardTree});

    scene::BtRuntime bt;
    bt.Configure({});
    script::ScriptContext ctx;
    scene::BtRuntime::Hosts hosts;
    bt.AttachAll(world, ctx, hosts);
    CHECK_EQ(bt.Count(), 1u);

    // Despawned entity: the tree is skipped and (1/1 > 1/5) compacted away.
    world.Destroy(ent);
    bt.Tick(0.016f, world, ctx);
    CHECK_EQ(bt.Count(), 0u);
}
