#include "neon/script/bindings.hpp"

#include <cstdint>
#include <utility>

#include "neon/core/json.hpp"

namespace neon::script {
namespace {

constexpr float kRayMaxDist = 100000.0f;

// Entity handles are small Lua tables { id = <int>, gen = <int> }.
Value EntityToValue(const ecs::Entity& e) {
    Value t = Value::Tbl();
    t.table->fields.emplace_back("id", Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", Value::Num(static_cast<double>(e.generation)));
    return t;
}

// Rebuilds an entity from a Lua table; an invalid entity (missing id, wrong
// type, or no table at all) comes back as an all-zero, invalid handle.
ecs::Entity EntityFromValue(const Value& v) {
    if (v.type != Value::Type::Table || !v.table) return {};
    ecs::Entity e;
    bool hasId = false;
    for (const auto& kv : v.table->fields) {
        if (kv.second.type != Value::Type::Number) continue;
        if (kv.first == "id") {
            e.id = static_cast<uint32_t>(kv.second.number);
            hasId = true;
        } else if (kv.first == "gen") {
            e.generation = static_cast<uint32_t>(kv.second.number);
        }
    }
    if (!hasId) return {};
    return e;
}

Value Vec3ToValue(const math::Vec3& v) {
    Value t = Value::Tbl();
    t.table->fields.emplace_back("x", Value::Num(v.x));
    t.table->fields.emplace_back("y", Value::Num(v.y));
    t.table->fields.emplace_back("z", Value::Num(v.z));
    return t;
}

// Reads {x=,y=,z=} from a table, falling back to `def` for missing/non-numeric
// fields. Positional tables are not supported; named fields only.
math::Vec3 Vec3FromValue(const Value& v, const math::Vec3& def) {
    if (v.type != Value::Type::Table || !v.table) return def;
    math::Vec3 out = def;
    for (const auto& kv : v.table->fields) {
        if (kv.second.type != Value::Type::Number) continue;
        if (kv.first == "x") out.x = static_cast<float>(kv.second.number);
        else if (kv.first == "y") out.y = static_cast<float>(kv.second.number);
        else if (kv.first == "z") out.z = static_cast<float>(kv.second.number);
    }
    return out;
}

std::string StringArg(IScriptHost& host, int index) {
    Value v = host.GetArg(index);
    return v.type == Value::Type::String ? v.str : std::string();
}

Value NativeSpawn(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    const std::string kind = StringArg(host, 0);
    math::Vec3 pos = Vec3FromValue(host.GetArg(1), math::Vec3{});
    ecs::Entity e = ctx->world->Create();
    ctx->world->Add<CTransformBind>(e, CTransformBind{pos});
    ctx->entityKinds[e] = kind;
    return EntityToValue(e);
}

Value NativeDespawn(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    if (!e.IsValid() || !ctx->world->Alive(e)) return Value::Nil();
    ctx->world->Destroy(e);
    ctx->entityKinds.erase(e);
    return Value::Nil();
}

Value NativeGetPosition(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    const CTransformBind* t = ctx->world->Get<CTransformBind>(e);
    if (!t) return Value::Nil();
    return Vec3ToValue(t->pos);
}

Value NativeSetPosition(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    CTransformBind* t = ctx->world->Get<CTransformBind>(e);
    if (!t) return Value::Nil();
    t->pos = Vec3FromValue(host.GetArg(1), t->pos);
    return Value::Nil();
}

Value NativeGetVar(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx) return Value::Nil();
    return ctx->gameVars.Get(StringArg(host, 0));
}

Value NativeSetVar(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx) return Value::Nil();
    ctx->gameVars.Set(StringArg(host, 0), host.GetArg(1));
    return Value::Nil();
}

Value NativeRaycast(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    math::Vec3 origin = Vec3FromValue(host.GetArg(0), math::Vec3{});
    math::Vec3 dir = Vec3FromValue(host.GetArg(1), math::Vec3{0, -1, 0});
    math::Ray ray{origin, dir};
    float t = 0.0f;
    bool hit = ctx->physics->Raycast(ray, kRayMaxDist, t, nullptr);
    return Value::Bool(hit);
}

Value NativePlaySfx(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->playSfx) return Value::Nil();
    std::string name = StringArg(host, 0);
    if (!name.empty()) ctx->playSfx(name);
    return Value::Nil();
}

// Recursively converts a core::Json DOM into Lua-shaped Values. JSON null
// becomes Value::Nil; null entries inside arrays are kept in the array so the
// index layout matches the source, and pushing them later drops the key (so a
// null array element reads back as nil, matching Lua semantics).
Value JsonToValue(const core::Json& j) {
    switch (j.type()) {
        case core::Json::Type::Null: return Value::Nil();
        case core::Json::Type::Bool: return Value::Bool(j.GetBool());
        case core::Json::Type::Number: return Value::Num(j.GetNumber());
        case core::Json::Type::String: return Value::Str(j.GetString());
        case core::Json::Type::Array: {
            Value t = Value::Tbl();
            for (size_t i = 0; i < j.Size(); ++i) {
                const core::Json* item = j.At(i);
                t.table->array.push_back(item ? JsonToValue(*item) : Value::Nil());
            }
            return t;
        }
        case core::Json::Type::Object: {
            Value t = Value::Tbl();
            for (const auto& kv : j.Members()) {
                t.table->fields.emplace_back(kv.first, JsonToValue(kv.second));
            }
            return t;
        }
    }
    return Value::Nil();
}

Value NativeJsonParse(IScriptHost& host, void* user) {
    (void)user;
    Value textArg = host.GetArg(0);
    if (textArg.type != Value::Type::String) return Value::Nil();
    std::string error;
    core::Json dom = core::Json::Parse(textArg.str, &error);
    if (!error.empty()) return Value::Nil();
    return JsonToValue(dom);
}

} // namespace

void RegisterEngineBindings(IScriptHost& host, ScriptContext& ctx) {
    host.Register("Spawn", &NativeSpawn, &ctx);
    host.Register("Despawn", &NativeDespawn, &ctx);
    host.Register("GetPosition", &NativeGetPosition, &ctx);
    host.Register("SetPosition", &NativeSetPosition, &ctx);
    host.Register("GetVar", &NativeGetVar, &ctx);
    host.Register("SetVar", &NativeSetVar, &ctx);
    host.Register("Raycast", &NativeRaycast, &ctx);
    host.Register("PlaySfx", &NativePlaySfx, &ctx);
    host.RegisterField("Json", "Parse", &NativeJsonParse, &ctx);
}

} // namespace neon::script
