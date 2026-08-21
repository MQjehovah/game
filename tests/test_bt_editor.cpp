// Task 4.4: behavior tree visual editor.
//
// Engine side (tested here): node introspection (AllNodeTypes / ChildCapacity
// param schema matching the loader), BehaviorTree::ToJson serialization
// round-trip, and Context::activePath debug bookkeeping.
//
// Editor side: the pure node graph model (editor/src/bt_editor.hpp, ImGui-free
// by design) — add/remove/set-parent/set-arg ops, link validation, tree-path
// ids, graph -> JSON -> graph round-trips and a file save/load round-trip.

#include <fstream>
#include <string>

#include "neon/bt/behavior_tree.hpp"
#include "neon/bt/nodes.hpp"
#include "neon/neon.hpp"
#include "bt_editor.hpp"
#include "helpers.hpp"

using namespace neon;
using namespace neon::editor; // btgraph (the editor's pure BT graph model)

namespace {

// A tree exercising nested composites, category aliases, named nodes and the
// full arg surface the loader reads.
const char* kFixture = R"({
  "root": {"type":"selector","name":"guard","children":[
    {"type":"condition","name":"in_range","args":{"distance":8}},
    {"type":"sequence","children":[
      {"type":"cooldown","args":{"seconds":2},"child":{
         "type":"action","name":"move_to","args":{"speed":5}}},
      {"type":"wait","args":{"seconds":0.5}},
      {"type":"blackboard_cmp","args":{"key":"hp","op":"<=","value":30}}
    ]},
    {"type":"play_sfx","args":{"name":"whoosh"}}
  ]}
})";

core::Json JNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

} // namespace

// ---------------------------------------------------------------------------
// bt introspection
// ---------------------------------------------------------------------------

TEST(BtAllNodeTypesCoverEveryLoaderType) {
    std::vector<bt::NodeTypeInfo> types = bt::AllNodeTypes();
    CHECK_EQ(types.size(), 23u);

    auto find = [&](const std::string& t) -> const bt::NodeTypeInfo* {
        for (const auto& ti : types)
            if (ti.type == t) return &ti;
        return nullptr;
    };

    // Composites.
    for (const char* c : {"sequence", "selector", "random_selector", "parallel"}) {
        const bt::NodeTypeInfo* ti = find(c);
        CHECK(ti != nullptr);
        if (ti) CHECK_EQ(ti->category, std::string("composite"));
    }
    // Decorators.
    for (const char* c : {"invert", "cooldown", "repeat", "until_fail"}) {
        const bt::NodeTypeInfo* ti = find(c);
        CHECK(ti != nullptr);
        if (ti) CHECK_EQ(ti->category, std::string("decorator"));
    }
    // Actions.
    for (const char* c : {"blackboard_set", "move_to", "attack", "dialogue", "spawn", "wait",
                          "play_sfx", "run_script"}) {
        const bt::NodeTypeInfo* ti = find(c);
        CHECK(ti != nullptr);
        if (ti) CHECK_EQ(ti->category, std::string("action"));
    }
    // Conditions.
    for (const char* c : {"in_range", "has_target", "quest_state", "health_below",
                          "blackboard_cmp", "gamevar_cmp", "script_bool"}) {
        const bt::NodeTypeInfo* ti = find(c);
        CHECK(ti != nullptr);
        if (ti) CHECK_EQ(ti->category, std::string("condition"));
    }
}

TEST(BtNodeTypeParamSchemasMatchLoader) {
    std::vector<bt::NodeTypeInfo> types = bt::AllNodeTypes();
    auto find = [&](const std::string& t) -> const bt::NodeTypeInfo* {
        for (const auto& ti : types)
            if (ti.type == t) return &ti;
        return nullptr;
    };
    auto hasParam = [](const bt::NodeTypeInfo& ti, const std::string& name,
                       bt::ParamType want) -> bool {
        for (const auto& p : ti.params)
            if (p.name == name && p.type == want) return true;
        return false;
    };

    const bt::NodeTypeInfo* repeat = find("repeat");
    CHECK(repeat != nullptr);
    if (repeat) {
        CHECK_EQ(repeat->params.size(), 1u);
        CHECK(hasParam(*repeat, "count", bt::ParamType::Number));
        if (!repeat->params.empty())
            CHECK(repeat->params[0].required); // loader fails without a positive count
    }
    const bt::NodeTypeInfo* moveTo = find("move_to");
    if (moveTo) CHECK(hasParam(*moveTo, "speed", bt::ParamType::Number));
    const bt::NodeTypeInfo* bbcmp = find("blackboard_cmp");
    if (bbcmp) {
        CHECK(hasParam(*bbcmp, "key", bt::ParamType::String));
        CHECK(hasParam(*bbcmp, "op", bt::ParamType::String));
        CHECK(hasParam(*bbcmp, "value", bt::ParamType::Object)); // raw JSON scalar/obj/array
    }
    const bt::NodeTypeInfo* quest = find("quest_state");
    if (quest) {
        CHECK(hasParam(*quest, "quest", bt::ParamType::String));
        CHECK(hasParam(*quest, "state", bt::ParamType::String));
    }
    const bt::NodeTypeInfo* parallel = find("parallel");
    if (parallel) CHECK(hasParam(*parallel, "threshold", bt::ParamType::Number));
}

TEST(BtChildCapacity) {
    CHECK_EQ(bt::ChildCapacity("sequence"), -1);
    CHECK_EQ(bt::ChildCapacity("random_selector"), -1);
    CHECK_EQ(bt::ChildCapacity("invert"), 1);
    CHECK_EQ(bt::ChildCapacity("wait"), 0);
    CHECK_EQ(bt::ChildCapacity("attack"), 0);
    CHECK_EQ(bt::ChildCapacity("nope"), -2);
}

// ---------------------------------------------------------------------------
// bt serialization round-trip
// ---------------------------------------------------------------------------

TEST(BtToJsonRoundTrip) {
    bt::BehaviorTree t1;
    std::string err;
    CHECK(t1.LoadText(kFixture, &err));
    CHECK(t1.Valid());

    const std::string json1 = core::JsonWriter::Write(t1.ToJson());
    // The serialized output is itself a loadable tree.
    bt::BehaviorTree t2;
    CHECK(t2.LoadText(json1, &err));
    CHECK(t2.Valid());
    // Reserialize: canonical output must be byte-identical.
    const std::string json2 = core::JsonWriter::Write(t2.ToJson());
    CHECK_EQ(json1, json2);

    // Spot-check the shape: root selector named "guard", nested structure.
    core::Json dom = core::Json::Parse(json1, &err);
    const core::Json* root = dom.Get("root");
    CHECK(root != nullptr);
    if (!root) return;
    CHECK_EQ(root->Get("type")->GetString(), std::string("selector"));
    CHECK_EQ(root->Get("name")->GetString(), std::string("guard"));
    const core::Json* children = root->Get("children");
    CHECK(children != nullptr && children->Size() == 3u);
    if (!children || children->Size() != 3u) return;
    // "condition" alias normalized to the concrete "in_range" type.
    CHECK_EQ(children->At(0)->Get("type")->GetString(), std::string("in_range"));
    CHECK_NEAR(children->At(0)->Get("args")->Get("distance")->GetNumber(), 8.0, 1e-9);
    // The cooldown decorator keeps its single "child".
    const core::Json* seq = children->At(1);
    const core::Json* cdn = seq->Get("children")->At(0);
    CHECK_EQ(cdn->Get("type")->GetString(), std::string("cooldown"));
    CHECK_NEAR(cdn->Get("args")->Get("seconds")->GetNumber(), 2.0, 1e-9);
    CHECK_EQ(cdn->Get("child")->Get("type")->GetString(), std::string("move_to"));
    // The blackboard_cmp `value` survives as a raw number.
    const core::Json* cmp = seq->Get("children")->At(2);
    CHECK_NEAR(cmp->Get("args")->Get("value")->GetNumber(), 30.0, 1e-9);
}

TEST(BtToJsonEmptyTree) {
    bt::BehaviorTree tree;
    core::Json dom = tree.ToJson();
    CHECK(dom.IsObject());
    CHECK(dom.Get("root") == nullptr);
}

TEST(BtToJsonPreservesNodeNamesAndArgs) {
    // A category-typed node with an explicit name must re-emit the concrete
    // type plus its args; a default (name == type) emits no redundant name.
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"action","name":"wait","args":{"seconds":0.25}}})",
                        &err));
    core::Json dom = tree.ToJson();
    const core::Json* root = dom.Get("root");
    CHECK(root != nullptr);
    if (!root) return;
    CHECK_EQ(root->Get("type")->GetString(), std::string("wait"));
    CHECK(root->Get("name") == nullptr); // name == type: not re-emitted
    CHECK_NEAR(root->Get("args")->Get("seconds")->GetNumber(), 0.25, 1e-9);
}

// ---------------------------------------------------------------------------
// Context::activePath debug bookkeeping
// ---------------------------------------------------------------------------

TEST(BtContextActivePathTracksDeepestNode) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    CHECK(tree.LoadText(R"({"root":{"type":"sequence","children":[
        {"type":"condition","name":"in_range","args":{"distance":5}},
        {"type":"action","name":"move_to","args":{"speed":3}},
        {"type":"action","name":"attack"}
    ]}})", &err));

    bt::Context ctx(gv, nullptr);
    bool inRange = false;
    ctx.inRange = [&](uint64_t, float) { return inRange; };
    ctx.moveTo = [&](uint64_t, float) { return true; };
    ctx.attack = [&](uint64_t) { return true; };

    CHECK(ctx.activePath.empty());
    // The first child fails -> the sequence short-circuits there; activePath
    // names the deepest node that actually ran ("0/0" = the in_range child).
    CHECK(tree.Tick(ctx) == bt::Status::Failure);
    CHECK_EQ(ctx.activePath, std::string("0/0"));

    // All children run -> the deepest node that ran is the last action.
    inRange = true;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(ctx.activePath, std::string("0/2"));
}

TEST(BtContextActivePathDecoratorChild) {
    script::GameVars gv;
    bt::BehaviorTree tree;
    std::string err;
    // Nested decorator: root invert "0" -> child wait "0/child".
    CHECK(tree.LoadText(R"({"root":{"type":"invert","child":{
        "type":"wait","args":{"seconds":1.0}}}})", &err));
    bt::Context ctx(gv, nullptr);
    ctx.dt = 0.1f;
    CHECK(tree.Tick(ctx) == bt::Status::Running);
    CHECK_EQ(ctx.activePath, std::string("0/child"));
}

// ---------------------------------------------------------------------------
// editor: graph model ops
// ---------------------------------------------------------------------------

TEST(BtGraphAddLinkSerialize) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{10.f, 20.f});
    const std::string cond = g.AddNode("in_range", math::Vec2{50.f, 60.f});
    const std::string act = g.AddNode("move_to", math::Vec2{90.f, 100.f});
    CHECK(!root.empty() && !cond.empty() && !act.empty());
    CHECK_EQ(g.NodeCount(), 3u);

    CHECK(g.SetParent(cond, root));
    CHECK(g.SetParent(act, root));
    CHECK_EQ(g.LinkCount(), 2u);

    CHECK(g.SetArg(cond, "distance", JNum(8)));
    CHECK(g.SetArg(act, "speed", JNum(3)));

    core::Json tree = g.ToTreeJson();
    const core::Json* r = tree.Get("root");
    CHECK(r != nullptr);
    if (!r) return;
    CHECK_EQ(r->Get("type")->GetString(), std::string("sequence"));
    const core::Json* kids = r->Get("children");
    CHECK(kids != nullptr && kids->Size() == 2u);
    if (!kids || kids->Size() != 2u) return;
    CHECK_EQ(kids->At(0)->Get("type")->GetString(), std::string("in_range"));
    CHECK_NEAR(kids->At(0)->Get("args")->Get("distance")->GetNumber(), 8.0, 1e-9);
    CHECK_EQ(kids->At(1)->Get("type")->GetString(), std::string("move_to"));
}

TEST(BtGraphRoundTrip) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("selector", math::Vec2{});
    const std::string cond = g.AddNode("in_range", math::Vec2{});
    const std::string dec = g.AddNode("cooldown", math::Vec2{});
    const std::string act = g.AddNode("move_to", math::Vec2{});
    g.SetArg(cond, "distance", JNum(8));
    g.SetArg(dec, "seconds", JNum(2));
    g.SetArg(act, "speed", JNum(5));
    g.SetParent(cond, root);
    g.SetParent(dec, root);
    g.SetParent(act, dec);

    const std::string json1 = g.Serialize();

    btgraph::BtGraph g2;
    core::Json dom = core::Json::Parse(json1, nullptr);
    CHECK(g2.FromTreeJson(dom));
    CHECK_EQ(g2.NodeCount(), g.NodeCount());
    CHECK_EQ(g2.LinkCount(), g.LinkCount());
    CHECK_EQ(g2.Serialize(), json1); // graph -> JSON -> graph -> identical JSON
}

TEST(BtGraphInvalidLinksRejected) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    const std::string act = g.AddNode("move_to", math::Vec2{});
    const std::string cond = g.AddNode("in_range", math::Vec2{});
    const std::string dec = g.AddNode("invert", math::Vec2{});
    const std::string dec2 = g.AddNode("cooldown", math::Vec2{});
    const std::string leaf = g.AddNode("wait", math::Vec2{});

    // An action cannot be a parent.
    CHECK(!g.SetParent(cond, act));
    // Self-parenting.
    CHECK(!g.SetParent(root, root));
    // Linking under an unknown node.
    CHECK(!g.SetParent(cond, "ghost"));
    // Unknown child.
    CHECK(!g.SetParent("ghost", root));

    // A decorator holds exactly one child.
    CHECK(g.SetParent(leaf, dec));
    CHECK(!g.SetParent(cond, dec)); // already full
    CHECK_EQ(g.LinkCount(), 1u);

    // Cycles are rejected: root -> dec2 -> cond, then root under cond.
    CHECK(g.SetParent(dec2, root));
    CHECK(g.SetParent(cond, dec2));
    CHECK(!g.SetParent(root, cond)); // root is an ancestor of cond
    CHECK_EQ(g.LinkCount(), 3u);     // unchanged

    // Detaching works: cond becomes a root again.
    CHECK(g.SetParent(cond, ""));
    CHECK_EQ(g.LinkCount(), 2u);
}

TEST(BtGraphRemoveNodeCleansLinks) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    const std::string a = g.AddNode("in_range", math::Vec2{});
    const std::string b = g.AddNode("attack", math::Vec2{});
    g.SetParent(a, root);
    g.SetParent(b, root);
    CHECK_EQ(g.LinkCount(), 2u);
    CHECK(g.RemoveNode(a));
    CHECK(!g.Find(a));
    CHECK_EQ(g.LinkCount(), 1u); // only the root->b link survives
    CHECK(g.RemoveNode(root));
    CHECK_EQ(g.LinkCount(), 0u); // b becomes an unparented root
    CHECK(!g.RemoveNode("ghost"));
}

TEST(BtGraphTreeIdOf) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    const std::string a = g.AddNode("in_range", math::Vec2{});
    const std::string b = g.AddNode("selector", math::Vec2{});
    const std::string c = g.AddNode("wait", math::Vec2{});
    g.SetParent(a, root);
    g.SetParent(b, root);
    g.SetParent(c, b);

    CHECK_EQ(g.TreeIdOf(root), std::string("0"));
    CHECK_EQ(g.TreeIdOf(a), std::string("0/0"));
    CHECK_EQ(g.TreeIdOf(b), std::string("0/1"));
    CHECK_EQ(g.TreeIdOf(c), std::string("0/1/0"));
    CHECK_EQ(g.TreeIdOf("ghost"), std::string(""));

    // Reparenting shifts the path ids like the loader's index paths.
    g.SetParent(c, root);
    CHECK_EQ(g.TreeIdOf(c), std::string("0/2"));
}

TEST(BtGraphMultipleRootsWrapInSequence) {
    btgraph::BtGraph g;
    g.AddNode("wait", math::Vec2{});
    g.AddNode("attack", math::Vec2{});
    core::Json tree = g.ToTreeJson();
    const core::Json* r = tree.Get("root");
    CHECK(r != nullptr);
    if (!r) return;
    CHECK_EQ(r->Get("type")->GetString(), std::string("sequence"));
    const core::Json* kids = r->Get("children");
    CHECK(kids != nullptr && kids->Size() == 2u);
}

TEST(BtGraphEmptyTreeJson) {
    btgraph::BtGraph g;
    core::Json tree = g.ToTreeJson();
    CHECK(tree.IsObject());
    CHECK(tree.Get("root") == nullptr);
}

TEST(BtGraphLayoutIsDeterministic) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    const std::string a = g.AddNode("in_range", math::Vec2{});
    const std::string b = g.AddNode("wait", math::Vec2{});
    g.SetParent(a, root);
    g.SetParent(b, root);
    g.LayoutTopDown();
    const math::Vec2 rootPos = g.Find(root)->pos;
    const math::Vec2 aPos = g.Find(a)->pos;
    const math::Vec2 bPos = g.Find(b)->pos;
    // Root centered between its two children; children share the root's row.
    CHECK_NEAR(aPos.y, rootPos.y + 150.0f, 1e-4);
    CHECK_NEAR(bPos.y, aPos.y, 1e-4);
    CHECK_NEAR(rootPos.x, (aPos.x + bPos.x) * 0.5f, 1e-3);
    // Re-running must not move anything.
    g.LayoutTopDown();
    CHECK_NEAR(g.Find(root)->pos.x, rootPos.x, 1e-4);
    CHECK_NEAR(g.Find(a)->pos.x, aPos.x, 1e-4);
}

// ---------------------------------------------------------------------------
// editor: graph model -> engine tree
// ---------------------------------------------------------------------------

TEST(BtGraphTreeLoadsAndTicks) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    const std::string cond = g.AddNode("in_range", math::Vec2{});
    const std::string act = g.AddNode("move_to", math::Vec2{});
    g.SetArg(cond, "distance", JNum(5));
    g.SetArg(act, "speed", JNum(3));
    g.SetParent(cond, root);
    g.SetParent(act, root);

    std::string err;
    bt::BehaviorTree tree;
    CHECK(tree.LoadText(g.Serialize(), &err));
    CHECK(tree.Valid());

    script::GameVars gv;
    bt::Context ctx(gv, nullptr);
    bool inRange = false;
    int moves = 0;
    ctx.inRange = [&](uint64_t, float) { return inRange; };
    ctx.moveTo = [&](uint64_t, float) { ++moves; return true; };

    CHECK(tree.Tick(ctx) == bt::Status::Failure);
    CHECK_EQ(moves, 0);
    inRange = true;
    CHECK(tree.Tick(ctx) == bt::Status::Success);
    CHECK_EQ(moves, 1);
    CHECK_EQ(ctx.activePath, std::string("0/1"));
}

TEST(BtGraphFromTreeJsonRejectsMalformed) {
    btgraph::BtGraph g;
    const std::string root = g.AddNode("sequence", math::Vec2{});
    core::Json dom = core::Json::Parse(R"({"root":{"type":"nope"}})", nullptr);
    CHECK(!g.FromTreeJson(dom));
    CHECK_EQ(g.NodeCount(), 1u); // transactional: unchanged on failure

    // An action with children is malformed for the graph model.
    core::Json dom2 = core::Json::Parse(R"({"root":{"type":"attack","children":[{"type":"wait"}]}})",
                                        nullptr);
    CHECK(!g.FromTreeJson(dom2));

    // A decorator using "children" is malformed.
    core::Json dom3 = core::Json::Parse(R"({"root":{"type":"invert","children":[{"type":"wait"}]}})",
                                        nullptr);
    CHECK(!g.FromTreeJson(dom3));

    // A composite using "child" is malformed.
    core::Json dom4 = core::Json::Parse(R"({"root":{"type":"sequence","child":{"type":"wait"}}})",
                                        nullptr);
    CHECK(!g.FromTreeJson(dom4));
}

// ---------------------------------------------------------------------------
// editor: file save/load round-trip (smoke level, model-only)
// ---------------------------------------------------------------------------

TEST(BtGraphFileSaveLoadRoundTrip) {
    test::TempDir tmp;
    btgraph::BtGraph g;
    const std::string root = g.AddNode("selector", math::Vec2{});
    const std::string cond = g.AddNode("in_range", math::Vec2{});
    const std::string dec = g.AddNode("cooldown", math::Vec2{});
    const std::string act = g.AddNode("move_to", math::Vec2{});
    g.SetArg(cond, "distance", JNum(8));
    g.SetArg(dec, "seconds", JNum(1.5));
    g.SetArg(act, "speed", JNum(4));
    g.SetParent(cond, root);
    g.SetParent(dec, root);
    g.SetParent(act, dec);

    const std::string path = tmp.Str() + "/behavior.bt.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << g.Serialize();
    }
    std::string text;
    CHECK(test::ReadFileAll(path, text));

    btgraph::BtGraph loaded;
    CHECK(loaded.FromTreeJson(core::Json::Parse(text, nullptr)));
    CHECK_EQ(loaded.Serialize(), g.Serialize());
    CHECK_EQ(loaded.NodeCount(), g.NodeCount());
    CHECK_EQ(loaded.LinkCount(), g.LinkCount());
}
