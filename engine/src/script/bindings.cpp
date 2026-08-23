#include "neon/script/bindings.hpp"

#include <cmath>
#include <cstdint>

#include "neon/core/json.hpp"
#include "neon/scene/status.hpp"

namespace neon::script {
namespace {

constexpr float kRayMaxDist = 100000.0f;

// Lua numbers are doubles; casting one directly to uint32_t is UB for
// negative, NaN, or >= 2^32 values. Range-check first; anything out of range
// maps to 0 (an invalid entity id).
uint32_t SafeU32FromNumber(double n) {
    if (std::isfinite(n) && n >= 0.0 && n < 4294967296.0) {
        return static_cast<uint32_t>(n);
    }
    return 0;
}

// Entity handles are small Lua tables { id = <int>, gen = <int> }.
Value EntityToValue(const ecs::Entity& e) {
    Value t = Value::Tbl();
    t.table->fields.emplace_back("id", Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", Value::Num(static_cast<double>(e.generation)));
    return t;
}

// Rebuilds an entity from a Lua table; an invalid entity (missing id, wrong
// type, out-of-range id, or no table at all) comes back as an all-zero,
// invalid handle.
ecs::Entity EntityFromValue(const Value& v) {
    if (v.type != Value::Type::Table || !v.table) return {};
    ecs::Entity e;
    bool hasId = false;
    for (const auto& kv : v.table->fields) {
        if (kv.second.type != Value::Type::Number) continue;
        if (kv.first == "id") {
            e.id = SafeU32FromNumber(kv.second.number);
            hasId = true;
        } else if (kv.first == "gen") {
            e.generation = SafeU32FromNumber(kv.second.number);
        }
    }
    if (!hasId || e.id == 0) return {};
    return e;
}

Value Vec3ToValue(const math::Vec3& v) {
    Value t = Value::Tbl();
    t.table->fields.emplace_back("x", Value::Num(v.x));
    t.table->fields.emplace_back("y", Value::Num(v.y));
    t.table->fields.emplace_back("z", Value::Num(v.z));
    return t;
}

// Resolves the input state a script sees: the per-entity input registered for
// the entity currently being updated (multi-player), else the shared input.
platform::IInput* InputFor(ScriptContext& ctx) {
    if (ctx.inputForEntity && ctx.currentEntity.IsValid()) {
        if (platform::IInput* perEntity = ctx.inputForEntity(ctx.currentEntity))
            return perEntity;
    }
    return ctx.input;
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
    if (v.type == Value::Type::String) return v.str;
    // The Lua host reports a numeric-looking string literal ("1") as a NUMBER
    // (lua_isnumber matches numeric strings), so stringify numbers back for
    // bindings that take key names / asset paths (e.g. InputKey("1")).
    if (v.type == Value::Type::Number) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v.number);
        return std::string(buf);
    }
    return std::string();
}

Value NativeSpawn(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    const std::string kind = StringArg(host, 0);
    math::Vec3 pos = Vec3FromValue(host.GetArg(1), math::Vec3{});
    ecs::Entity e = ctx->world->Create();
    ctx->world->Add<CTransformBind>(e, CTransformBind{pos});
    ctx->entityKinds[e] = kind;
    // Optional 3rd arg: attach a Lua script to the spawned entity so it runs
    // on_start/on_update (multi-player player controllers).
    if (host.ArgCount() >= 3 && ctx->spawnScript) {
        const std::string path = StringArg(host, 2);
        if (!path.empty()) ctx->spawnScript(e, path);
    }
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
    if (ctx->sceneGetPos) return Vec3ToValue(ctx->sceneGetPos(e));
    const CTransformBind* t = ctx->world->Get<CTransformBind>(e);
    if (!t) return Value::Nil();
    return Vec3ToValue(t->pos);
}

Value NativeSetPosition(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    if (ctx->sceneSetPos) {
        math::Vec3 p = Vec3FromValue(host.GetArg(1), math::Vec3{});
        ctx->sceneSetPos(e, p);
        return Value::Nil();
    }
    CTransformBind* t = ctx->world->Get<CTransformBind>(e);
    if (!t) return Value::Nil();
    t->pos = Vec3FromValue(host.GetArg(1), t->pos);
    return Value::Nil();
}

Value NativeSetRotationY(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneSetYaw) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    Value v = host.GetArg(1);
    if (v.type != Value::Type::Number) return Value::Nil();
    ctx->sceneSetYaw(e, static_cast<float>(v.number));
    return Value::Nil();
}

Value NativeGetHealth(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneGetHp) return Value::Num(-1);
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    return Value::Num(ctx->sceneGetHp(e));
}

Value NativeSetHealth(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneSetHp) return Value::Nil();
    ecs::Entity e = EntityFromValue(host.GetArg(0));
    Value v = host.GetArg(1);
    if (v.type != Value::Type::Number) return Value::Nil();
    ctx->sceneSetHp(e, static_cast<float>(v.number));
    return Value::Nil();
}

Value NativeSpawnProjectile(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->spawnProjectile) return Value::Nil();
    const math::Vec3 pos = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const math::Vec3 dir = Vec3FromValue(host.GetArg(1), math::Vec3{0, 0, 1});
    const float speed = host.GetArg(2).type == Value::Type::Number
                            ? static_cast<float>(host.GetArg(2).number)
                            : 12.0f;
    const float damage = host.GetArg(3).type == Value::Type::Number
                             ? static_cast<float>(host.GetArg(3).number)
                             : 10.0f;
    const float life = host.GetArg(4).type == Value::Type::Number
                           ? static_cast<float>(host.GetArg(4).number)
                           : 2.0f;
    // Optional 6th arg: the caster entity (never hit by its own projectile).
    const ecs::Entity caster =
        host.ArgCount() >= 6 ? EntityFromValue(host.GetArg(5)) : ecs::Entity{};
    ctx->spawnProjectile(pos, dir, speed, damage, life, caster);
    return Value::Nil();
}

// MeleeAttack(origin{x,y,z}, dir{x,y,z}, range, arcDeg, damage) -> entities hit.
Value NativeMeleeAttack(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->meleeAttack) return Value::Num(0);
    const math::Vec3 origin = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const math::Vec3 dir = Vec3FromValue(host.GetArg(1), math::Vec3{0, 0, 1});
    const float range = host.GetArg(2).type == Value::Type::Number
                            ? static_cast<float>(host.GetArg(2).number)
                            : 2.0f;
    const float arcDeg = host.GetArg(3).type == Value::Type::Number
                             ? static_cast<float>(host.GetArg(3).number)
                             : 90.0f;
    const float damage = host.GetArg(4).type == Value::Type::Number
                             ? static_cast<float>(host.GetArg(4).number)
                             : 15.0f;
    return Value::Num(ctx->meleeAttack(origin, dir, range, arcDeg, damage));
}

// Status effects (M2 combat core): ApplyStatus(ent, "burning", 3, 2) applies
// 3s of burning dealing 2 damage/tick; HasStatus/StatusMagnitude/RemoveStatus
// query and remove. Names resolve through the built-in status table.
Value NativeApplyStatus(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneApplyStatus) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const uint32_t id = scene::StatusIdByName(StringArg(host, 1));
    const float duration =
        host.GetArg(2).type == Value::Type::Number ? static_cast<float>(host.GetArg(2).number) : 0.0f;
    const float magnitude =
        host.GetArg(3).type == Value::Type::Number ? static_cast<float>(host.GetArg(3).number) : 0.0f;
    if (id == 0 || duration <= 0.0f) return Value::Nil();
    ctx->sceneApplyStatus(e, id, duration, magnitude);
    return Value::Nil();
}

Value NativeHasStatus(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneHasStatus) return Value::Num(0);
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const uint32_t id = scene::StatusIdByName(StringArg(host, 1));
    return Value::Num(id != 0 && ctx->sceneHasStatus(e, id) ? 1.0 : 0.0);
}

Value NativeStatusMagnitude(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneStatusMagnitude) return Value::Num(0);
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const uint32_t id = scene::StatusIdByName(StringArg(host, 1));
    return Value::Num(id != 0 ? ctx->sceneStatusMagnitude(e, id) : 0.0);
}

Value NativeRemoveStatus(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneRemoveStatus) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const uint32_t id = scene::StatusIdByName(StringArg(host, 1));
    if (id != 0) ctx->sceneRemoveStatus(e, id);
    return Value::Nil();
}

// CastSkill(name, origin{x,y,z}, dir{x,y,z}, caster) -> 1 cast / 0 failed
// (unknown skill, on cooldown, out of mana). SkillCooldown(caster, name)
// returns the remaining seconds (0 = ready).
Value NativeCastSkill(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->castSkill) return Value::Num(0);
    const std::string name = StringArg(host, 0);
    const math::Vec3 origin = Vec3FromValue(host.GetArg(1), math::Vec3{});
    const math::Vec3 dir = Vec3FromValue(host.GetArg(2), math::Vec3{0, 0, 1});
    const ecs::Entity caster =
        host.ArgCount() >= 4 ? EntityFromValue(host.GetArg(3)) : ecs::Entity{};
    return Value::Num(ctx->castSkill(name, origin, dir, caster));
}

Value NativeSkillCooldown(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->sceneSkillCooldown) return Value::Num(0);
    const ecs::Entity caster = EntityFromValue(host.GetArg(0));
    const std::string name = StringArg(host, 1);
    return Value::Num(ctx->sceneSkillCooldown(name, caster));
}

// AttackBox(center{x,y,z}, half{x,y,z}, yawDeg, damage) -> entities hit. The
// box is centered at `center`, half-extents `half` along the local axes, and
// rotated `yawDeg` degrees around Y.
Value NativeAttackBox(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->attackBox) return Value::Num(0);
    const math::Vec3 center = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const math::Vec3 half = Vec3FromValue(host.GetArg(1), math::Vec3{1, 1, 1});
    const float yawDeg =
        host.GetArg(2).type == Value::Type::Number
            ? static_cast<float>(host.GetArg(2).number) * math::kDegToRad
            : 0.0f;
    const float damage =
        host.GetArg(3).type == Value::Type::Number ? static_cast<float>(host.GetArg(3).number) : 0.0f;
    return Value::Num(ctx->attackBox(center, half, yawDeg, damage));
}

Value NativeInputMouseDown(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Num(0);
    const std::string b = StringArg(host, 0);
    const int idx = b == "right" ? 1 : b == "middle" ? 2 : 0;
    return Value::Num(in->MouseDown(static_cast<platform::MouseButton>(idx)) ? 1.0 : 0.0);
}

Value NativeInputMousePressed(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Num(0);
    const std::string b = StringArg(host, 0);
    const int idx = b == "right" ? 1 : b == "middle" ? 2 : 0;
    return Value::Num(
        in->MousePressed(static_cast<platform::MouseButton>(idx)) ? 1.0 : 0.0);
}

// BindPlayerToClient(entity, clientId): multi-player ownership — the server
// routes that client's MsgInput to the bound entity's script. No-op when the
// host did not wire the hook (single-player runtimes).
Value NativeBindPlayerToClient(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->bindPlayerToClient) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const Value v = host.GetArg(1);
    if (v.type != Value::Type::Number) return Value::Nil();
    ctx->bindPlayerToClient(e, v.number);
    return Value::Nil();
}

// --- 2D immediate-mode canvas (data-driven 2D games) -----------------------
// Design units are 1280x720; DrawRect/DrawRectOutline/DrawText append to the
// runtime's 2D buffer and are flushed by GameRuntime::Draw every frame.

Value NativeDrawRect(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->draw2d) return Value::Nil();
    auto num = [&](int i, float def) {
        return host.GetArg(i).type == Value::Type::Number
                   ? static_cast<float>(host.GetArg(i).number)
                   : def;
    };
    Draw2DCmd c;
    c.kind = Draw2DCmd::Kind::Rect;
    c.x = num(0, 0.0f);
    c.y = num(1, 0.0f);
    c.w = num(2, 10.0f);
    c.h = num(3, 10.0f);
    c.r = num(4, 1.0f);
    c.g = num(5, 1.0f);
    c.b = num(6, 1.0f);
    c.a = num(7, 1.0f);
    ctx->draw2d->push_back(std::move(c));
    return Value::Nil();
}

// DrawSprite(path, x, y, w, h): textured quad. The runtime resolves `path`
// against the project/pack asset root (assets/sprites/*.png). When the
// texture cannot be loaded the command still draws a plain quad with the
// caller's tint (default white).
Value NativeDrawSprite(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->draw2d) return Value::Nil();
    auto num = [&](int i, float def) {
        return host.GetArg(i).type == Value::Type::Number
                   ? static_cast<float>(host.GetArg(i).number)
                   : def;
    };
    Draw2DCmd c;
    c.kind = Draw2DCmd::Kind::Rect;
    const std::string path = StringArg(host, 0);
    if (ctx->loadTexture) c.texture = ctx->loadTexture(path);
    c.x = num(1, 0.0f);
    c.y = num(2, 0.0f);
    c.w = num(3, 64.0f);
    c.h = num(4, 64.0f);
    c.r = num(5, 1.0f);
    c.g = num(6, 1.0f);
    c.b = num(7, 1.0f);
    c.a = num(8, 1.0f);
    ctx->draw2d->push_back(std::move(c));
    return Value::Nil();
}

Value NativeDrawRectOutline(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->draw2d) return Value::Nil();
    auto num = [&](int i, float def) {
        return host.GetArg(i).type == Value::Type::Number
                   ? static_cast<float>(host.GetArg(i).number)
                   : def;
    };
    Draw2DCmd c;
    c.kind = Draw2DCmd::Kind::RectOutline;
    c.x = num(0, 0.0f);
    c.y = num(1, 0.0f);
    c.w = num(2, 10.0f);
    c.h = num(3, 10.0f);
    c.thickness = num(4, 1.0f);
    c.r = num(5, 1.0f);
    c.g = num(6, 1.0f);
    c.b = num(7, 1.0f);
    c.a = num(8, 1.0f);
    ctx->draw2d->push_back(std::move(c));
    return Value::Nil();
}

Value NativeDrawText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->draw2d) return Value::Nil();
    auto num = [&](int i, float def) {
        return host.GetArg(i).type == Value::Type::Number
                   ? static_cast<float>(host.GetArg(i).number)
                   : def;
    };
    Draw2DCmd c;
    c.kind = Draw2DCmd::Kind::Text;
    c.text = StringArg(host, 0);
    c.x = num(1, 0.0f);
    c.y = num(2, 0.0f);
    c.size = num(3, 16.0f);
    c.r = num(4, 1.0f);
    c.g = num(5, 1.0f);
    c.b = num(6, 1.0f);
    c.a = num(7, 1.0f);
    c.centerX = host.GetArg(8).type == Value::Type::Bool && host.GetArg(8).boolean;
    c.centerY = host.GetArg(9).type == Value::Type::Bool && host.GetArg(9).boolean;
    ctx->draw2d->push_back(std::move(c));
    return Value::Nil();
}

// ReadText(path): loads a text file from the project/pack (levels/*.json etc.),
// returns "" when missing. Lets data-driven games ship level/config JSON next
// to their scripts.
Value NativeReadText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->readData) return Value::Str("");
    return Value::Str(ctx->readData(StringArg(host, 0)));
}

// WriteText(path, content): saves a text file into the project/pack data root
// (saves, editor exports). Returns 1 on success / 0 on failure.
Value NativeWriteText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->writeData) return Value::Num(0);
    const std::string path = StringArg(host, 0);
    const std::string content = StringArg(host, 1);
    return Value::Num(ctx->writeData(path, content) ? 1.0 : 0.0);
}

// FindNamedEntity(name) -> entity handle (invalid handle when not found).
Value NativeFindNamedEntity(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->findEntity) return Value::Nil();
    const ecs::Entity e = ctx->findEntity(StringArg(host, 0));
    return e.IsValid() ? EntityToValue(e) : Value::Nil();
}

// SetVisible(entity, true|false): hides/shows an entity in the runtime's
// render pass (dead mobs, spawn markers, toggled props). Works on any entity
// with a mesh; the world state itself is untouched.
Value NativeSetVisible(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->hiddenEntities) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    if (!e.IsValid()) return Value::Nil();
    const uint64_t key =
        (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
    const Value v = host.GetArg(1);
    const bool visible = v.type == Value::Type::Bool ? v.boolean : v.number != 0.0;
    if (visible)
        ctx->hiddenEntities->erase(key);
    else
        ctx->hiddenEntities->insert(key);
    return Value::Nil();
}

// InputMousePos() -> {x=, y=} in 2D design units (1280x720). Falls back to
// raw screen pixels when no renderer conversion is wired.
Value NativeInputMousePos(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Nil();
    math::Vec2 p = ctx->input->MousePos();
    if (ctx->screenToUi) p = ctx->screenToUi(p);
    Value t = Value::Tbl();
    t.table->fields.emplace_back("x", Value::Num(p.x));
    t.table->fields.emplace_back("y", Value::Num(p.y));
    return t;
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

// ---------------------------------------------------------------------------
// Input queries (data-driven gameplay). Every query is guarded on a null input
// (headless hosts / unit tests) and returns 0 so scripts never fault.
// ---------------------------------------------------------------------------

// Single key name -> engine key. "space"/"shift"/"ctrl"/"alt"/"enter"/"esc",
// arrows ("up"/"down"/"left"/"right"), single letters and digits.
platform::Key KeyFromName(const std::string& name) {
    if (name == "space") return platform::Key::Space;
    if (name == "shift") return platform::Key::Shift;
    if (name == "ctrl" || name == "control") return platform::Key::Control;
    if (name == "alt") return platform::Key::Alt;
    if (name == "enter" || name == "return") return platform::Key::Enter;
    if (name == "esc" || name == "escape") return platform::Key::Escape;
    if (name == "up") return platform::Key::ArrowUp;
    if (name == "down") return platform::Key::ArrowDown;
    if (name == "left") return platform::Key::ArrowLeft;
    if (name == "right") return platform::Key::ArrowRight;
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<platform::Key>(static_cast<int>(platform::Key::A) + (c - 'a'));
        }
        if (c >= 'A' && c <= 'Z') {
            return static_cast<platform::Key>(static_cast<int>(platform::Key::A) + (c - 'A'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<platform::Key>(static_cast<int>(platform::Key::D0) + (c - '0'));
        }
    }
    return platform::Key::Unknown;
}

// InputAxis(name): normalized analog-ish axis in [-1, 1]. "forward" is
// W-S, "strafe" is D-A, "vertical" is E-Q. World-space, not camera-relative:
// data-driven gameplay picks its own camera frame if it needs one.
Value NativeInputAxis(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Num(0);
    const std::string name = StringArg(host, 0);
    auto axis = [&](platform::Key pos, platform::Key neg) {
        return (in->IsDown(pos) ? 1.0 : 0.0) - (in->IsDown(neg) ? 1.0 : 0.0);
    };
    if (name == "forward") return Value::Num(axis(platform::Key::W, platform::Key::S));
    if (name == "strafe") return Value::Num(axis(platform::Key::D, platform::Key::A));
    if (name == "vertical") return Value::Num(axis(platform::Key::E, platform::Key::Q));
    return Value::Num(0);
}

// InputKey(name): 1 while the named key is held, 0 otherwise.
Value NativeInputKey(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Num(0);
    const platform::Key key = KeyFromName(StringArg(host, 0));
    if (key == platform::Key::Unknown) return Value::Num(0);
    return Value::Num(in->IsDown(key) ? 1.0 : 0.0);
}

// InputMouseX()/InputMouseY(): accumulated mouse delta since the last frame
// (pixels, screen space). Used for look / aim from data-driven scripts.
Value NativeInputMouseX(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    return Value::Num(in ? in->MouseDelta().x : 0.0);
}

Value NativeInputMouseY(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    return Value::Num(in ? in->MouseDelta().y : 0.0);
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
    host.Register("InputAxis", &NativeInputAxis, &ctx);
    host.Register("InputKey", &NativeInputKey, &ctx);
    host.Register("InputMouseX", &NativeInputMouseX, &ctx);
    host.Register("InputMouseY", &NativeInputMouseY, &ctx);
    host.Register("InputMouseDown", &NativeInputMouseDown, &ctx);
    host.Register("InputMousePressed", &NativeInputMousePressed, &ctx);
    host.Register("SetRotationY", &NativeSetRotationY, &ctx);
    host.Register("GetHealth", &NativeGetHealth, &ctx);
    host.Register("SetHealth", &NativeSetHealth, &ctx);
    host.Register("SpawnProjectile", &NativeSpawnProjectile, &ctx);
    host.Register("MeleeAttack", &NativeMeleeAttack, &ctx);
    host.Register("ApplyStatus", &NativeApplyStatus, &ctx);
    host.Register("HasStatus", &NativeHasStatus, &ctx);
    host.Register("StatusMagnitude", &NativeStatusMagnitude, &ctx);
    host.Register("RemoveStatus", &NativeRemoveStatus, &ctx);
    host.Register("CastSkill", &NativeCastSkill, &ctx);
    host.Register("SkillCooldown", &NativeSkillCooldown, &ctx);
    host.Register("AttackBox", &NativeAttackBox, &ctx);
    host.Register("BindPlayerToClient", &NativeBindPlayerToClient, &ctx);
    host.Register("DrawRect", &NativeDrawRect, &ctx);
    host.Register("DrawSprite", &NativeDrawSprite, &ctx);
    host.Register("DrawRectOutline", &NativeDrawRectOutline, &ctx);
    host.Register("DrawText", &NativeDrawText, &ctx);
    host.Register("ReadText", &NativeReadText, &ctx);
    host.Register("WriteText", &NativeWriteText, &ctx);
    host.Register("FindNamedEntity", &NativeFindNamedEntity, &ctx);
    host.Register("SetVisible", &NativeSetVisible, &ctx);
    host.Register("InputMousePos", &NativeInputMousePos, &ctx);
    host.RegisterField("Json", "Parse", &NativeJsonParse, &ctx);
}

} // namespace neon::script
