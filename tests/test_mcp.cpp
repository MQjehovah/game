#include "helpers.hpp"

// D: MCP server (mcp_server) — headless JSON-RPC over a SceneFile. Verifies the
// reflection-driven tools (list/get/set component) so an AI assistant can
// inspect and edit a scene without a GUI. Uses the same component schema that
// drives the editor inspector.

#include <string>

#include "neon/core/json.hpp"
#include "neon/mcp/mcp_server.hpp"
#include "neon/scene/scene_file.hpp"

using namespace neon;

namespace {
core::Json Obj() {
    core::Json j;
    j.type_ = core::Json::Type::Object;
    return j;
}
void Set(core::Json& j, const char* k, core::Json v) { j.object_[k] = std::move(v); }
core::Json Num(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}
core::Json Str(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}
scene::SceneFile MakeScene() {
    scene::SceneFile sf;
    scene::EntityDef e;
    e.id = 1;
    e.name = "hero";
    scene::ComponentDef h;
    h.name = "health";
    h.data = Obj();
    Set(h.data, "hp", Num(10));
    Set(h.data, "maxHp", Num(100));
    e.components.push_back(h);
    sf.entities.push_back(e);
    return sf;
}
} // namespace

TEST(mcp_initialize_and_tools) {
    scene::SceneFile sf = MakeScene();
    core::Json req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(1));
    Set(req, "method", Str("initialize"));
    bool changed = false;
    core::Json res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("result") != nullptr);
    CHECK(!changed);

    req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(2));
    Set(req, "method", Str("tools/list"));
    res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("result") != nullptr);
    CHECK(res.Get("result")->Get("tools") != nullptr);
}

TEST(mcp_list_get_set_component) {
    scene::SceneFile sf = MakeScene();

    // list_entities
    core::Json args = Obj();
    core::Json req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(1));
    Set(req, "method", Str("tools/call"));
    core::Json params = Obj();
    Set(params, "name", Str("list_entities"));
    Set(params, "arguments", args);
    Set(req, "params", std::move(params));
    bool changed = false;
    core::Json res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("result") != nullptr);
    CHECK(!changed);

    // get_component: type-checked via the reflection schema (health exists).
    args = Obj();
    Set(args, "entity", Num(1));
    Set(args, "name", Str("health"));
    req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(2));
    Set(req, "method", Str("tools/call"));
    params = Obj();
    Set(params, "name", Str("get_component"));
    Set(params, "arguments", std::move(args));
    Set(req, "params", std::move(params));
    res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("result") != nullptr);
    CHECK(!changed);

    // set_component: change hp; validated by reflection; scene mutated + changed.
    args = Obj();
    Set(args, "entity", Num(1));
    Set(args, "name", Str("health"));
    core::Json value = Obj();
    Set(value, "hp", Num(42));
    Set(value, "maxHp", Num(100));
    Set(args, "value", std::move(value));
    req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(3));
    Set(req, "method", Str("tools/call"));
    params = Obj();
    Set(params, "name", Str("set_component"));
    Set(params, "arguments", std::move(args));
    Set(req, "params", std::move(params));
    res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("result") != nullptr);
    CHECK(changed);

    // The scene was actually mutated.
    bool found = false;
    for (const auto& e : sf.entities)
        for (const auto& c : e.components)
            if (c.name == "health" && c.data.Get("hp") && c.data.Get("hp")->GetNumber() == 42.0)
                found = true;
    CHECK(found);

    // set_component with a wrong-typed field is rejected (reflection schema).
    args = Obj();
    Set(args, "entity", Num(1));
    Set(args, "name", Str("health"));
    core::Json bad = Obj();
    Set(bad, "hp", Str("not-a-number"));
    Set(bad, "maxHp", Num(100));
    Set(args, "value", std::move(bad));
    req = Obj();
    Set(req, "jsonrpc", Str("2.0"));
    Set(req, "id", Num(4));
    Set(req, "method", Str("tools/call"));
    params = Obj();
    Set(params, "name", Str("set_component"));
    Set(params, "arguments", std::move(args));
    Set(req, "params", std::move(params));
    res = mcp::Handle(sf, req, &changed);
    CHECK(res.Get("error") != nullptr); // rejected
}
