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

// Physics Lua bindings: spawn bodies, drive velocities, query state, and read
// per-step collision pairs. A static floor box + a dropped ball must collide
// (dynamic owner first), and the ball must rest on the box top.
TEST(ScriptBindingsPhysicsBodiesAndCollisions) {
    Bindings b;
    const char* src = R"(
local floor = PhysicsAddBox({x=0, y=0, z=0}, {x=3, y=0.5, z=3}, false)
assert(type(floor) == "number" and floor > 0)
local ball = PhysicsAddSphere({x=0, y=3, z=0}, 0.5, true)
assert(type(ball) == "number" and ball > 0)
PhysicsSetVelocity(ball, {x=0, y=-5, z=0})
local v = PhysicsGetVelocity(ball)
assert(v.y == -5)
PhysicsSetPosition(ball, {x=0, y=2.5, z=0})
assert(PhysicsGetPosition(ball).y == 2.5)
)";
    CHECK(RunScript(*b.host, src));

    bool collided = false;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 180 && !collided; ++i) {
        b.physics.Step(dt, {0, -9.81f, 0});
        collided = !b.physics.Collisions().empty();
    }
    CHECK(collided);

    // The ball rests on the box top (y = boxTop 0.5 + radius 0.5) with zero
    // vertical velocity.
    const char* check = R"(
local p = PhysicsGetPosition(2)
local v = PhysicsGetVelocity(2)
assert(math.abs(p.y - 1.0) < 0.01, "rest y")
assert(math.abs(v.y) < 0.01, "rest vy")
local cols = PhysicsCollisions()
assert(type(cols) == "table")
assert(#cols >= 1)
local c = cols[1]
assert(c.a ~= nil and c.b ~= nil)
)";
    CHECK(RunScript(*b.host, check));
}

TEST(ScriptBindingsPhysicsTriggers) {
    Bindings b;
    const char* src = R"(
local zone = PhysicsAddTriggerBox({x=0, y=1, z=0}, {x=2, y=1, z=2}, {owner=11})
assert(type(zone) == "number" and zone > 0)
local ball = PhysicsAddSphere({x=0, y=5, z=0}, 0.5, true, {owner=22})
assert(type(ball) == "number" and ball > 0)
)";
    CHECK(RunScript(*b.host, src));

    bool reported = false;
    for (int i = 0; i < 240 && !reported; ++i) {
        b.physics.Step(1.0f / 60.0f, {0, -9.81f, 0});
        for (const auto& t : b.physics.Triggers()) {
            if (t.first == 11 && t.second == 22) reported = true;
        }
    }
    CHECK(reported);
    // No collision was ever raised against the sensor (it is non-physical).
    for (const auto& c : b.physics.Collisions()) {
        CHECK(c.first != 11);
        CHECK(c.second != 11);
    }

    // Script-side query exposes the same pairs as {trigger=, other=}.
    const char* check = R"(
local tr = PhysicsTriggers()
assert(type(tr) == "table")
local found = false
for i = 1, #tr do
    if tr[i].trigger == 11 and tr[i].other == 22 then found = true end
end
assert(found, "trigger pair reported to script")
)";
    CHECK(RunScript(*b.host, check));
}

TEST(ScriptBindingsPhysicsQueries) {
    Bindings b;
    const char* src = R"(
local box = PhysicsAddBox({x=0, y=0, z=0}, {x=2, y=1, z=2}, false, {owner=200})
assert(box > 0)
local hits = PhysicsOverlapSphere({x=0, y=0.5, z=0}, 1.0)
assert(type(hits) == "table" and #hits >= 1)
local found = false
for i = 1, #hits do if hits[i] == 200 then found = true end end
assert(found, "overlap reports the box owner")

local boxHits = PhysicsOverlapBox({x=0, y=0.5, z=0}, {x=2, y=1, z=2})
assert(#boxHits >= 1)

local cast = PhysicsSphereCast({x=0, y=5, z=0}, 0.5, {x=0, y=-1, z=0}, 10.0)
assert(cast ~= nil, "cast hit")
assert(cast.owner == 200)
assert(math.abs(cast.distance - 3.5) < 0.1, "cast distance")
assert(cast.point.y > 1.4 and cast.point.y < 1.6)
assert(cast.normal.y > 0.9)
)";
    CHECK(RunScript(*b.host, src));
}

TEST(ScriptBindingsDespawnThenGetPositionNil) {    Bindings b;
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
assert(Raycast({x=0, y=5, z=0}, {x=0, y=-1, z=0}) == nil)
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
      assert(WriteText("save.json", "level=3") == true)
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
      -- Action queries return real booleans (Lua's number 0 is truthy, so a
      -- numeric 0/1 return made `if ActionPressed(x)` fire every frame).
      assert(ActionDown("jump") == true)
      assert(ActionPressed("jump") == true)
      assert(ActionPressed("forward") == false)
      assert(not ActionDown("no_such_action"))
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

// Engine push-downs for MOBA/RTS scripts: PickGround + the perspective-correct
// ground drawing bindings + the fog-of-war bindings all dispatch to their
// ScriptContext hooks.
TEST(ScriptBindingsPickGroundDispatches) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    ctx.groundPick = [](const math::Vec2& d, math::Vec3& out) {
        out = {d.x * 2.0f, 0.0f, d.y * 3.0f};
        return true;
    };
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(RunScript(*host, "local p = PickGround(5, 7); GX = p.x; GZ = p.z"));
    const auto gx = host->GetGlobal("GX");
    const auto gz = host->GetGlobal("GZ");
    CHECK(gx.Ok() && gx.Value().type == script::Value::Type::Number);
    CHECK_NEAR(gx.Value().number, 10.0, 1e-6);
    CHECK(gz.Ok() && gz.Value().type == script::Value::Type::Number);
    CHECK_NEAR(gz.Value().number, 21.0, 1e-6);
    host->Shutdown();
}

TEST(ScriptBindingsGroundDrawBindings) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    std::vector<script::Draw2DCmd> cmds;
    ctx.draw2d = &cmds;
    ctx.worldToScreen = [](const math::Vec3& w, float& sx, float& sy) {
        sx = 640.0f + w.x;
        sy = 360.0f + w.z;
        return true;
    };
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    // 12-segment ring + 8-segment disc + 4-segment line + one gradient triangle.
    CHECK(RunScript(*host, R"(
      DrawGroundRing(0, 0, 5, 2, 1, 0, 0, 1, 12)
      DrawGroundDisc(0, 0, 3, 1, 1, 1, 0.5, 8)
      DrawGroundLine(0, 0, 5, 5, 2, 0, 1, 0, 1, 4)
      DrawTriGradient(0,0, 1,0, 0,1, 1,0,0,1, 0,1,0,1, 0,0,1,1)
    )"));
    CHECK_EQ(cmds.size(), 25u);
    CHECK_EQ(static_cast<int>(cmds[0].kind),
             static_cast<int>(script::Draw2DCmd::Kind::Line));
    CHECK_EQ(static_cast<int>(cmds.back().kind),
             static_cast<int>(script::Draw2DCmd::Kind::TriangleGradient));
    host->Shutdown();
}

TEST(ScriptBindingsFogDispatches) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    int setupCols = 0;
    bool began = false;
    float srcX = 0.0f, srcZ = 0.0f, srcR = 0.0f;
    int drawCalls = 0;
    ctx.fogSetup = [&](int cols, int rows, float cell, float minX, float minZ, float, float,
                       float, float, float) { setupCols = cols; };
    ctx.fogBegin = [&]() { began = true; };
    ctx.fogAddSource = [&](float x, float z, float r) { srcX = x; srcZ = z; srcR = r; };
    ctx.fogVisibleAt = [](float, float) { return false; };
    ctx.fogDraw = [&]() -> int { ++drawCalls; return 7; };
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(RunScript(*host, R"(
      FogSetup(8, -96, -96, 25, 25, 0.5, 0.96, 0.02, 0.02, 0.05)
      FogBegin()
      FogAddSource(1, 2, 3)
      FOGV = FogVisibleAt(0, 0)
      FOGT = FogDraw()
    )"));
    CHECK_EQ(setupCols, 25);
    CHECK(began);
    CHECK_NEAR(srcX, 1.0, 1e-6);
    CHECK_NEAR(srcZ, 2.0, 1e-6);
    CHECK_NEAR(srcR, 3.0, 1e-6);
    CHECK_EQ(drawCalls, 1);
    const auto fv = host->GetGlobal("FOGV");
    CHECK(fv.Ok() && fv.Value().type == script::Value::Type::Bool && !fv.Value().boolean);
    const auto ft = host->GetGlobal("FOGT");
    CHECK(ft.Ok() && ft.Value().type == script::Value::Type::Number);
    CHECK_NEAR(ft.Value().number, 7.0, 1e-6);
    host->Shutdown();
}

TEST(ScriptBindingsDecalDispatches) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    std::string gotTex;
    math::Vec3 gotPos{};
    float gotSize = 0.0f, gotAlpha = 0.0f;
    int setCalls = 0;
    float setSize = 0.0f, setAlpha = 0.0f;
    ctx.spawnDecal = [&](const std::string& tex, const math::Vec3& p, float s, float a) {
        gotTex = tex;
        gotPos = p;
        gotSize = s;
        gotAlpha = a;
        ecs::Entity e;
        e.id = 7;
        e.generation = 1;
        return e;
    };
    ctx.setDecal = [&](ecs::Entity, float s, float a) {
        ++setCalls;
        setSize = s;
        setAlpha = a;
    };
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(RunScript(*host, R"(
      local d = SpawnDecal("assets/x.png", {x = 1, y = 2, z = 3}, 4, 0.5)
      DID = d.id
      SetDecal(d, 6, 0.2)
    )"));
    CHECK_EQ(gotTex, std::string("assets/x.png"));
    CHECK_NEAR(gotPos.x, 1.0, 1e-6);
    CHECK_NEAR(gotPos.y, 2.0, 1e-6);
    CHECK_NEAR(gotPos.z, 3.0, 1e-6);
    CHECK_NEAR(gotSize, 4.0, 1e-6);
    CHECK_NEAR(gotAlpha, 0.5, 1e-6);
    CHECK_EQ(setCalls, 1);
    CHECK_NEAR(setSize, 6.0, 1e-6);
    CHECK_NEAR(setAlpha, 0.2, 1e-6);
    const auto did = host->GetGlobal("DID");
    CHECK(did.Ok() && did.Value().type == script::Value::Type::Number);
    CHECK_NEAR(did.Value().number, 7.0, 1e-6);
    host->Shutdown();
}

