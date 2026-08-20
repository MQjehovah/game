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
