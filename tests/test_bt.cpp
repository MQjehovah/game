#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/bt/behavior_tree.hpp"
#include "neon/bt/nodes.hpp"
#include "neon/script/blackboard.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// The exact tree from the T2.5 plan step 1: a condition gating two actions.
const char* kPlanTree = R"({
  "root": {"type":"sequence","children":[
    {"type":"condition","name":"in_range","args":{"distance":5}},
    {"type":"action","name":"move_to","args":{"speed":3}},
    {"type":"action","name":"attack"}
  ]}
})";

} // namespace

// ---------------------------------------------------------------------------
// Plan step 1: sequence gates actions behind a condition
// ---------------------------------------------------------------------------

TEST(BtSequenceConditionGatesActions) {
    script::GameVars gv;
    script::Blackboard bb;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(kPlanTree, &err));
    CHECK(tree.Valid());

    int moves = 0, attacks = 0;
    bool inRange = false;
    bt::Context ctx(gv, &bb);
    ctx.inRange = [&](uint64_t, float) { return inRange; };
    ctx.moveTo = [&](uint64_t, float) { ++moves; return true; };
    ctx.attack = [&](uint64_t) { ++attacks; return true; };

    // Condition not met -> whole tree FAILs and no action runs.
    inRange = false;
    CHECK(tree.Tick(ctx) == bt::Status::Failure);
    CHECK_EQ(moves, 0);
    CHECK_EQ(attacks, 0);

    // Condition met -> actions run in order, tree SUCCEEDS.
    inRange = true;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 1);
    CHECK_EQ(attacks, 1);
}

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------

TEST(BtSelectorShortCircuitAndAllFail) {
    script::GameVars gv;

    bt::BehaviorTree sel;
    std::string err;
    CHECK(sel.LoadText(R"({"root":{"type":"selector","children":[
        {"type":"in_range","args":{"distance":5}},
        {"type":"move_to","args":{"speed":1}}
    ]}})", &err));

    bt::Context ctx(gv, nullptr);
    int moves = 0;
    bool inRange = false;
    ctx.inRange = [&](uint64_t, float) { return inRange; };
    ctx.moveTo = [&](uint64_t, float) { ++moves; return true; };

    inRange = true; // first child succeeds -> short circuit, move_to untouched
    CHECK(sel.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 0);

    inRange = false; // fall through to move_to
    CHECK(sel.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 1);

    bt::BehaviorTree allFail;
    CHECK(allFail.LoadText(R"({"root":{"type":"selector","children":[
        {"type":"in_range","args":{"distance":1}},
        {"type":"attack"}
    ]}})", &err));
    bt::Context ctx2(gv, nullptr);
    bool never = false;
    ctx2.inRange = [&](uint64_t, float) { return never; };
    ctx2.attack = [&](uint64_t) { return false; };
    CHECK(allFail.Tick(ctx2) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// wait
// ---------------------------------------------------------------------------

TEST(BtWaitAccumulatesDt) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"wait","args":{"seconds":1.0}}})", &err));

    // The same tree instance ticks two entities; timers are per-entity in the
    // Context, so entity 2 starts fresh and entity 1's progress is preserved.
    bt::Context ctx(gv, nullptr);
    ctx.entity = 1;
    ctx.dt = 0.3f;
    CHECK(tree.Tick(ctx) == bt::Status::Running); // entity 1: 0.3
    CHECK(tree.Tick(ctx) == bt::Status::Running); // entity 1: 0.6

    ctx.entity = 2; // unaffected by entity 1's timer
    ctx.dt = 1.0f;
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    ctx.entity = 1; // resumes at 0.6, not 0.0
    ctx.dt = 0.5f;
    CHECK(tree.Tick(ctx) == bt::Status::Success); // 0.6 + 0.5 >= 1.0
}

// ---------------------------------------------------------------------------
// invert
// ---------------------------------------------------------------------------

TEST(BtInvertFlipsStatus) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"invert","child":{
        "type":"in_range","args":{"distance":5}}}})", &err));

    bt::Context ctx(gv, nullptr);
    bool inRange = false;
    ctx.inRange = [&](uint64_t, float) { return inRange; };

    inRange = false; // child fails -> inverted Success
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    inRange = true; // child succeeds -> inverted Failure
    CHECK(tree.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// cooldown
// ---------------------------------------------------------------------------

TEST(BtCooldownBlocksChild) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"cooldown","args":{"seconds":1.0},"child":{
        "type":"move_to","args":{"speed":1}}}})", &err));

    // Same tree instance, two entities: the cooldown timer is per entity in
    // the Context, so entity 2 is not blocked by entity 1's cooldown.
    bt::Context ctx(gv, nullptr);
    int moves = 0;
    ctx.moveTo = [&](uint64_t, float) { ++moves; return true; };

    ctx.entity = 1;
    ctx.dt = 0.5f;
    CHECK(tree.Tick(ctx) == bt::Status::Success); // entity 1 runs child
    CHECK_EQ(moves, 1);

    ctx.dt = 0.5f;
    CHECK(tree.Tick(ctx) == bt::Status::Failure); // entity 1 on cooldown
    CHECK_EQ(moves, 1);

    ctx.entity = 2; // independent cooldown: child runs immediately
    ctx.dt = 0.5f;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 2);

    ctx.entity = 1; // remaining cooldown (0.5s) consumed by a 0.5s tick
    ctx.dt = 0.5f;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 3);
}

// ---------------------------------------------------------------------------
// blackboard_set + blackboard_cmp
// ---------------------------------------------------------------------------

TEST(BtBlackboardSetThenCompare) {
    script::GameVars gv;
    script::Blackboard bb;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"sequence","children":[
        {"type":"blackboard_set","args":{"key":"gold","value":100}},
        {"type":"blackboard_cmp","args":{"key":"gold","op":">=","value":50}}
    ]}})", &err));

    bt::Context ctx(gv, &bb);
    ctx.entity = 7;
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    script::Value v = bb.Get(7, "gold");
    CHECK(v.type == script::Value::Type::Number);
    CHECK_EQ(v.number, 100.0);

    // Same key against an unmet bound -> Failure.
    bt::BehaviorTree cmp;
    CHECK(cmp.LoadText(R"({"root":{"type":"blackboard_cmp","args":{"key":"gold","op":">=","value":200}}})", &err));
    CHECK(cmp.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// gamevar_cmp
// ---------------------------------------------------------------------------

TEST(BtGameVarCmp) {
    script::GameVars gv;
    gv.Set("level", script::Value::Num(5));

    bt::Context ctx(gv, nullptr);
    std::string err;

    bt::BehaviorTree gt;
    CHECK(gt.LoadText(R"({"root":{"type":"gamevar_cmp","args":{"key":"level","op":">","value":3}}})", &err));
    CHECK(gt.Tick(ctx) == bt::Status::Success);

    bt::BehaviorTree lt;
    CHECK(lt.LoadText(R"({"root":{"type":"gamevar_cmp","args":{"key":"level","op":"<","value":3}}})", &err));
    CHECK(lt.Tick(ctx) == bt::Status::Failure);

    bt::BehaviorTree missing;
    CHECK(missing.LoadText(R"({"root":{"type":"gamevar_cmp","args":{"key":"nope","op":"==","value":1}}})", &err));
    CHECK(missing.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// health_below
// ---------------------------------------------------------------------------

TEST(BtHealthBelow) {
    script::GameVars gv;
    script::Blackboard bb;
    const uint64_t ent = 42;
    bb.Set(ent, "hp", script::Value::Num(30));
    bb.Set(ent, "maxHp", script::Value::Num(100));

    bt::Context ctx(gv, &bb);
    ctx.entity = ent;
    std::string err;

    bt::BehaviorTree below;
    CHECK(below.LoadText(R"({"root":{"type":"health_below","args":{"pct":50}}})", &err));
    CHECK(below.Tick(ctx) == bt::Status::Success); // 30/100 < 0.50

    bt::BehaviorTree above;
    CHECK(above.LoadText(R"({"root":{"type":"health_below","args":{"pct":20}}})", &err));
    CHECK(above.Tick(ctx) == bt::Status::Failure); // 30/100 >= 0.20
}

// ---------------------------------------------------------------------------
// JSON loading: unknown types error, missing args fall back to defaults
// ---------------------------------------------------------------------------

TEST(BtLoadErrorsAndDefaults) {
    script::GameVars gv;
    std::string err;

    bt::BehaviorTree bad;
    CHECK(!bad.LoadText(R"({"root":{"type":"teleport"}})", &err));
    CHECK(!err.empty());

    // wait with no args: seconds defaults to 0 -> completes immediately
    bt::BehaviorTree wait;
    CHECK(wait.LoadText(R"({"root":{"type":"wait"}})", &err));
    bt::Context ctx(gv, nullptr);
    ctx.dt = 0.25f;
    CHECK(wait.Tick(ctx) == bt::Status::Success);

    // move_to with no speed arg: defaults to 0 and reaches the hook
    bt::BehaviorTree move;
    CHECK(move.LoadText(R"({"root":{"type":"move_to"}})", &err));
    float seen = -1.f;
    ctx.moveTo = [&](uint64_t, float speed) { seen = speed; return true; };
    CHECK(move.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(seen, 0.0f);
}

// ---------------------------------------------------------------------------
// random_selector determinism
// ---------------------------------------------------------------------------

TEST(BtRandomSelectorDeterministic) {
    script::GameVars gv;
    std::string err;

    auto build = [&]() {
        bt::BehaviorTree t;
        CHECK(t.LoadText(R"({"root":{"type":"random_selector","children":[
            {"type":"in_range","name":"a","args":{"distance":1}},
            {"type":"in_range","name":"b","args":{"distance":2}},
            {"type":"in_range","name":"c","args":{"distance":3}}
        ]}})", &err));
        return t;
    };
    bt::BehaviorTree a = build();
    bt::BehaviorTree b = build();

    std::vector<float> orderA, orderB;
    bt::Context ctxA(gv, nullptr);
    ctxA.entity = 99;
    ctxA.inRange = [&](uint64_t, float d) { orderA.push_back(d); return false; };
    bt::Context ctxB(gv, nullptr);
    ctxB.entity = 99;
    ctxB.inRange = [&](uint64_t, float d) { orderB.push_back(d); return false; };

    CHECK(a.Tick(ctxA) == bt::Status::Failure);
    CHECK(b.Tick(ctxB) == bt::Status::Failure);
    CHECK_EQ(orderA.size(), 3u);
    CHECK_EQ(orderA, orderB); // identical trees, same entity seed -> same order
}

// ---------------------------------------------------------------------------
// run_script with a null hook fails gracefully
// ---------------------------------------------------------------------------

TEST(BtRunScriptNullHookFails) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"run_script","args":{"script":"return 42"}}})", &err));

    bt::Context ctx(gv, nullptr);
    CHECK(ctx.callScript == nullptr);
    CHECK(tree.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// spawn
// ---------------------------------------------------------------------------

TEST(BtSpawnCallsHook) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"spawn","args":{"kind":"goblin"}}})", &err));

    bt::Context ctx(gv, nullptr);
    ctx.entity = 5;
    std::string seenKind;
    uint64_t seenEnt = 0;
    ctx.spawn = [&](const std::string& kind, uint64_t ent) {
        seenKind = kind;
        seenEnt = ent;
        return 1u;
    };
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(seenKind, std::string("goblin"));
    CHECK_EQ(seenEnt, 5u);
}

// ---------------------------------------------------------------------------
// parallel
// ---------------------------------------------------------------------------

TEST(BtParallelThreshold) {
    script::GameVars gv;
    std::string err;

    // Two of three children succeed (dialogue hook is null -> Failure).
    // threshold 2 -> enough successes -> Success.
    bt::BehaviorTree tree;
    CHECK(tree.LoadText(R"({"root":{"type":"parallel","args":{"threshold":2},"children":[
        {"type":"move_to","args":{"speed":1}},
        {"type":"attack"},
        {"type":"dialogue","args":{"text":"hello"}}
    ]}})", &err));
    bt::Context ctx(gv, nullptr);
    ctx.moveTo = [&](uint64_t, float) { return true; };
    ctx.attack = [&](uint64_t) { return true; };
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    // threshold 3 with one failure -> cannot reach threshold -> Failure.
    bt::BehaviorTree tree2;
    CHECK(tree2.LoadText(R"({"root":{"type":"parallel","args":{"threshold":3},"children":[
        {"type":"move_to","args":{"speed":1}},
        {"type":"attack"},
        {"type":"dialogue","args":{"text":"hello"}}
    ]}})", &err));
    CHECK(tree2.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// repeat
// ---------------------------------------------------------------------------

TEST(BtRepeatCount) {
    script::GameVars gv;
    std::string err;

    bt::BehaviorTree tree;
    CHECK(tree.LoadText(R"({"root":{"type":"repeat","args":{"count":3},"child":{
        "type":"move_to","args":{"speed":1}}}})", &err));
    bt::Context ctx(gv, nullptr);
    int moves = 0;
    ctx.moveTo = [&](uint64_t, float) { ++moves; return true; };
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 3);

    // Failing child short-circuits.
    bt::BehaviorTree fail;
    CHECK(fail.LoadText(R"({"root":{"type":"repeat","args":{"count":3},"child":{
        "type":"attack"}}})", &err));
    bt::Context ctx2(gv, nullptr);
    int attacks = 0;
    ctx2.attack = [&](uint64_t) { ++attacks; return false; };
    CHECK(fail.Tick(ctx2) == bt::Status::Failure);
    CHECK_EQ(attacks, 1);
}

// ---------------------------------------------------------------------------
// Remaining builtin nodes
// ---------------------------------------------------------------------------

TEST(BtHasTarget) {
    script::GameVars gv;
    script::Blackboard bb;
    const uint64_t ent = 3;
    bt::BehaviorTree has;
    std::string err;
    CHECK(has.LoadText(R"({"root":{"type":"has_target"}})", &err));

    bt::Context ctx(gv, &bb);
    ctx.entity = ent;
    CHECK(has.Tick(ctx) == bt::Status::Failure);
    bb.Set(ent, "target", script::Value::Num(9));
    CHECK(has.Tick(ctx) == bt::Status::Success);
}

TEST(BtQuestState) {
    script::GameVars gv;
    gv.Set("quest_pet", script::Value::Str("started"));

    bt::Context ctx(gv, nullptr);
    std::string err;

    bt::BehaviorTree tree;
    CHECK(tree.LoadText(R"({"root":{"type":"quest_state","args":{"quest":"pet","state":"started"}}})", &err));
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    bt::BehaviorTree other;
    CHECK(other.LoadText(R"({"root":{"type":"quest_state","args":{"quest":"pet","state":"done"}}})", &err));
    CHECK(other.Tick(ctx) == bt::Status::Failure);
}

TEST(BtPlaySfxAndDialogue) {
    script::GameVars gv;
    std::string err;

    bt::BehaviorTree sfx;
    CHECK(sfx.LoadText(R"({"root":{"type":"play_sfx","args":{"name":"hit"}}})", &err));
    bt::Context ctx(gv, nullptr);
    std::string heard;
    ctx.playSfx = [&](const std::string& n) { heard = n; };
    CHECK(sfx.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(heard, std::string("hit"));

    bt::BehaviorTree dl;
    CHECK(dl.LoadText(R"({"root":{"type":"dialogue","args":{"text":"hello"}}})", &err));
    std::string spoken;
    ctx.dialogue = [&](const std::string& t) { spoken = t; };
    CHECK(dl.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(spoken, std::string("hello"));
}

TEST(BtUntilFail) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"until_fail","child":{
        "type":"in_range","args":{"distance":5}}}})", &err));

    bt::Context ctx(gv, nullptr);
    bool inRange = true;
    ctx.inRange = [&](uint64_t, float) { return inRange; };

    CHECK(tree.Tick(ctx) == bt::Status::Running);
    inRange = false;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
}

TEST(BtScriptBool) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"script_bool","args":{"script":"return true"}}})", &err));

    bt::Context ctx(gv, nullptr);
    ctx.callScript = [&](const std::string&, uint64_t) { return script::Value::Bool(true); };
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    ctx.callScript = [&](const std::string&, uint64_t) { return script::Value::Nil(); };
    CHECK(tree.Tick(ctx) == bt::Status::Failure);

    ctx.callScript = nullptr;
    CHECK(tree.Tick(ctx) == bt::Status::Failure);
}

// ---------------------------------------------------------------------------
// Load-time validation
// ---------------------------------------------------------------------------

TEST(BtLoadValidationThreshold) {
    script::GameVars gv;
    std::string err;

    bt::BehaviorTree t;
    CHECK(!t.LoadText(R"({"root":{"type":"parallel","args":{"threshold":0},"children":[
        {"type":"attack"}]}})", &err));
    CHECK(!err.empty());

    err.clear();
    bt::BehaviorTree t2;
    CHECK(!t2.LoadText(R"({"root":{"type":"parallel","args":{"threshold":3},"children":[
        {"type":"attack"},{"type":"attack"}]}})", &err));
    CHECK(!err.empty());
}

TEST(BtLoadValidationStructure) {
    script::GameVars gv;
    std::string err;

    // "children" with the wrong JSON type
    bt::BehaviorTree t1;
    CHECK(!t1.LoadText(R"({"root":{"type":"sequence","children":5}})", &err));
    CHECK(!err.empty());

    // "child" with the wrong JSON type
    err.clear();
    bt::BehaviorTree t2;
    CHECK(!t2.LoadText(R"({"root":{"type":"invert","child":5}})", &err));
    CHECK(!err.empty());

    // "args" with the wrong JSON type
    err.clear();
    bt::BehaviorTree t3;
    CHECK(!t3.LoadText(R"({"root":{"type":"wait","args":42}})", &err));
    CHECK(!err.empty());

    // empty composites are config errors
    err.clear();
    bt::BehaviorTree t4;
    CHECK(!t4.LoadText(R"({"root":{"type":"parallel","children":[]}})", &err));
    CHECK(!err.empty());

    err.clear();
    bt::BehaviorTree t5;
    CHECK(!t5.LoadText(R"({"root":{"type":"sequence"}})", &err));
    CHECK(!err.empty());

    err.clear();
    bt::BehaviorTree t6;
    CHECK(!t6.LoadText(R"({"root":{"type":"selector","children":[]}})", &err));
    CHECK(!err.empty());
}

TEST(BtLoadValidationDeepNesting) {
    script::GameVars gv;
    std::string err;

    // 300 nested invert decorators, deeper than the 256-level cap.
    std::string deep;
    for (int i = 0; i < 300; ++i) deep += R"({"type":"invert","child":)";
    deep += R"({"type":"wait","args":{"seconds":0}})";
    for (int i = 0; i < 300; ++i) deep += "}";
    const std::string json = R"({"root":)" + deep + "}";

    bt::BehaviorTree t;
    CHECK(!t.LoadText(json, &err));
    CHECK(!err.empty());
}

TEST(BtLoadValidationOpsAndCount) {
    script::GameVars gv;
    std::string err;

    // invalid op on blackboard_cmp
    bt::BehaviorTree t1;
    CHECK(!t1.LoadText(R"({"root":{"type":"blackboard_cmp","args":{"key":"x","op":"~~~","value":1}}})", &err));
    CHECK(!err.empty());

    // invalid op on gamevar_cmp
    err.clear();
    bt::BehaviorTree t2;
    CHECK(!t2.LoadText(R"({"root":{"type":"gamevar_cmp","args":{"key":"x","op":"~~","value":1}}})", &err));
    CHECK(!err.empty());

    // negative repeat count
    err.clear();
    bt::BehaviorTree t3;
    CHECK(!t3.LoadText(R"({"root":{"type":"repeat","args":{"count":-1},"child":{
        "type":"wait","args":{"seconds":0}}}})", &err));
    CHECK(!err.empty());

    // repeat without a count
    err.clear();
    bt::BehaviorTree t4;
    CHECK(!t4.LoadText(R"({"root":{"type":"repeat","child":{"type":"wait","args":{"seconds":0}}}})", &err));
    CHECK(!err.empty());
}

TEST(BtLoadValidationCategoryMismatch) {
    script::GameVars gv;
    std::string err;

    // type "condition" but name maps to a composite -> load error
    bt::BehaviorTree t1;
    CHECK(!t1.LoadText(R"({"root":{"type":"condition","name":"sequence","children":[
        {"type":"attack"}]}})", &err));
    CHECK(!err.empty());

    // type "action" but name maps to a condition -> load error
    err.clear();
    bt::BehaviorTree t2;
    CHECK(!t2.LoadText(R"({"root":{"type":"action","name":"in_range","args":{"distance":5}}})", &err));
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// Minor: table values, transactional load, blackboard scoping, shuffle variety
// ---------------------------------------------------------------------------

TEST(BtBlackboardSetTableValue) {
    script::GameVars gv;
    script::Blackboard bb;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"blackboard_set","args":{
        "key":"stats","value":{"hp":10,"mp":5}}}})", &err));

    bt::Context ctx(gv, &bb);
    ctx.entity = 1;
    CHECK(tree.Tick(ctx) == bt::Status::Success);

    script::Value v = bb.Get(1, "stats");
    CHECK(v.type == script::Value::Type::Table);
    CHECK(v.table != nullptr);
    bool foundHp = false, foundMp = false;
    for (const auto& kv : v.table->fields) {
        if (kv.first == "hp") { foundHp = true; CHECK_EQ(kv.second.number, 10.0); }
        if (kv.first == "mp") { foundMp = true; CHECK_EQ(kv.second.number, 5.0); }
    }
    CHECK(foundHp);
    CHECK(foundMp);

    // array value round-trips through JsonToValue
    bt::BehaviorTree arr;
    CHECK(arr.LoadText(R"({"root":{"type":"blackboard_set","args":{
        "key":"tags","value":[1,2,3]}}})", &err));
    CHECK(arr.Tick(ctx) == bt::Status::Success);
    script::Value tags = bb.Get(1, "tags");
    CHECK(tags.type == script::Value::Type::Table);
    CHECK(tags.table != nullptr);
    CHECK_EQ(tags.table->array.size(), 3u);
}

TEST(BtLoadIsTransactional) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;

    CHECK(tree.LoadText(R"({"root":{"type":"wait","args":{"seconds":0.5}}})", &err));
    CHECK(tree.Valid());

    // A failed load must not clobber the previously loaded tree.
    CHECK(!tree.LoadText(R"({"root":{"type":"teleport"}})", &err));
    CHECK(!tree.LoadText(R"({"root":{"type":"parallel","args":{"threshold":9},"children":[
        {"type":"attack"}]}})", &err));
    CHECK(tree.Valid());

    bt::Context ctx(gv, nullptr);
    ctx.dt = 0.6f;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
}

TEST(BtBlackboardEntityScoping) {
    script::GameVars gv;
    script::Blackboard bb;
    bb.Set(10, "hp", script::Value::Num(50));
    CHECK_EQ(bb.Get(10, "hp").number, 50.0);
    CHECK(bb.Has(10, "hp"));
    CHECK(!bb.Has(11, "hp"));
    CHECK(bb.Get(11, "hp").type == script::Value::Type::Nil);
}

TEST(BtRandomSelectorShufflesAndVariesPerEntity) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"random_selector","children":[
        {"type":"in_range","name":"a","args":{"distance":1}},
        {"type":"in_range","name":"b","args":{"distance":2}},
        {"type":"in_range","name":"c","args":{"distance":3}}
    ]}})", &err));

    std::vector<float> natural = {1.f, 2.f, 3.f};

    std::vector<float> orderA;
    bt::Context ctxA(gv, nullptr);
    ctxA.entity = 99;
    ctxA.inRange = [&](uint64_t, float d) { orderA.push_back(d); return false; };
    CHECK(tree.Tick(ctxA) == bt::Status::Failure);
    CHECK(orderA != natural); // actually shuffled, not natural order

    std::vector<float> orderB;
    bt::Context ctxB(gv, nullptr);
    ctxB.entity = 7;
    ctxB.inRange = [&](uint64_t, float d) { orderB.push_back(d); return false; };
    CHECK(tree.Tick(ctxB) == bt::Status::Failure);
    CHECK(orderB != orderA); // different entities -> different orders
}
