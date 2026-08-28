#include "neon/script/bindings.hpp"
#include "neon/script/input_map.hpp" // C7: KeyFromName single source of truth

#include <cmath>
#include <cstdint>

#include "neon/core/json.hpp"
#include "neon/core/localization.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/status.hpp"

namespace neon::script {
namespace {

constexpr float kRayMaxDist = 100000.0f;

// Defined below; forward-declared so EntityComponent can convert component
// JSON into a script Value.
Value JsonToValue(const core::Json& j);
core::Json ValueToJson(const Value& v);

// Lua numbers are doubles; casting one directly to uint32_t is UB for
// negative, NaN, or >= 2^32 values. Range-check first; anything out of range
// maps to 0 (an invalid entity id).
uint32_t SafeU32FromNumber(double n) {
    if (std::isfinite(n) && n >= 0.0 && n < 4294967296.0) {
        return static_cast<uint32_t>(n);
    }
    return 0;
}

float NumberArg(IScriptHost& host, int index, float def) {
    const Value v = host.GetArg(index);
    return v.type == Value::Type::Number ? static_cast<float>(v.number) : def;
}

bool BoolArg(IScriptHost& host, int index, bool def) {
    const Value v = host.GetArg(index);
    return v.type == Value::Type::Bool ? v.boolean : def;
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

// SetScale(entity, x, y, z) or SetScale(entity, uniform): writes the entity's
// transform scale (death shrink, spawn pop-in...).
Value NativeSetScale(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->setScale) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const Value& a1 = host.GetArg(1);
    if (a1.type != Value::Type::Number) return Value::Nil();
    math::Vec3 s{static_cast<float>(a1.number), static_cast<float>(a1.number),
                 static_cast<float>(a1.number)};
    const Value& a2 = host.GetArg(2);
    const Value& a3 = host.GetArg(3);
    if (a2.type == Value::Type::Number && a3.type == Value::Type::Number) {
        s.y = static_cast<float>(a2.number);
        s.z = static_cast<float>(a3.number);
    }
    ctx->setScale(e, s);
    return Value::Nil();
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
    return Value::Bool(id != 0 && ctx->sceneHasStatus(e, id));
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
    if (!ctx || !ctx->input) return Value::Bool(false);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Bool(false);
    const std::string b = StringArg(host, 0);
    const int idx = b == "right" ? 1 : b == "middle" ? 2 : 0;
    return Value::Bool(in->MouseDown(static_cast<platform::MouseButton>(idx)));
}

Value NativeInputMousePressed(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Bool(false);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Bool(false);
    const std::string b = StringArg(host, 0);
    const int idx = b == "right" ? 1 : b == "middle" ? 2 : 0;
    return Value::Bool(in->MousePressed(static_cast<platform::MouseButton>(idx)));
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

// UIShow(path): loads and shows a UI document (.ui.json, project-relative).
// Returns 1 when the document loaded, 0 otherwise.
Value NativeUIShow(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->uiShow) return Value::Num(0);
    return Value::Bool(ctx->uiShow(StringArg(host, 0)));
}

// UIHide(): hides the currently shown UI document (if any).
Value NativeUIHide(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (ctx && ctx->uiHide) ctx->uiHide();
    return Value::Nil();
}

// UIClicked(name): 1 when the named button was clicked since the last frame.
Value NativeUIClicked(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->uiClicked) return Value::Num(0);
    return Value::Bool(ctx->uiClicked(StringArg(host, 0)));
}

// UISetText(name, text) / UISetFill(name, 0..1) / UISetVisible(name, bool):
// mutate a node by name (no-op when the node is missing).
Value NativeUISetText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (ctx && ctx->uiSetText) ctx->uiSetText(StringArg(host, 0), StringArg(host, 1));
    return Value::Nil();
}

Value NativeUISetFill(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (ctx && ctx->uiSetFill) {
        const Value v = host.GetArg(1);
        ctx->uiSetFill(StringArg(host, 0), v.type == Value::Type::Number ? v.number : 0.0);
    }
    return Value::Nil();
}

Value NativeUISetVisible(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (ctx && ctx->uiSetVisible) {
        const Value v = host.GetArg(1);
        ctx->uiSetVisible(StringArg(host, 0),
                          v.type == Value::Type::Bool ? v.boolean : v.number != 0.0);
    }
    return Value::Nil();
}

// UISetColor(name, r, g, b[, a]): tint a node (label text / panel / bar fill).
Value NativeUISetColor(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (ctx && ctx->uiSetColor) {
        const float r = static_cast<float>(NumberArg(host, 1, 1.0));
        const float g = static_cast<float>(NumberArg(host, 2, 1.0));
        const float b = static_cast<float>(NumberArg(host, 3, 1.0));
        const float a = static_cast<float>(NumberArg(host, 4, 1.0));
        ctx->uiSetColor(StringArg(host, 0), r, g, b, a);
    }
    return Value::Nil();
}

// Loc(key): localized string for the active language (fallback chain active ->
// default -> key). Returns the key itself when no tables are loaded.
Value NativeLoc(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    const std::string key = StringArg(host, 0);
    if (!ctx || !ctx->loc) return Value::Str(key);
    return Value::Str(ctx->loc->Get(key));
}

// WriteText(path, content): saves a text file into the project/pack data root
// (saves, editor exports). Returns 1 on success / 0 on failure.
Value NativeWriteText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->writeData) return Value::Num(0);
    const std::string path = StringArg(host, 0);
    const std::string content = StringArg(host, 1);
    return Value::Bool(ctx->writeData(path, content));
}

// FindNamedEntity(name) -> entity handle (invalid handle when not found).
Value NativeFindNamedEntity(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->findEntity) return Value::Nil();
    const ecs::Entity e = ctx->findEntity(StringArg(host, 0));
    return e.IsValid() ? EntityToValue(e) : Value::Nil();
}

// SpawnSprite(texture, x, y, width, height, flipX, flipY): creates a
// renderable sprite entity (2D games) and returns its handle. The runtime
// renders it through the normal scene pass, so editing and playtesting share
// one content pipeline. width/height are design units (1 world unit = 1 design
// pixel in the 2D view).
Value NativeSpawnSprite(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->spawnSprite) return Value::Nil();
    const std::string tex = StringArg(host, 0);
    const math::Vec3 pos = Vec3FromValue(host.GetArg(1), math::Vec3{});
    auto num = [&host](int idx, float def) {
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Number ? static_cast<float>(v.number) : def;
    };
    const float w = num(2, 1.0f);
    const float h = num(3, 1.0f);
    auto asBool = [&host](int idx) {
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Bool ? v.boolean : v.number != 0.0;
    };
    const bool flipX = asBool(4);
    const bool flipY = asBool(5);
    const std::string script = host.ArgCount() >= 8 ? StringArg(host, 7) : "";
    return EntityToValue(ctx->spawnSprite(tex, pos, w, h, flipX, flipY, script));
}

// SpawnPrefab(name, pos): instantiates prefabs/<name>.json at runtime (the
// prefab's script components attach and on_start fires immediately). Returns
// the entity handle or nil.
Value NativeSpawnPrefab(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->spawnPrefab) return Value::Nil();
    const std::string name = StringArg(host, 0);
    const math::Vec3 pos = Vec3FromValue(host.GetArg(1), math::Vec3{});
    if (name.empty()) return Value::Nil();
    return EntityToValue(ctx->spawnPrefab(name, pos));
}

// --- M1 gameplay bindings ----------------------------------------------------

// PlayAnimation(entity, clip, loop=true, crossFade=0.2, speed=1): plays a
// named clip (substring match) on the entity's own animation instance.
Value NativePlayAnimation(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->playAnimation) return Value::Bool(false);
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const std::string clip = StringArg(host, 1);
    auto asBool = [&host](int idx, bool def) {
        if (host.ArgCount() <= static_cast<size_t>(idx)) return def;
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Bool ? v.boolean : v.number != 0.0;
    };
    auto num = [&host](int idx, float def) {
        if (host.ArgCount() <= static_cast<size_t>(idx)) return def;
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Number ? static_cast<float>(v.number) : def;
    };
    return Value::Bool(ctx->playAnimation(e, clip, asBool(2, true), num(3, 0.2f),
                                          num(4, 1.0f)));
}

// AnimationProgress(entity) -> [0..1] of the override clip (-1 when none).
Value NativeAnimProgress(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->animProgress) return Value::Num(-1.0);
    return Value::Num(ctx->animProgress(EntityFromValue(host.GetArg(0))));
}

// AnimationFinished(entity) -> true when a one-shot override clip completed.
Value NativeAnimFinished(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->animFinished) return Value::Bool(false);
    return Value::Bool(ctx->animFinished(EntityFromValue(host.GetArg(0))));
}

// AttachStateMachine(entity, path) -> bool (loaded + bound to the skinned model).
Value NativeAttachStateMachine(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->attachStateMachine) return Value::Bool(false);
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const std::string path = StringArg(host, 1);
    return Value::Bool(ctx->attachStateMachine(e, path));
}

// SetAnimParam(entity, name, value) — drives the state machine's transitions.
Value NativeSetAnimParam(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->setAnimParam) return Value::Bool(false);
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const std::string name = StringArg(host, 1);
    float value = 0.0f;
    if (host.ArgCount() > 2) {
        const Value v = host.GetArg(2);
        if (v.type == Value::Type::Number) value = static_cast<float>(v.number);
    }
    ctx->setAnimParam(e, name, value);
    return Value::Bool(true);
}

// WorldToScreen(x, y, z) -> {x=, y=} design coords or nil (behind camera).
Value NativeWorldToScreen(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->worldToScreen) return Value::Nil();
    // Accepts either a table {x=,y=,z=} or three numbers x, y, z.
    math::Vec3 w;
    const Value& a0 = host.GetArg(0);
    if (a0.type == Value::Type::Table) {
        w = Vec3FromValue(a0, math::Vec3{});
    } else if (host.ArgCount() >= 3) {
        w = {static_cast<float>(a0.number),
             static_cast<float>(host.GetArg(1).number),
             static_cast<float>(host.GetArg(2).number)};
    }
    float sx = 0.0f, sy = 0.0f;
    if (!ctx->worldToScreen(w, sx, sy)) return Value::Nil();
    Value t = Value::Tbl();
    t.table->fields.emplace_back("x", Value::Num(sx));
    t.table->fields.emplace_back("y", Value::Num(sy));
    return t;
}

// ScreenToWorld(x, y) or ScreenToWorld({x=,y=}) -> world {x=, y=} or nil
// (perspective camera / before the first render). Design coords in, world XY
// out - the inverse of WorldToScreen for the ortho 2D camera.
Value NativeScreenToWorld(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->worldFromScreen) return Value::Nil();
    math::Vec2 d{0.0f, 0.0f};
    const Value& a0 = host.GetArg(0);
    if (a0.type == Value::Type::Table && a0.table) {
        for (const auto& kv : a0.table->fields) {
            if (kv.second.type != Value::Type::Number) continue;
            if (kv.first == "x") d.x = static_cast<float>(kv.second.number);
            else if (kv.first == "y") d.y = static_cast<float>(kv.second.number);
        }
    } else if (host.ArgCount() >= 2) {
        d = {static_cast<float>(a0.number), static_cast<float>(host.GetArg(1).number)};
    }
    float wx = 0.0f, wy = 0.0f;
    if (!ctx->worldFromScreen(d, wx, wy)) return Value::Nil();
    Value t = Value::Tbl();
    t.table->fields.emplace_back("x", Value::Num(wx));
    t.table->fields.emplace_back("y", Value::Num(wy));
    return t;
}

// GetViewportSize() -> {w=, h=} in design units. Constant-height mapping:
// h is always 720; w follows the live viewport aspect (adaptive UI).
Value NativeGetViewportSize(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    Value t = Value::Tbl();
    if (ctx && ctx->uiViewportSize) {
        const math::Vec2 v = ctx->uiViewportSize();
        t.table->fields.emplace_back("w", Value::Num(v.x));
        t.table->fields.emplace_back("h", Value::Num(v.y));
    } else {
        t.table->fields.emplace_back("w", Value::Num(1280.0));
        t.table->fields.emplace_back("h", Value::Num(720.0));
    }
    return t;
}

// SpawnFloatText(x, y, z, text, crit=false, life=1.2) or
// SpawnFloatText({x=,y=,z=}, text, ...): floating combat text.
Value NativeSpawnFloatText(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->spawnFloatText) return Value::Nil();
    const Value& a0 = host.GetArg(0);
    math::Vec3 w;
    int textIdx = 1;
    if (a0.type == Value::Type::Table) {
        w = Vec3FromValue(a0, math::Vec3{});
    } else if (host.ArgCount() >= 4) {
        w = {static_cast<float>(a0.number),
             static_cast<float>(host.GetArg(1).number),
             static_cast<float>(host.GetArg(2).number)};
        textIdx = 3;
    } else {
        return Value::Nil();
    }
    const std::string text = StringArg(host, textIdx);
    auto asBool = [&host](int idx, bool def) {
        if (host.ArgCount() <= static_cast<size_t>(idx)) return def;
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Bool ? v.boolean : v.number != 0.0;
    };
    auto num = [&host](int idx, float def) {
        if (host.ArgCount() <= static_cast<size_t>(idx)) return def;
        const Value v = host.GetArg(idx);
        return v.type == Value::Type::Number ? static_cast<float>(v.number) : def;
    };
    ctx->spawnFloatText(w, text, asBool(textIdx + 1, false), num(textIdx + 2, 1.2f));
    return Value::Nil();
}

// SetEntityPlate(entity, name, hpFrac): stamps overhead-bar metadata.
Value NativeSetEntityPlate(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->setEntityPlate) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    ctx->setEntityPlate(e, StringArg(host, 1),
                        host.GetArg(2).type == Value::Type::Number
                            ? static_cast<float>(host.GetArg(2).number)
                            : -1.0f);
    return Value::Nil();
}

// ScreenAnchors() -> array of {entity={id=,gen=}, x=, y=, onscreen=, world=}.
Value NativeScreenAnchors(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->screenAnchors) return Value::Tbl(); // empty array
    return ctx->screenAnchors();
}

// EntityPlates() -> map entityKey -> {name=, hp=}.
Value NativeEntityPlates(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->entityPlates) return Value::Tbl();
    return ctx->entityPlates();
}

// FloatTexts() -> array of {world=, text=, crit=, age=, life=}.
Value NativeFloatTexts(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->floatTexts) return Value::Tbl();
    return ctx->floatTexts();
}

// ZombieInfo(entity) -> {row=, delay=, type=, ...} or nil. The zombie
// component is now a generic data component stored in SceneData (readable via
// EntityComponent); this legacy binding reads the same data for older scripts.
Value NativeZombieInfo(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    if (!e.IsValid() || !ctx->world->Alive(e)) return Value::Nil();
    // Legacy hook first (hosts that still wire SceneZombie), then the generic
    // SceneData path so data-only zombie components work everywhere.
    if (ctx->zombieInfo) {
        const Value v = ctx->zombieInfo(e);
        if (v.type != Value::Type::Nil) return v;
    }
    const scene::SceneData* sd = ctx->world->Get<scene::SceneData>(e);
    if (!sd) return Value::Nil();
    for (const auto& kv : sd->components) {
        if (kv.first == "zombie") return JsonToValue(kv.second);
    }
    return Value::Nil();
}

// EntityComponent(entity, name) -> component JSON as a table, or nil. Reads a
// scene component that has no registered factory (plugin/game-data components
// like "inventory" or "plant") from the entity's generic SceneData.
Value NativeEntityComponent(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const std::string name = StringArg(host, 1);
    if (!e.IsValid() || !ctx->world->Alive(e) || name.empty()) return Value::Nil();
    const scene::SceneData* sd = ctx->world->Get<scene::SceneData>(e);
    if (!sd) return Value::Nil();
    for (const auto& kv : sd->components) {
        if (kv.first == name) return JsonToValue(kv.second);
    }
    return Value::Nil();
}

// SetEntityComponent(entity, name, value): writes/replaces a custom component
// on a runtime entity (spawned via Spawn/SpawnSprite) so its script can read
// it with EntityComponent - the same data contract scene-placed entities get
// from their JSON components.
Value NativeSetEntityComponent(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->world) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const std::string name = StringArg(host, 1);
    if (!e.IsValid() || !ctx->world->Alive(e) || name.empty()) return Value::Nil();
    if (!ctx->world->Has<scene::SceneData>(e)) ctx->world->Add<scene::SceneData>(e);
    scene::SceneData* sd = ctx->world->Get<scene::SceneData>(e);
    if (!sd) return Value::Nil();
    const core::Json data = ValueToJson(host.GetArg(2));
    for (auto& kv : sd->components) {
        if (kv.first == name) {
            kv.second = data;
            return Value::Nil();
        }
    }
    sd->components.emplace_back(name, data);
    return Value::Nil();
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

// ChangeScene(path): swaps to another scene file (relative to the project's
// scenes/ root) at the next fixed tick. The current runtime is restarted with
// the same config, so per-scene state resets (title -> level -> results).
Value NativeChangeScene(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->changeScene) return Value::Num(0);
    const std::string path = StringArg(host, 0);
    return Value::Bool(ctx->changeScene(path));
}

// SignalConnect(name, fn): registers a Lua function (captured by value, so
// local/anonymous functions work) for the signal. SignalEmit(name, arg) calls
// every handler in registration order with the argument.
Value NativeSignalConnect(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->signalHandlers) return Value::Nil();
    const std::string name = StringArg(host, 0);
    const core::Result<uint64_t> h = host.CaptureStackFunction(1);
    if (!h.Ok()) return Value::Nil();
    ctx->signalHandlers->emplace_back(name, h.Value());
    return Value::Nil();
}

Value NativeSignalEmit(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->signalHandlers) return Value::Nil();
    const std::string name = StringArg(host, 0);
    // Snapshot the handler list: a handler may connect/disconnect on emit.
    std::vector<uint64_t> calls;
    for (const auto& kv : *ctx->signalHandlers)
        if (kv.first == name) calls.push_back(kv.second);
    const Value arg = host.GetArg(1);
    for (uint64_t handle : calls) host.CallCaptured(handle, {arg});
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

// Raycast(origin, dir [, maxDist]) -> nil on miss; on hit a truthy table
// { hit=true, dist=<t>, point={x,y,z}, entity={id,gen} } (A8: the distance and
// the hit entity used to be discarded, leaving scripts unable to aim/select).
// Legacy truthiness is preserved: miss returns nil (falsy), hit returns a
// non-nil table (truthy), so `if Raycast(...)` keeps working.
Value NativeRaycast(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    math::Vec3 origin = Vec3FromValue(host.GetArg(0), math::Vec3{});
    math::Vec3 dir = Vec3FromValue(host.GetArg(1), math::Vec3{0, -1, 0});
    const Value& distArg = host.GetArg(2);
    const float maxDist = distArg.type == Value::Type::Number
                              ? static_cast<float>(distArg.number)
                              : kRayMaxDist;
    math::Ray ray{origin, dir};
    float t = 0.0f;
    uint64_t hitOwner = 0;
    if (!ctx->physics->Raycast(ray, maxDist, t, &hitOwner)) return Value::Nil();
    Value out = Value::Tbl();
    out.table->fields.emplace_back("hit", Value::Bool(true));
    out.table->fields.emplace_back("dist", Value::Num(t));
    out.table->fields.emplace_back("point", Vec3ToValue(origin + dir * t));
    ecs::Entity e;
    e.id = static_cast<uint32_t>(hitOwner >> 32);
    e.generation = static_cast<uint32_t>(hitOwner & 0xFFFFFFFFu);
    out.table->fields.emplace_back("entity", EntityToValue(e));
    return out;
}

// Reads an optional RigidBodyDesc table ({mass=, restitution=, friction=,
// damping=, gravityScale=}) from `argIndex`; missing entries keep defaults.
physics::RigidBodyDesc RigidBodyDescFromValue(IScriptHost& host, int argIndex) {
    physics::RigidBodyDesc desc;
    const Value v = host.GetArg(argIndex);
    if (v.type != Value::Type::Table || !v.table) return desc;
    for (const auto& kv : v.table->fields) {
        if (kv.second.type != Value::Type::Number) continue;
        const float f = static_cast<float>(kv.second.number);
        if (kv.first == "mass") desc.mass = f;
        else if (kv.first == "restitution") desc.restitution = f;
        else if (kv.first == "friction") desc.friction = f;
        else if (kv.first == "damping") desc.linearDamping = f;
        else if (kv.first == "gravityScale") desc.gravityScale = f;
        else if (kv.first == "layer") desc.layer = static_cast<uint32_t>(f);
        else if (kv.first == "mask") desc.mask = static_cast<uint32_t>(f);
    }
    return desc;
}

Value NativePhysicsAddSphere(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const math::Vec3 pos = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const float radius = NumberArg(host, 1, 0.5f);
    const bool dynamic = BoolArg(host, 2, true);
    const physics::RigidBodyDesc desc = RigidBodyDescFromValue(host, 3);
    const physics::World::BodyId id =
        ctx->physics->AddSphere(0, pos, radius, dynamic, desc);
    return Value::Num(static_cast<double>(id.id));
}

Value NativePhysicsAddBox(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const math::Vec3 center = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const math::Vec3 half = Vec3FromValue(host.GetArg(1), math::Vec3{0.5f, 0.5f, 0.5f});
    const bool dynamic = BoolArg(host, 2, true);
    const physics::RigidBodyDesc desc = RigidBodyDescFromValue(host, 3);
    const physics::World::BodyId id =
        ctx->physics->AddBox(0, center, half, dynamic, desc);
    return Value::Num(static_cast<double>(id.id));
}

Value NativePhysicsRemove(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->Remove({id});
    return Value::Nil();
}

Value NativePhysicsSetVelocity(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->SetVelocity({id}, Vec3FromValue(host.GetArg(1), math::Vec3{}));
    return Value::Nil();
}

Value NativePhysicsGetVelocity(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    return id == 0 ? Value::Nil() : Vec3ToValue(ctx->physics->GetVelocity({id}));
}

Value NativePhysicsSetPosition(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->SetPosition({id}, Vec3FromValue(host.GetArg(1), math::Vec3{}));
    return Value::Nil();
}

Value NativePhysicsGetPosition(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    return id == 0 ? Value::Nil() : Vec3ToValue(ctx->physics->GetPosition({id}));
}

Value NativePhysicsSetMass(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->SetMass({id}, NumberArg(host, 1, 1.0f));
    return Value::Nil();
}

Value NativePhysicsSetRestitution(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->SetRestitution({id}, NumberArg(host, 1, 0.0f));
    return Value::Nil();
}

Value NativePhysicsSetFriction(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0) ctx->physics->SetFriction({id}, NumberArg(host, 1, 0.4f));
    return Value::Nil();
}

Value NativePhysicsIsOnGround(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    return id == 0 ? Value::Bool(false) : Value::Bool(ctx->physics->IsOnGround({id}));
}

Value NativePhysicsCollisions(IScriptHost& host, void* user) {
    (void)host;
    auto* ctx = static_cast<ScriptContext*>(user);
    Value t = Value::Tbl();
    if (!ctx || !ctx->physics) return t;
    for (const auto& c : ctx->physics->Collisions()) {
        Value pair = Value::Tbl();
        pair.table->fields.emplace_back("a", Value::Num(static_cast<double>(c.first)));
        pair.table->fields.emplace_back("b", Value::Num(static_cast<double>(c.second)));
        t.table->array.push_back(std::move(pair));
    }
    return t;
}

Value NativePhysicsAddCharacter(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const math::Vec3 pos = Vec3FromValue(host.GetArg(0), math::Vec3{});
    const float radius = NumberArg(host, 1, 0.4f);
    const float halfHeight = NumberArg(host, 2, 0.9f);
    const physics::RigidBodyDesc desc = RigidBodyDescFromValue(host, 3);
    const physics::World::BodyId id =
        ctx->physics->AddCharacter(0, pos, radius, halfHeight, desc);
    return Value::Num(static_cast<double>(id.id));
}

Value NativePhysicsSetCharacterMove(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    if (id != 0)
        ctx->physics->SetCharacterMove({id}, Vec3FromValue(host.GetArg(1), math::Vec3{}));
    return Value::Nil();
}

Value NativePhysicsGetCharacterMove(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->physics) return Value::Nil();
    const uint32_t id = SafeU32FromNumber(NumberArg(host, 0, 0.0));
    return id == 0 ? Value::Nil()
                   : Vec3ToValue(ctx->physics->GetCharacterMove({id}));
}

Value NativeTween(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->tweenStart) return Value::Nil();
    const ecs::Entity e = EntityFromValue(host.GetArg(0));
    const int prop = static_cast<int>(NumberArg(host, 1, 0.0));
    const math::Vec3 from = Vec3FromValue(host.GetArg(2), math::Vec3{});
    const math::Vec3 to = Vec3FromValue(host.GetArg(3), math::Vec3{});
    const float time = static_cast<float>(NumberArg(host, 4, 1.0));
    const int easing = static_cast<int>(NumberArg(host, 5, 0.0));
    ctx->tweenStart(e, prop, from, to, time, easing);
    return Value::Nil();
}

Value NativeGetEntitiesInGroup(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    Value t = Value::Tbl();
    if (!ctx || !ctx->entitiesInGroup) return t;
    const std::string group = StringArg(host, 0);
    if (group.empty()) return t;
    for (const ecs::Entity& e : ctx->entitiesInGroup(group)) {
        t.table->array.push_back(Value::Num(static_cast<double>(e.id)));
    }
    return t;
}

Value NativePlayMusic(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->playMusic) return Value::Nil();
    ctx->playMusic(StringArg(host, 0), static_cast<float>(NumberArg(host, 1, 1.0)));
    return Value::Nil();
}

Value NativePlaySfx3D(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->playSfx3D) return Value::Nil();
    ctx->playSfx3D(StringArg(host, 0), Vec3FromValue(host.GetArg(1), math::Vec3{}));
    return Value::Nil();
}

Value NativeSetListener(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->setAudioListener) return Value::Nil();
    ctx->setAudioListener(Vec3FromValue(host.GetArg(0), math::Vec3{}),
                          Vec3FromValue(host.GetArg(1), math::Vec3{0, 0, -1}));
    return Value::Nil();
}

Value NativeSetBusVolume(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->setBusVolume) return Value::Nil();
    const int bus = static_cast<int>(NumberArg(host, 0, 0.0));
    ctx->setBusVolume(bus, static_cast<float>(NumberArg(host, 1, 1.0)));
    return Value::Nil();
}

// P2-4: Rpc(name, argsTable) -> sends a named call with JSON-serialized args
// through the host's network layer (no-op when not wired).
core::Json ValueToJson(const Value& v);  // defined below (with JsonToValue)
Value NativeRpc(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->rpcCall) return Value::Nil();
    const std::string name = StringArg(host, 0);
    if (name.empty()) return Value::Nil();
    const std::string json = core::JsonWriter::Write(ValueToJson(host.GetArg(1)));
    ctx->rpcCall(name, json);
    return Value::Nil();
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

// P2-4: converts a Lua-shaped Value back into a core::Json DOM for RPC args.
// Tables serialize as objects (fields) plus array entries under "1".."N"
// keys when both are present; a pure array serializes as a JSON array.
core::Json ValueToJson(const Value& v) {
    core::Json j;
    switch (v.type) {
        case Value::Type::Nil:
            j.type_ = core::Json::Type::Null;
            break;
        case Value::Type::Bool:
            j.type_ = core::Json::Type::Bool;
            j.bool_ = v.boolean;
            break;
        case Value::Type::Number:
            j.type_ = core::Json::Type::Number;
            j.number_ = v.number;
            break;
        case Value::Type::String:
            j.type_ = core::Json::Type::String;
            j.string_ = v.str;
            break;
        case Value::Type::Table: {
            const bool hasFields = !v.table->fields.empty();
            const bool hasArray = !v.table->array.empty();
            if (hasFields || !hasArray) {
                j.type_ = core::Json::Type::Object;
                for (const auto& f : v.table->fields)
                    j.object_[f.first] = ValueToJson(f.second);
                if (hasArray) {
                    for (size_t i = 0; i < v.table->array.size(); ++i)
                        j.object_[std::to_string(i + 1)] = ValueToJson(v.table->array[i]);
                }
            } else {
                j.type_ = core::Json::Type::Array;
                for (const Value& item : v.table->array)
                    j.array_.push_back(ValueToJson(item));
            }
            break;
        }
        default:
            j.type_ = core::Json::Type::Null;
            break;
    }
    return j;
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

// C7: single source of truth -- the full InputMap::KeyFromName (which also
// handles tab/backspace/F1-F12 and lowercases) replaces the old partial table
// that silently dropped those keys for script bindings.
platform::Key KeyFromName(const std::string& name) {
    return InputMap::KeyFromName(name);
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
    // Godot-style: a project input.json action overrides the legacy mapping.
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Num(ctx->inputMap->Axis(name, *in));
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
    const std::string name = StringArg(host, 0);
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Num(ctx->inputMap->IsDown(name, *in) ? 1.0 : 0.0);
    const platform::Key key = KeyFromName(name);
    if (key == platform::Key::Unknown) return Value::Num(0);
    return Value::Num(in->IsDown(key) ? 1.0 : 0.0);
}

// Godot-style action queries: ActionDown/ActionPressed/ActionReleased/
// ActionAxis(name). An action not present in the input map falls back to the
// legacy single-key table (KeyFromName), so scripts migrate gradually.

Value NativeActionDown(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Bool(false);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Bool(false);
    const std::string name = StringArg(host, 0);
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Bool(ctx->inputMap->IsDown(name, *in));
    const platform::Key key = KeyFromName(name);
    return Value::Bool(key != platform::Key::Unknown && in->IsDown(key));
}

Value NativeActionPressed(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Bool(false);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Bool(false);
    const std::string name = StringArg(host, 0);
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Bool(ctx->inputMap->Pressed(name, *in));
    const platform::Key key = KeyFromName(name);
    return Value::Bool(key != platform::Key::Unknown && in->Pressed(key));
}

Value NativeActionReleased(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Bool(false);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Bool(false);
    const std::string name = StringArg(host, 0);
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Bool(ctx->inputMap->Released(name, *in));
    const platform::Key key = KeyFromName(name);
    return Value::Bool(key != platform::Key::Unknown && in->Released(key));
}

Value NativeActionAxis(IScriptHost& host, void* user) {
    auto* ctx = static_cast<ScriptContext*>(user);
    if (!ctx || !ctx->input) return Value::Num(0);
    platform::IInput* in = InputFor(*ctx);
    if (!in) return Value::Num(0);
    const std::string name = StringArg(host, 0);
    if (ctx->inputMap && ctx->inputMap->Has(name))
        return Value::Num(ctx->inputMap->Axis(name, *in));
    return Value::Num(0);
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
    host.Register("PhysicsAddSphere", &NativePhysicsAddSphere, &ctx);
    host.Register("PhysicsAddBox", &NativePhysicsAddBox, &ctx);
    host.Register("PhysicsRemove", &NativePhysicsRemove, &ctx);
    host.Register("PhysicsSetVelocity", &NativePhysicsSetVelocity, &ctx);
    host.Register("PhysicsGetVelocity", &NativePhysicsGetVelocity, &ctx);
    host.Register("PhysicsSetPosition", &NativePhysicsSetPosition, &ctx);
    host.Register("PhysicsGetPosition", &NativePhysicsGetPosition, &ctx);
    host.Register("PhysicsSetMass", &NativePhysicsSetMass, &ctx);
    host.Register("PhysicsSetRestitution", &NativePhysicsSetRestitution, &ctx);
    host.Register("PhysicsSetFriction", &NativePhysicsSetFriction, &ctx);
    host.Register("PhysicsIsOnGround", &NativePhysicsIsOnGround, &ctx);
    host.Register("PhysicsCollisions", &NativePhysicsCollisions, &ctx);
    host.Register("PhysicsAddCharacter", &NativePhysicsAddCharacter, &ctx);
    host.Register("PhysicsSetCharacterMove", &NativePhysicsSetCharacterMove, &ctx);
    host.Register("PhysicsGetCharacterMove", &NativePhysicsGetCharacterMove, &ctx);
    host.Register("Tween", &NativeTween, &ctx);
    host.Register("GetEntitiesInGroup", &NativeGetEntitiesInGroup, &ctx);
    host.Register("PlayMusic", &NativePlayMusic, &ctx);
    host.Register("PlaySfx3D", &NativePlaySfx3D, &ctx);
    host.Register("SetAudioListener", &NativeSetListener, &ctx);
    host.Register("SetBusVolume", &NativeSetBusVolume, &ctx);
    host.Register("Rpc", &NativeRpc, &ctx);
    host.Register("PlaySfx", &NativePlaySfx, &ctx);
    host.Register("InputAxis", &NativeInputAxis, &ctx);
    host.Register("InputKey", &NativeInputKey, &ctx);
    host.Register("InputMouseX", &NativeInputMouseX, &ctx);
    host.Register("InputMouseY", &NativeInputMouseY, &ctx);
    host.Register("ActionDown", &NativeActionDown, &ctx);
    host.Register("ActionPressed", &NativeActionPressed, &ctx);
    host.Register("ActionReleased", &NativeActionReleased, &ctx);
    host.Register("ActionAxis", &NativeActionAxis, &ctx);
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
    host.Register("UIShow", &NativeUIShow, &ctx);
    host.Register("UIHide", &NativeUIHide, &ctx);
    host.Register("UIClicked", &NativeUIClicked, &ctx);
    host.Register("UISetText", &NativeUISetText, &ctx);
    host.Register("UISetFill", &NativeUISetFill, &ctx);
    host.Register("UISetVisible", &NativeUISetVisible, &ctx);
    host.Register("UISetColor", &NativeUISetColor, &ctx);
    host.Register("Loc", &NativeLoc, &ctx);
    host.Register("WriteText", &NativeWriteText, &ctx);
    host.Register("FindNamedEntity", &NativeFindNamedEntity, &ctx);
    host.Register("SpawnSprite", &NativeSpawnSprite, &ctx);
    host.Register("SpawnPrefab", &NativeSpawnPrefab, &ctx);
    // M1 gameplay: per-entity animation + world-anchored HUD helpers.
    host.Register("PlayAnimation", &NativePlayAnimation, &ctx);
    host.Register("AnimationProgress", &NativeAnimProgress, &ctx);
    host.Register("AnimationFinished", &NativeAnimFinished, &ctx);
    // G5-4-4(项2): data-driven animation state machine.
    host.Register("AttachStateMachine", &NativeAttachStateMachine, &ctx);
    host.Register("SetAnimParam", &NativeSetAnimParam, &ctx);
    host.Register("WorldToScreen", &NativeWorldToScreen, &ctx);
    host.Register("ScreenToWorld", &NativeScreenToWorld, &ctx);
    host.Register("GetViewportSize", &NativeGetViewportSize, &ctx);
    host.Register("SpawnFloatText", &NativeSpawnFloatText, &ctx);
    host.Register("SetEntityPlate", &NativeSetEntityPlate, &ctx);
    host.Register("SetScale", &NativeSetScale, &ctx);
    host.Register("ScreenAnchors", &NativeScreenAnchors, &ctx);
    host.Register("EntityPlates", &NativeEntityPlates, &ctx);
    host.Register("FloatTexts", &NativeFloatTexts, &ctx);
    host.Register("ZombieInfo", &NativeZombieInfo, &ctx);
    host.Register("EntityComponent", &NativeEntityComponent, &ctx);
    host.Register("SetEntityComponent", &NativeSetEntityComponent, &ctx);
    host.Register("SetVisible", &NativeSetVisible, &ctx);
    host.Register("ChangeScene", &NativeChangeScene, &ctx);
    host.Register("SignalConnect", &NativeSignalConnect, &ctx);
    host.Register("SignalEmit", &NativeSignalEmit, &ctx);
    host.Register("InputMousePos", &NativeInputMousePos, &ctx);
    host.RegisterField("Json", "Parse", &NativeJsonParse, &ctx);
}

} // namespace neon::script
