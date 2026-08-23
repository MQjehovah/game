#include <set>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/gamevars.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// A script host wired to fresh ECS + physics worlds and a recording audio
// sink. Deterministic per-test setup: entities start from a clean world.
struct Bindings {
    std::unique_ptr<script::IScriptHost> host;
    script::ScriptContext ctx;
    ecs::World world;
    physics::World physics;
    std::vector<std::string> sfx;

    Bindings() {
        host = script::CreateLuaHost();
        ctx.world = &world;
        ctx.physics = &physics;
        ctx.playSfx = [this](const std::string& name) { sfx.push_back(name); };
        CHECK(host != nullptr);
        CHECK(host->Init());
        script::RegisterEngineBindings(*host, ctx);
    }

    ~Bindings() {
        if (host) host->Shutdown();
    }
};

bool RunScript(script::IScriptHost& host, const std::string& source) {
    return host.Load(source) && host.Run().Ok();
}

// Native fixtures for table round-trip / cycle / overwrite tests.

// Returns a plain table { x = 3, y = 4 }.
script::Value NativeMakePoint(script::IScriptHost& host, void* /*user*/) {
    (void)host;
    script::Value p = script::Value::Tbl();
    p.table->fields.emplace_back("x", script::Value::Num(3));
    p.table->fields.emplace_back("y", script::Value::Num(4));
    return p;
}

// Ignores its argument and returns a fixed number; used to prove a
// RegisterField overwrite takes effect.
script::Value NativeJsonOverride(script::IScriptHost& host, void* /*user*/) {
    (void)host;
    return script::Value::Num(99);
}

// Returns a self-referential Value::Table: the "self" field's table payload is
// the very same TableValue as the outer one. The conversion guard must stop at
// the cycle instead of recursing forever. (The shared_ptr cycle keeps the
// TableValue alive for the process lifetime; a single small object.)
script::Value NativeMakeCyclic(script::IScriptHost& host, void* /*user*/) {
    (void)host;
    script::Value v = script::Value::Tbl();
    script::Value inner;
    inner.type = script::Value::Type::Table;
    inner.table = v.table; // same TableValue: v.table -> fields[0].second.table == v.table
    v.table->fields.emplace_back("self", inner);
    v.table->fields.emplace_back("leaf", script::Value::Num(7));
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// GameVars
// ---------------------------------------------------------------------------

TEST(GameVarsSetGetHasClear) {
    script::GameVars vars;

    CHECK(!vars.Has("missing"));
    CHECK(vars.Get("missing").type == script::Value::Type::Nil);

    vars.Set("gold", script::Value::Num(42));
    CHECK(vars.Has("gold"));
    CHECK(vars.Get("gold").type == script::Value::Type::Number);
    CHECK_EQ(vars.Get("gold").number, 42.0);

    vars.Set("name", script::Value::Str("neon"));
    CHECK(vars.Get("name").type == script::Value::Type::String);
    CHECK_EQ(vars.Get("name").str, std::string("neon"));

    vars.Set("flag", script::Value::Bool(true));
    CHECK(vars.Get("flag").type == script::Value::Type::Bool);
    CHECK(vars.Get("flag").boolean);

    // Overwrite replaces the stored value.
    vars.Set("gold", script::Value::Num(7));
    CHECK_EQ(vars.Get("gold").number, 7.0);
    CHECK_EQ(vars.Get("gold").type, script::Value::Type::Number);

    // Setting nil stores the key (Has reports it; Get reads it back as nil).
    vars.Set("void", script::Value::Nil());
    CHECK(vars.Has("void"));
    CHECK(vars.Get("void").type == script::Value::Type::Nil);

    vars.Clear();
    CHECK(!vars.Has("gold"));
    CHECK(!vars.Has("name"));
    CHECK(!vars.Has("flag"));
    CHECK(!vars.Has("void"));
    CHECK(vars.Get("gold").type == script::Value::Type::Nil);
}

TEST(GameVarsStoresAllTypes) {
    script::GameVars vars;
    vars.Set("n", script::Value::Num(3.25));
    vars.Set("s", script::Value::Str("hello"));
    vars.Set("b", script::Value::Bool(false));
    CHECK(vars.Get("n").type == script::Value::Type::Number);
    CHECK_EQ(vars.Get("n").number, 3.25);
    CHECK(vars.Get("s").type == script::Value::Type::String);
    CHECK_EQ(vars.Get("s").str, std::string("hello"));
    CHECK(vars.Get("b").type == script::Value::Type::Bool);
    CHECK(!vars.Get("b").boolean);
    CHECK_EQ(vars.Size(), 3u);
}

// ---------------------------------------------------------------------------
// Engine bindings (Lua integration)
// ---------------------------------------------------------------------------

// The Step-1 integration flow: spawn, query/update transform, game vars, and a
// physics raycast. A static sphere at the origin makes the raycast hit
// deterministically (the ray fires down the -Y axis from (0,5,0)).
TEST(ScriptBindingsSpawnPositionVarsRaycast) {
    Bindings b;
    b.physics.AddSphere(100, {0, 0, 0}, 2.0f, false);

    const char* src = R"(
local e = Spawn("wolf", {x=1, y=0, z=2})
assert(type(e) == "table")
assert(e.id ~= nil)
assert(e.gen ~= nil)
local p = GetPosition(e)
assert(type(p) == "table")
assert(p.x == 1 and p.y == 0 and p.z == 2)
SetPosition(e, {x=5, y=0, z=5})
assert(GetPosition(e).x == 5 and GetPosition(e).y == 0 and GetPosition(e).z == 5)
assert(GetVar("gold") == nil)
SetVar("gold", 42)
assert(GetVar("gold") == 42)
assert(Raycast({x=0, y=5, z=0}, {x=0, y=-1, z=0}))
)";
    CHECK(RunScript(*b.host, src));

    // The kind name is recorded on the context for later use.
    CHECK_EQ(b.ctx.entityKinds.size(), 1u);
    if (b.ctx.entityKinds.size() == 1u) {
        for (const auto& kv : b.ctx.entityKinds) {
            CHECK_EQ(kv.second, std::string("wolf"));
        }
    }
    // SetVar surfaced into the context's GameVars.
    CHECK_EQ(b.ctx.gameVars.Get("gold").number, 42.0);
}

TEST(ScriptBindingsDespawnThenGetPositionNil) {
    Bindings b;
    const char* src = R"(
local e = Spawn("wolf", {x=1, y=0, z=2})
assert(GetPosition(e) ~= nil)
Despawn(e)
assert(GetPosition(e) == nil)
Despawn(e)
)";
    CHECK(RunScript(*b.host, src));
    CHECK_EQ(b.ctx.entityKinds.size(), 0u);
    CHECK_EQ(b.world.EntityCount(), 0u);
}

TEST(ScriptBindingsJsonParse) {
    Bindings b;
    const char* src = R"(
local p = Json.Parse('{"a":1,"b":[true,"x",null]}')
assert(p ~= nil)
assert(p.a == 1)
assert(p.b[1] == true)
assert(p.b[2] == "x")
assert(p.b[3] == nil)
)";
    CHECK(RunScript(*b.host, src));
}

TEST(ScriptBindingsPlaySfxCallback) {
    Bindings b;
    const char* src = R"(
PlaySfx("coin")
PlaySfx("jump")
)";
    CHECK(RunScript(*b.host, src));
    CHECK_EQ(b.sfx.size(), 2u);
    if (b.sfx.size() == 2u) {
        CHECK_EQ(b.sfx[0], std::string("coin"));
        CHECK_EQ(b.sfx[1], std::string("jump"));
    }
}

TEST(ScriptBindingsPlaySfxNoOpWhenNull) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx; // playSfx intentionally left unset
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(RunScript(*host, "PlaySfx(\"boom\")"));
    host->Shutdown();
}

TEST(ScriptBindingsMissingWorldGraceful) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx; // world intentionally left null
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    const char* src = R"(
local e = Spawn("wolf", {x=1, y=0, z=2})
assert(e == nil)
assert(GetPosition(e) == nil)
Despawn(e)
SetPosition(e, {x=9, y=9, z=9})
)";
    CHECK(RunScript(*host, src));
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// Robustness: recursion guards + safe number->id casts
// ---------------------------------------------------------------------------

// A cyclic Lua table passed to a binding must not recurse forever.
TEST(ScriptBindingsCyclicTableNoCrash) {
    Bindings b;
    const char* src = R"(
local t = {x=1}
t.self = t
SetVar("cyc", t)
local got = GetVar("cyc")
assert(type(got) == "table")
assert(got.x == 1)
assert(got.self == nil)
)";
    CHECK(RunScript(*b.host, src));
}

// A deeply nested Lua table is truncated at the depth budget instead of
// overflowing the C++ stack.
TEST(ScriptBindingsDeepTableNoCrash) {
    Bindings b;
    const char* src = R"(
local deep = {x=1}
for i=1,5000 do deep = {nested=deep} end
SetVar("deep", deep)
local got = GetVar("deep")
assert(type(got) == "table")
)";
    CHECK(RunScript(*b.host, src));
}

// A native fn returning a self-referential Value::Table is cycle-guarded.
TEST(ScriptBindingsCyclicNativeTableNoCrash) {
    Bindings b;
    b.host->Register("MakeCyclic", &NativeMakeCyclic);
    const char* src = R"(
local c = MakeCyclic()
assert(type(c) == "table")
assert(c.self == nil)
assert(c.leaf == 7)
)";
    CHECK(RunScript(*b.host, src));
}

// Out-of-range / negative ids must be rejected, not UB-cast, and yield nil.
TEST(ScriptBindingsEntityBadIdGraceful) {
    Bindings b;
    const char* src = R"(
local e1 = {id=1e30, gen=0}
assert(GetPosition(e1) == nil)
Despawn(e1)
SetPosition(e1, {x=1, y=1, z=1})
local e2 = {id=-5, gen=0}
assert(GetPosition(e2) == nil)
)";
    CHECK(RunScript(*b.host, src));
}

// ---------------------------------------------------------------------------
// Cheap behavioral coverage
// ---------------------------------------------------------------------------

TEST(ScriptBindingsRaycastMissFalse) {
    Bindings b; // empty physics world: no bodies to hit
    const char* src = R"(
assert(Raycast({x=0, y=5, z=0}, {x=0, y=-1, z=0}) == false)
)";
    CHECK(RunScript(*b.host, src));
}

TEST(ScriptBindingsJsonInvalidNil) {
    Bindings b;
    const char* src = R"(
assert(Json.Parse('{"a":}') == nil)
)";
    CHECK(RunScript(*b.host, src));
}

// Tables round-trip through the host Call path: native returns a table, both
// Lua and C++ read its fields.
TEST(ScriptBindingsCallTableRoundTrip) {
    Bindings b;
    b.host->Register("MakePoint", &NativeMakePoint);

    const char* src = R"(
local p = MakePoint()
assert(p.x == 3 and p.y == 4)
)";
    CHECK(RunScript(*b.host, src));

    auto res = b.host->Call("MakePoint", {});
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Table);
    if (res.Ok() && res.Value().type == script::Value::Type::Table) {
        CHECK_EQ(res.Value().table->fields.size(), 2u);
    }
}

// Re-registering a field replaces the previous native function.
TEST(ScriptBindingsRegisterFieldOverwrite) {
    Bindings b;
    b.host->RegisterField("Json", "Parse", &NativeJsonOverride, &b.ctx);
    const char* src = R"(
assert(Json.Parse("anything") == 99)
)";
    CHECK(RunScript(*b.host, src));
}

// GetVar/SetVar store and return table values.
TEST(ScriptBindingsVarTableValue) {
    Bindings b;
    const char* src = R"(
SetVar("inv", {a=1, b=2})
local v = GetVar("inv")
assert(type(v) == "table")
assert(v.a == 1 and v.b == 2)
)";
    CHECK(RunScript(*b.host, src));
}

// 2D canvas bindings (data-driven 2D games): DrawRect/DrawRectOutline/DrawText
// append to the runtime's buffer; ReadText loads project data files.
TEST(ScriptBindings2DCanvasAndData) {
    Bindings b;
    std::vector<script::Draw2DCmd> cmds;
    b.ctx.draw2d = &cmds;
    b.ctx.readData = [](const std::string& p) {
        return p == "levels/a.json" ? std::string("{\"ok\":1}") : std::string();
    };

    CHECK(RunScript(*b.host, R"(
      DrawRect(10, 20, 30, 40, 1, 0, 0, 0.5)
      DrawRectOutline(1, 2, 3, 4, 2, 0.5, 0.5, 0.5, 1)
      DrawText("hi", 5, 6, 18, 1, 1, 1, 1)
      local t = ReadText("levels/a.json")
      assert(t == '{"ok":1}')
      assert(ReadText("missing") == "")
    )"));

    CHECK_EQ(cmds.size(), 3u);
    if (cmds.size() == 3u) {
        CHECK(cmds[0].kind == script::Draw2DCmd::Kind::Rect);
        CHECK_NEAR(cmds[0].x, 10.0f, 1e-5);
        CHECK_NEAR(cmds[0].w, 30.0f, 1e-5);
        CHECK_NEAR(cmds[0].a, 0.5f, 1e-5);

        CHECK(cmds[1].kind == script::Draw2DCmd::Kind::RectOutline);
        CHECK_NEAR(cmds[1].thickness, 2.0f, 1e-5);

        CHECK(cmds[2].kind == script::Draw2DCmd::Kind::Text);
        CHECK_EQ(cmds[2].text, std::string("hi"));
        CHECK_NEAR(cmds[2].size, 18.0f, 1e-5);
    }
}

// DrawSprite resolves a texture path through the runtime hook and carries the
// handle in the 2D command; missing textures fall back to a plain quad.
TEST(ScriptBindingsDrawSprite) {
    Bindings b;
    std::vector<script::Draw2DCmd> cmds;
    b.ctx.draw2d = &cmds;
    b.ctx.loadTexture = [](const std::string& p) {
        return p == "assets/sprites/sun.png" ? gfx::TextureHandle{7}
                                             : gfx::TextureHandle{};
    };
    CHECK(RunScript(*b.host, R"(
      DrawSprite("assets/sprites/sun.png", 10, 20, 48, 48)
      DrawSprite("missing.png", 0, 0, 16, 16)
    )"));
    CHECK_EQ(cmds.size(), 2u);
    if (cmds.size() == 2u) {
        CHECK(cmds[0].texture.id == 7u);
        CHECK_NEAR(cmds[0].w, 48.0f, 1e-5);
        CHECK(!cmds[1].texture.Valid());
    }
}

// Data-driven game plumbing: WriteText persists, FindNamedEntity resolves a
// scene entity by name, SetVisible toggles the render-hide list.
TEST(ScriptBindingsWriteFindVisible) {
    Bindings b;
    std::string writtenPath, writtenContent;
    b.ctx.writeData = [&](const std::string& p, const std::string& c) {
        writtenPath = p;
        writtenContent = c;
        return true;
    };
    ecs::Entity target = b.world.Create(); // id 1, generation 1
    b.ctx.findEntity = [&](const std::string& n) {
        return n == "hero" ? target : ecs::Entity{};
    };
    std::set<uint64_t> hidden;
    b.ctx.hiddenEntities = &hidden;

    CHECK(RunScript(*b.host, R"(
      assert(WriteText("save.json", "level=3") == 1)
      local h = FindNamedEntity("hero")
      assert(h ~= nil and h.id == 1 and h.gen == 1)
      assert(FindNamedEntity("ghost") == nil)
      SetVisible(h, false)
    )"));
    CHECK_EQ(writtenPath, std::string("save.json"));
    CHECK_EQ(writtenContent, std::string("level=3"));
    const uint64_t key = (static_cast<uint64_t>(target.id) << 32) | target.generation;
    CHECK(hidden.count(key) == 1u); // SetVisible(false) hid the entity
}

// Godot-style input actions: Defaults + JSON merge, Axis/IsDown/Pressed, and
// the Lua Action* bindings reading through a wired InputMap.
namespace {

struct KeyHeldInput : platform::IInput {
    std::set<platform::Key> held;
    std::set<platform::Key> pressedEdge;
    void HandleEvent(const platform::InputEvent&) override {}
    bool IsDown(platform::Key k) const override { return held.count(k) != 0; }
    bool Pressed(platform::Key k) const override { return pressedEdge.count(k) != 0; }
    bool Released(platform::Key) const override { return false; }
    bool MouseDown(platform::MouseButton) const override { return false; }
    bool MousePressed(platform::MouseButton) const override { return false; }
    bool MouseReleased(platform::MouseButton) const override { return false; }
    math::Vec2 MousePos() const override { return {}; }
    math::Vec2 MouseDelta() const override { return {}; }
    float WheelDelta() const override { return 0.0f; }
    void EndFrame() override { pressedEdge.clear(); }
};

} // namespace

TEST(InputMapDefaultsAndJsonMerge) {
    script::InputMap map = script::InputMap::Defaults();
    CHECK(map.Has("forward"));
    CHECK(map.Has("jump"));
    CHECK_EQ(map.Names().size(), 8u);

    std::string err;
    CHECK(map.Load(R"({"actions":{
        "jump":["X"],
        "custom_axis":{"positive":["W"],"negative":["S"]}
    }})", &err));
    const script::InputAction* jump = map.Find("jump");
    CHECK(jump != nullptr);
    if (jump) {
        CHECK_EQ(jump->keys.size(), 1u);
        CHECK(jump->keys[0] == platform::Key::X); // JSON overrides the default
    }
    CHECK(map.Find("custom_axis") != nullptr);
    CHECK_EQ(map.Names().size(), 9u); // 8 defaults + custom_axis (jump merged)
}

TEST(InputMapAxisAndEdges) {
    script::InputMap map = script::InputMap::Defaults();
    KeyHeldInput in;

    // forward = W / S axis.
    CHECK_NEAR(map.Axis("forward", in), 0.0f, 1e-6);
    in.held.insert(platform::Key::W);
    CHECK_NEAR(map.Axis("forward", in), 1.0f, 1e-6);
    in.held.insert(platform::Key::S);
    CHECK_NEAR(map.Axis("forward", in), 0.0f, 1e-6);

    // jump = Space down/pressed.
    CHECK(!map.IsDown("jump", in));
    in.held.insert(platform::Key::Space);
    in.pressedEdge.insert(platform::Key::Space);
    CHECK(map.IsDown("jump", in));
    CHECK(map.Pressed("jump", in));
    in.pressedEdge.clear();
    CHECK(!map.Pressed("jump", in));
}

TEST(ScriptBindingsActionQueries) {
    Bindings b;
    script::InputMap map = script::InputMap::Defaults();
    KeyHeldInput in;
    in.held.insert(platform::Key::W);
    in.held.insert(platform::Key::Space);
    in.pressedEdge.insert(platform::Key::Space);
    b.ctx.inputMap = &map;
    b.ctx.input = &in;

    const bool ok = RunScript(*b.host, R"(
      assert(ActionAxis("forward") == 1)
      assert(ActionAxis("strafe") == 0)
      assert(ActionDown("jump") == 1)
      assert(ActionPressed("jump") == 1)
      assert(ActionPressed("forward") == 0)
      assert(ActionAxis("unknown_action") == 0)
    )");
    if (!ok) std::printf("DIAG ActionQueries error: %s\n",
                         b.host->LastError().message.c_str());
    CHECK(ok);
}

// Godot-style signals: SignalConnect captures a Lua function value (local
// closures work); SignalEmit calls every handler with the argument.
TEST(ScriptBindingsSignals) {
    Bindings b;
    std::vector<std::pair<std::string, uint64_t>> handlers;
    b.ctx.signalHandlers = &handlers;
    CHECK(RunScript(*b.host, R"(
      local count = 0
      local last = 0
      function on_wave(n)
        count = count + 1
        last = n
      end
      SignalConnect("wave_started", on_wave)
      SignalEmit("wave_started", 1)
      SignalEmit("wave_started", 2)
      assert(count == 2)
      assert(last == 2)
      SignalEmit("other", 99)
      assert(count == 2)
    )"));
    CHECK_EQ(handlers.size(), 1u);
    CHECK_EQ(handlers[0].first, std::string("wave_started"));
}
