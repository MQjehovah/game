#include "neon/scene/game_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"
#include "neon/core/pack.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/scene/scene_file.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef DrawText // windows.h maps DrawText -> DrawTextA; keep the renderer API
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace neon::scene {
namespace {

constexpr float kGravityY = -9.81f;

// Entity handle as a Lua table {id, gen} (matches the T2.3 bindings' shape).
script::Value EntityToValue(const ecs::Entity& e) {
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", script::Value::Num(static_cast<double>(e.generation)));
    return t;
}

// Stable 64-bit key for per-entity BT/blackboard scoping: id occupies the high
// half so an id reused across generations still keys uniquely.
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

// "#RRGGBB" -> Color; empty/invalid -> white.
gfx::Color ParseColorHex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return gfx::Color::White;
    auto nibble = [](char c) -> unsigned int {
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(c - 'A' + 10);
        return 255u;
    };
    auto byte = [&](char hi, char lo) {
        return static_cast<unsigned int>((nibble(hi) << 4) | nibble(lo));
    };
    return {byte(hex[1], hex[2]) / 255.0f, byte(hex[3], hex[4]) / 255.0f,
            byte(hex[5], hex[6]) / 255.0f, 1.0f};
}

// Case-insensitive suffix match ("main.JSON" counts as a .json prefab).
bool HasSuffix(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
            s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

// "a/b/c.json" -> "c" (the prefab registration name).
std::string FileStem(const std::string& p) {
    const size_t slash = p.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Recursively lists every file under `absDir` (sorted, forward-slash relative
// to `absDir`). Missing directories yield an empty list (not an error). Used to
// load a packed game's prefabs/ tree from the unpacked directory.
void ListFilesRecursive(const std::string& absDir, const std::string& prefix,
                        std::vector<std::string>& out) {
#if defined(_WIN32)
    std::string pattern = absDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::string, bool>> entries; // name, isDir
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        entries.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second)
            ListFilesRecursive(absDir + "/" + e.first, rel, out);
        else
            out.push_back(rel);
    }
#else
    DIR* d = ::opendir(absDir.c_str());
    if (!d) return;
    std::vector<std::pair<std::string, bool>> entries;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = absDir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        entries.emplace_back(name, S_ISDIR(st.st_mode));
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second)
            ListFilesRecursive(absDir + "/" + e.first, rel, out);
        else
            out.push_back(rel);
    }
#endif
}

// Returns the mesh to draw for one entity given a camera position: the single
// resolved mesh when the item has no LOD chain, else the chain level selected
// by PickLod. Falls back to the base mesh for a malformed/missing selection.
const gfx::Mesh& SelectLodMesh(const gfx::Mesh& base, const gfx::LodChain& chain,
                               const math::Vec3& pos, const math::Vec3& camPos) {
    if (chain.levels.empty()) return base;
    const int level = PickLod(chain, math::Distance(pos, camPos));
    if (level < 0 || static_cast<size_t>(level) >= chain.levels.size()) return base;
    return chain.levels[static_cast<size_t>(level)];
}

} // namespace

core::Status GameRuntime::Start(const std::string& sceneJson, GameRuntimeConfig cfg) {
    Stop(); // idempotent: a Start always begins from a fresh state

    auto parsed = SceneFile::Parse(sceneJson);
    if (!parsed.Ok()) return core::Status::Err("runtime: " + parsed.Error());

    // Mesh keys are resolved lazily at Draw time (file-backed "obj:"/"gltf:"
    // plus procedural primitives), so instantiation validates structure but
    // not prefixes �?a scene with an unresolvable key still plays headless.
    // cfg_ must be assigned before LoadPrefabs/AttachScripts read it.
    cfg_ = std::move(cfg);
    // Create the physics world: Jolt when requested and compiled, else the
    // deterministic custom solver (server / headless tests).
    physics_ = std::make_unique<physics::World>();
#ifdef NEON_ENABLE_JOLT
    if (cfg_.physicsBackend == "jolt") {
        physics_ = std::make_unique<physics::JoltWorld>();
    }
#endif
    NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Info,
                 "runtime: physics backend '%s' (%zu rigid bodies cap)",
                 cfg_.physicsBackend.c_str(), physics_->BodyCount());
    ComponentRegistry reg;
    RegisterBuiltinComponents(reg, /*assets=*/nullptr);
    LoadPrefabs(); // scene entities may reference prefabs by name (packed games)
    LoadLocales(); // Loc() string tables (best effort; missing dir = no-op)
    auto inst = Instantiate(world_, parsed.Value(), prefs_, reg);
    if (!inst.Ok()) return core::Status::Err("runtime: " + inst.Error());
    RegisterSceneBodies();
    RegisterCharacters();

    scriptCtx_.world = &world_;
    scriptCtx_.physics = physics_.get();
    scriptCtx_.input = cfg_.input;
    scriptCtx_.loc = &loc_;
    scriptCtx_.playSfx = cfg_.playSfx;
    scriptCtx_.playMusic = cfg_.playMusic;
    scriptCtx_.playSfx3D = cfg_.playSfx3D;
    scriptCtx_.setAudioListener = cfg_.setAudioListener;
    scriptCtx_.setBusVolume = cfg_.setBusVolume;
    scriptCtx_.entityKinds.clear();
    // Data files (levels/*.json etc.) resolve like scripts: project dir on
    // disk, or the unpacked dir for packed games (ReadScript honors the pack
    // reader override).
    scriptCtx_.readData = [this](const std::string& path) {
        return ReadScript(FullScriptPath(path));
    };
    scriptCtx_.uiShow = [this](const std::string& path) { return ShowUI(path); };
    scriptCtx_.uiHide = [this]() { HideUI(); };
    scriptCtx_.uiClicked = [this](const std::string& name) { return UIClicked(name); };
    scriptCtx_.uiSetText = [this](const std::string& name, const std::string& text) {
        UISetText(name, text);
    };
    scriptCtx_.uiSetFill = [this](const std::string& name, float fill) {
        UISetFill(name, fill);
    };
    scriptCtx_.uiSetVisible = [this](const std::string& name, bool visible) {
        UISetVisible(name, visible);
    };
    scriptCtx_.loadTexture = [this](const std::string& path) {
        if (!cfg_.assets || path.empty()) return gfx::TextureHandle{};
        return cfg_.assets->LoadTexture(FullAssetPath(path)).Handle();
    };
    scriptCtx_.tweenStart = [this](ecs::Entity e, int prop, const math::Vec3& from,
                                   const math::Vec3& to, float time, int easing) {
        if (time <= 0.0f) return;
        Tween tw;
        tw.target = e;
        tw.prop = prop;
        tw.from = from;
        tw.to = to;
        tw.time = time;
        tw.easing = easing;
        tw.elapsed = 0.0f;
        tweens_.push_back(tw);
    };
    scriptCtx_.entitiesInGroup = [this](const std::string& group) {
        std::vector<ecs::Entity> out;
        if (group.empty()) return out;
        world_.ViewAll<SceneGroups>().ForEach([&](ecs::Entity e, const SceneGroups& g) {
            for (const std::string& name : g.groups) {
                if (name == group) {
                    out.push_back(e);
                    break;
                }
            }
        });
        return out;
    };
    scriptCtx_.writeData = [this](const std::string& path, const std::string& content) {
        std::ofstream out(FullScriptPath(path), std::ios::binary);
        if (!out.is_open()) return false;
        out << content;
        return static_cast<bool>(out);
    };
    scriptCtx_.findEntity = [this](const std::string& name) {
        return FindNamedEntity(name);
    };
    // SpawnSprite: create a renderable sprite entity (2D games). The texture
    // resolves at draw time through the same path as scene sprites, so dynamic
    // gameplay objects render identically in edit and play modes.
    scriptCtx_.spawnSprite = [this](const std::string& tex, const math::Vec3& pos,
                                    float w, float h, bool flipX, bool flipY,
                                    const std::string& scriptPath) {
        if (tex.empty()) return ecs::Entity{};
        ecs::Entity e = world_.Create();
        SceneTransform t;
        t.pos = pos;
        t.scale = {w, h, 1.0f};
        world_.Add<SceneTransform>(e, t);
        SceneSprite s;
        s.texture = tex;
        s.flipX = flipX;
        s.flipY = flipY;
        world_.Add<SceneSprite>(e, s);
        if (!scriptPath.empty()) {
            SceneScript sc;
            sc.backend = "lua";
            sc.path = scriptPath;
            AttachOneScript(e, sc); // loads once, captures + runs on_start
        }
        return e;
    };
    scriptCtx_.zombieInfo = [this](ecs::Entity e) {
        const SceneZombie* z = world_.Get<SceneZombie>(e);
        if (!z) return script::Value::Nil();
        script::Value t = script::Value::Tbl();
        t.table->fields.emplace_back("row", script::Value::Num(z->row));
        t.table->fields.emplace_back("delay", script::Value::Num(z->delay));
        t.table->fields.emplace_back("type", script::Value::Str(z->type));
        return t;
    };
    hiddenEntities_.clear();
    scriptCtx_.hiddenEntities = &hiddenEntities_;
    // Godot-style input actions: seed built-ins, then merge the project's
    // input.json (packed next to game.json; missing file = defaults only).
    inputMap_ = script::InputMap::Defaults();
    const std::string inputJson = ReadScript(FullScriptPath("input.json"));
    if (!inputJson.empty()) {
        std::string mapErr;
        if (!inputMap_.Load(inputJson, &mapErr)) {
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                         "runtime: input.json failed to load: %s (defaults kept)",
                         mapErr.c_str());
        }
    }
    scriptCtx_.inputMap = &inputMap_;
    signalHandlers_.clear();
    scriptCtx_.signalHandlers = &signalHandlers_;
    scriptCtx_.changeScene = [this](const std::string& path) {
        if (path.empty()) return false;
        pendingScene_ = path;
        return true;
    };
    // Combat / control hooks so scripts can drive scene entities. Both
    // component flavors are supported: scene entities carry SceneTransform
    // (from the scene JSON "transform" component) while script-spawned
    // entities (Spawn()) carry CTransformBind. Missing either falls through
    // so GetPosition on a script entity reads the component Spawn created.
    scriptCtx_.sceneGetPos = [this](ecs::Entity e) {
        if (const SceneTransform* t = world_.Get<SceneTransform>(e)) return t->pos;
        if (const script::CTransformBind* t = world_.Get<script::CTransformBind>(e))
            return t->pos;
        return math::Vec3{};
    };
    scriptCtx_.sceneSetPos = [this](ecs::Entity e, const math::Vec3& p) {
        if (SceneTransform* t = world_.Get<SceneTransform>(e)) t->pos = p;
        if (script::CTransformBind* t = world_.Get<script::CTransformBind>(e)) t->pos = p;
    };
    scriptCtx_.sceneSetYaw = [this](ecs::Entity e, float yaw) {
        const math::Quat q = math::Quat::FromAxisAngle({0, 1, 0}, yaw);
        if (SceneTransform* t = world_.Get<SceneTransform>(e)) t->rot = q;
        if (script::CTransformBind* t = world_.Get<script::CTransformBind>(e)) t->rot = q;
    };
    scriptCtx_.sceneGetHp = [this](ecs::Entity e) {
        const SceneHealth* h = world_.Get<SceneHealth>(e);
        return h ? h->hp : -1.0f;
    };
    scriptCtx_.sceneSetHp = [this](ecs::Entity e, float hp) {
        if (SceneHealth* h = world_.Get<SceneHealth>(e)) h->hp = hp;
    };
    // Status-effect hooks (M2 combat core): scripts apply/query/remove
    // buffs+debuffs through ApplyStatus/HasStatus/StatusMagnitude/RemoveStatus.
    scriptCtx_.sceneApplyStatus = [this](ecs::Entity e, uint32_t id, float dur, float mag) {
        if (!world_.Alive(e)) return;
        if (!world_.Has<StatusComponent>(e)) world_.Add<StatusComponent>(e);
        if (StatusComponent* c = world_.Get<StatusComponent>(e)) ApplyStatus(*c, id, dur, mag);
    };
    scriptCtx_.sceneHasStatus = [this](ecs::Entity e, uint32_t id) {
        const StatusComponent* c = world_.Get<StatusComponent>(e);
        return c ? scene::HasStatus(*c, id) : false;
    };
    scriptCtx_.sceneStatusMagnitude = [this](ecs::Entity e, uint32_t id) {
        const StatusComponent* c = world_.Get<StatusComponent>(e);
        return c ? scene::StatusMagnitude(*c, id) : 0.0f;
    };
    scriptCtx_.sceneRemoveStatus = [this](ecs::Entity e, uint32_t id) {
        if (StatusComponent* c = world_.Get<StatusComponent>(e)) RemoveStatus(*c, id);
    };
    // Skill hooks (M2 combat core): data-driven CastSkill / SkillCooldown and
    // the oriented attack box.
    scriptCtx_.castSkill = [this](const std::string& name, const math::Vec3& origin,
                                  const math::Vec3& dir, ecs::Entity caster) {
        return CastSkill(name, origin, dir, caster);
    };
    scriptCtx_.sceneSkillCooldown = [this](const std::string& name, ecs::Entity e) {
        return SkillCooldownLeft(name, e);
    };
    scriptCtx_.attackBox = [this](const math::Vec3& center, const math::Vec3& half, float yaw,
                                  float dmg) {
        return AttackBox(center, half, yaw, dmg);
    };
    scriptCtx_.spawnScript = [this](ecs::Entity e, const std::string& path) {
        if (!world_.Alive(e)) return;
        scene::SceneScript s;
        s.backend = "lua";
        s.path = path;
        if (!world_.Has<SceneScript>(e)) world_.Add<SceneScript>(e, s);
        AttachOneScript(e, s);
    };
    scriptCtx_.spawnProjectile = [this](const math::Vec3& pos, const math::Vec3& dir, float speed,
                                        float damage, float life, ecs::Entity caster) {
        SpawnProjectile(pos, dir, speed, damage, life, caster);
    };
    scriptCtx_.meleeAttack = [this](const math::Vec3& origin, const math::Vec3& dir, float range,
                                    float arcDeg, float damage) {
        return MeleeAttack(origin, dir, range, arcDeg, damage);
    };

    // Lua is the canonical backend (the editor's debugger targets it); a
    // failure there aborts the runtime. The JS backend is optional: when its
    // host fails to initialize, JS-scripted entities are skipped with a log.
    hosts_.lua = script::CreateLuaHost();
    if (!hosts_.lua) {
        Stop();
        return core::Status::Err("runtime: failed to create script host");
    }
    if (!hosts_.lua->Init()) {
        Stop();
        return core::Status::Err("runtime: failed to initialize script host");
    }
    script::RegisterEngineBindings(*hosts_.lua, scriptCtx_);
    hosts_.lua->SetRngSeed(cfg_.rngSeed ? cfg_.rngSeed : 1u); // 0 aliases seed 1
    hosts_.lua->SetSimClock(0.0);

    hosts_.js = script::CreateJsHost();
    if (hosts_.js && hosts_.js->Init()) {
        // Same bindings / seed / clock as Lua: a mixed scene keeps one
        // deterministic stream per backend, seeded identically.
        script::RegisterEngineBindings(*hosts_.js, scriptCtx_);
        hosts_.js->SetRngSeed(cfg_.rngSeed ? cfg_.rngSeed : 1u);
        hosts_.js->SetSimClock(0.0);
    } else {
        hosts_.js.reset();
        NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                     "runtime: JS script backend unavailable; JS scripts are skipped");
    }

    loadedScripts_.clear();
    scriptFailed_.clear();

    AttachScripts();
    AttachTrees();
    // Headless hosts (servers / sim tests) have no renderer: skip the draw
    // list entirely. Draw() is also a no-op without cfg_.assets, so both the
    // flag and a null AssetManager keep the runtime window-free.
    if (!cfg_.headless) BuildDrawList();

    running_ = true;
    simTime_ = 0.0;
    NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                 "runtime: started (%zu entities, %zu scripts, %zu trees, %zu draws)",
                 EntityCount(), ScriptCount(), BehaviorTreeCount(), draws_.size());
    return core::Status::Ok(true);
}

void GameRuntime::Stop() {
    uiDoc_.reset();
    uiClickedNames_.clear();
    physicsAccum_ = 0.0f;
    scripts_.clear();
    trees_.clear();
    draws_.clear();
    projectiles_.clear();
    tweens_.clear();
    skillCooldowns_.clear();
    signalHandlers_.clear();
    pendingScene_.clear();
    loadedScripts_.clear();
    scriptFailed_.clear();
    if (hosts_.lua) {
        hosts_.lua->Shutdown();
        hosts_.lua.reset();
    }
    if (hosts_.js) {
        hosts_.js->Shutdown();
        hosts_.js.reset();
    }
    world_.Clear();
    if (physics_) physics_->Clear();
    scriptCtx_ = script::ScriptContext{};
    prefs_ = PrefabLibrary{};
    running_ = false;
    simTime_ = 0.0;
}

// Registers every scene entity that carries a rigidbody component with the
// physics world. The entity's transform provides the initial position; the
// generated physics body id is stored back on the component so per-step
// SyncSceneBodies can follow it.
void GameRuntime::RegisterSceneBodies() {
    world_.ViewAll<SceneRigidBody, SceneTransform>().ForEach(
        [this](ecs::Entity e, SceneRigidBody& rb, const SceneTransform& t) {
            physics::RigidBodyDesc desc;
            desc.dynamic = rb.dynamic;
            desc.mass = rb.mass;
            desc.restitution = rb.restitution;
            desc.friction = rb.friction;
            desc.linearDamping = rb.linearDamping;
            desc.gravityScale = rb.gravityScale;
            desc.layer = rb.layer;
            desc.mask = rb.mask;
            physics::World::BodyId body;
            if (rb.shape == "box") {
                body = physics_->AddBox(EntityKey(e), t.pos, rb.halfExtents, rb.dynamic, desc);
            } else {
                body = physics_->AddSphere(EntityKey(e), t.pos, rb.radius, rb.dynamic, desc);
            }
            rb.bodyId = body.id;
        });
}

// Registers every entity with a character component as a Jolt virtual
// character (capsule controller). The custom deterministic world returns an
// invalid id, in which case the component is left untouched.
void GameRuntime::RegisterCharacters() {
    world_.ViewAll<SceneCharacter, SceneTransform>().ForEach(
        [this](ecs::Entity e, SceneCharacter& c, const SceneTransform& t) {
            physics::RigidBodyDesc desc;
            desc.layer = c.layer;
            desc.mask = c.mask;
            physics::World::BodyId body =
                physics_->AddCharacter(EntityKey(e), t.pos, c.radius, c.halfHeight, desc);
            c.bodyId = body.id;
        });
}

// Writes the physics bodies' positions back into their entities' transforms
// so rendered meshes follow the simulation. Called after every physics step.
void GameRuntime::SyncSceneBodies() {
    world_.ViewAll<SceneRigidBody, SceneTransform>().ForEach(
        [this](ecs::Entity e, const SceneRigidBody& rb, SceneTransform& t) {
            if (rb.bodyId == 0) return;
            t.pos = physics_->GetPosition({rb.bodyId});
            (void)e;
        });
    world_.ViewAll<SceneCharacter, SceneTransform>().ForEach(
        [this](ecs::Entity, const SceneCharacter& c, SceneTransform& t) {
            if (c.bodyId == 0) return;
            t.pos = physics_->GetPosition({c.bodyId});
        });
}

// Advances scripted tweens (P1-3): linear/in/out/inout easing over the entity's
// position, rotation (euler degrees) or scale. Finished tweens are dropped.
void GameRuntime::TickTweens(float dt) {
    if (tweens_.empty()) return;
    auto ease = [](float a, int kind) {
        switch (kind) {
            case 1: return a * a;                                  // in
            case 2: return 1.0f - (1.0f - a) * (1.0f - a);        // out
            case 3:                                               // inout
                return a < 0.5f ? 2.0f * a * a
                                : 1.0f - (-2.0f * a + 2.0f) * (-2.0f * a + 2.0f) * 0.5f;
            default: return a;                                    // linear
        }
    };
    for (size_t i = 0; i < tweens_.size();) {
        Tween& tw = tweens_[i];
        tw.elapsed += dt;
        const float a = math::Clamp(tw.elapsed / tw.time, 0.0f, 1.0f);
        const float e = ease(a, tw.easing);
        const math::Vec3 v = math::Lerp(tw.from, tw.to, e);
        if (SceneTransform* t = world_.Get<SceneTransform>(tw.target)) {
            if (tw.prop == 0) {
                t->pos = v;
            } else if (tw.prop == 1) {
                t->rot = math::Quat::FromEuler(v.x * 3.14159265f / 180.0f,
                                               v.y * 3.14159265f / 180.0f,
                                               v.z * 3.14159265f / 180.0f);
            } else if (tw.prop == 2) {
                t->scale = v;
            }
        }
        if (tw.elapsed >= tw.time) {
            tweens_[i] = tweens_.back();
            tweens_.pop_back();
        } else {
            ++i;
        }
    }
}

bool GameRuntime::ShowUI(const std::string& path) {
    auto doc = std::make_unique<ui::UiDocument>();
    if (!doc->Load(FullScriptPath(path))) return false;
    uiDoc_ = std::move(doc);
    uiClickedNames_.clear();
    return true;
}

void GameRuntime::HideUI() {
    uiDoc_.reset();
    uiClickedNames_.clear();
}

void GameRuntime::UISetText(const std::string& name, const std::string& text) {
    if (uiDoc_) {
        if (ui::UiNode* n = uiDoc_->Find(name)) n->text = text;
    }
}

void GameRuntime::UISetFill(const std::string& name, float fill) {
    if (uiDoc_) {
        if (ui::UiNode* n = uiDoc_->Find(name)) n->fill = fill;
    }
}

void GameRuntime::UISetVisible(const std::string& name, bool visible) {
    if (uiDoc_) {
        if (ui::UiNode* n = uiDoc_->Find(name)) n->visible = visible;
    }
}

void GameRuntime::AttachScripts() {
    if (!hosts_.lua) return;
    auto view = world_.ViewAll<SceneScript>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScript>(i);
        const SceneScript* s = world_.Get<SceneScript>(ent);
        if (s) AttachOneScript(ent, *s);
    }
    // Unity-style multiple scripts: every entry in a "scripts" component
    // attaches like a single script component.
    auto multiView = world_.ViewAll<SceneScripts>();
    for (size_t i = 0; i < multiView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScripts>(i);
        const SceneScripts* list = world_.Get<SceneScripts>(ent);
        if (!list) continue;
        for (const SceneScript& s : list->items) AttachOneScript(ent, s);
    }
}

bool GameRuntime::AttachOneScript(ecs::Entity ent, const SceneScript& s) {
    if (!hosts_.lua) return false;
    // The component's `backend` field picks the language ("lua" / "js");
    // empty means the schema default (lua). An unknown backend skips the
    // script without failing the runtime.
    const std::string backend = s.backend.empty() ? "lua" : s.backend;
    script::IScriptHost* host = hosts_.Get(backend);
    if (!host) {
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                     "runtime: unknown script backend '%s' for '%s' (skipped)",
                     backend.c_str(), s.path.c_str());
        return false;
    }

    // Defense-in-depth: a hand-crafted pack could reference ".." or an
    // absolute path to read arbitrary local files. Reject such scripts.
    if (neon::core::IsUnsafeRelPath(s.path)) {
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                     "runtime: skipping script '%s' (unsafe path)", s.path.c_str());
        return false;
    }

    const std::string full = FullScriptPath(s.path);
    // Load state is per (backend, path): the same file could be referenced by
    // a Lua and a JS component without sharing a chunk.
    const std::string loadKey = backend + "|" + full;
    if (scriptFailed_.count(loadKey)) {
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                     "runtime: skipping script '%s' (previous load failed)", full.c_str());
        return false;
    }

    // Load + run the chunk once per unique path (defines the global
    // functions); a missing file / syntax error skips every entity that
    // references it without failing the whole runtime.
    if (!loadedScripts_.count(loadKey)) {
        std::string source = ReadScript(full);
        if (source.empty()) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: cannot read script '%s' (skipped)", full.c_str());
            scriptFailed_.insert(full);
            return false;
        }
        if (!host->Load(source)) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' failed to compile: %s (skipped)",
                         full.c_str(), host->LastError().message.c_str());
            scriptFailed_.insert(loadKey);
            return false;
        }
        // Per-entity isolation: clear the handler globals before the chunk
        // runs so a chunk that does NOT define on_start/on_update cannot
        // inherit the previous chunk's handlers (a tree/utility script would
        // otherwise double-run the wrong counter every tick). The captures
        // below then succeed only for handlers THIS chunk declared.
        host->SetGlobal("on_start", script::Value::Nil());
        host->SetGlobal("on_update", script::Value::Nil());
        if (!host->Run().Ok()) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' failed to run: %s (skipped)",
                         full.c_str(), host->LastError().message.c_str());
            scriptFailed_.insert(loadKey);
            return false;
        }
        loadedScripts_.insert(loadKey);
    }

    scripts_.push_back({ent, s.path, host, 0, 0, false});
    ScriptInst& inst = scripts_.back();

    // Per-entity script vars become Lua globals so on_start/on_update can
    // read them (e.g. `aggro`). Globals persist, so across entities the
    // last-set value wins for all of them (documented single-host caveat).
    if (s.vars.IsObject()) {
        for (const auto& kv : s.vars.Members()) {
            host->SetGlobal(kv.first, bt::JsonToValue(kv.second));
        }
    }

    // Capture this chunk's handlers so later chunks cannot shadow them.
    if (const auto h = host->CaptureFunction("on_start"); h.Ok()) inst.onStart = h.Value();
    if (const auto h = host->CaptureFunction("on_update"); h.Ok()) inst.onUpdate = h.Value();
    if (inst.onStart != 0) {
        CallEntityFunctionHandle(inst, inst.onStart, "on_start", {EntityToValue(ent)});
    }
    return true;
}

void GameRuntime::CallEntityFunctionHandle(ScriptInst& inst, uint64_t handle,
                                           const char* fn,
                                           const std::vector<script::Value>& args) {
    if (!inst.host || handle == 0) return;
    // The input bindings resolve per-entity input through the entity being
    // updated (multi-player: each player's script reads its OWN client input).
    scriptCtx_.currentEntity = inst.ent;
    inst.host->SetCurrentScript(inst.path);
    const auto res = inst.host->CallCaptured(handle, args);
    scriptCtx_.currentEntity = {};
    if (!res.Ok() && !inst.errorLogged) {
        inst.errorLogged = true;
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                     "runtime: script '%s' %s() failed: %s", inst.path.c_str(), fn,
                     inst.host->LastError().message.c_str());
    }
}

script::Value GameRuntime::CallScriptOnTree(const BtInst& inst, const std::string& fn,
                                            uint64_t ent) {
    script::IScriptHost* host = inst.host;
    if (!host || !host->HasFunction(fn)) return script::Value::Nil();
    script::Value entVal = script::Value::Tbl();
    entVal.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(ent)));
    scriptCtx_.currentEntity = inst.ent;
    const core::Result<script::Value> res = host->Call(fn, {entVal});
    scriptCtx_.currentEntity = {};
    if (!res.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Bt, core::LogLevel::Error,
                     "runtime: tree script %s() failed: %s", fn.c_str(),
                     host->LastError().message.c_str());
        return script::Value::Nil();
    }
    return res.Value();
}

void GameRuntime::AttachTrees() {
    auto view = world_.ViewAll<SceneBehaviorTree>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneBehaviorTree>(i);
        const SceneBehaviorTree* bt = world_.Get<SceneBehaviorTree>(ent);
        if (!bt) continue;

        // The tree field is either inline JSON ("{...}") or a named reference
        // ("bt:<name>") resolving to behaviors/<name>.bt.json under the script
        // base dir. The same ReadScript path used for Lua scripts honors the
        // readScript override (pack readers) and the disk reader, so packed
        // games load named trees exactly like loose-file projects.
        std::string treeText;
        const std::string& ref = bt->treeJson;
        if (ref.compare(0, 3, "bt:") == 0) {
            const std::string name = ref.substr(3);
            if (name.empty()) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: empty behavior tree reference 'bt:' (skipped)");
                continue;
            }
            // Defense-in-depth: reject names that could escape the base dir
            // (a hostile "bt:../../secret" would otherwise read an arbitrary
            // local file).
            if (neon::core::IsUnsafeRelPath(name)) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: behavior tree '%s' has an unsafe name (skipped)",
                             ref.c_str());
                continue;
            }
            treeText = ReadScript(FullScriptPath("behaviors/" + name + ".bt.json"));
            if (treeText.empty()) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: cannot read behavior tree '%s' (skipped)", ref.c_str());
                continue;
            }
        } else if (!ref.empty() && ref[0] == '{') {
            treeText = ref; // inline JSON tree
        } else {
            NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                         "runtime: behavior tree reference '%s' is neither inline JSON "
                         "nor 'bt:<name>' (skipped)",
                         ref.c_str());
            continue;
        }
        // A UTF-8 BOM (common on Windows editors) must not break the JSON parse.
        if (treeText.size() >= 3 && static_cast<unsigned char>(treeText[0]) == 0xEF &&
            static_cast<unsigned char>(treeText[1]) == 0xBB &&
            static_cast<unsigned char>(treeText[2]) == 0xBF) {
            treeText.erase(0, 3);
        }

        auto tree = std::make_unique<bt::BehaviorTree>();
        std::string err;
        if (!tree->LoadText(treeText, &err)) {
            NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                         "runtime: entity behavior tree failed to load: %s (skipped)",
                         err.c_str());
            continue;
        }
        BtInst inst;
        inst.ent = ent;
        inst.tree = std::move(tree);
        // run_script / script_bool nodes call global functions on the tree
        // entity's own script backend (first mounted script's `backend`;
        // default Lua), so a JS-scripted entity's tree talks to JS.
        std::string backend = "lua";
        if (const SceneScript* sc = world_.Get<SceneScript>(ent))
            backend = sc->backend.empty() ? "lua" : sc->backend;
        else if (const SceneScripts* list = world_.Get<SceneScripts>(ent))
            if (!list->items.empty())
                backend = list->items[0].backend.empty() ? "lua"
                                                         : list->items[0].backend;
        inst.host = hosts_.Get(backend);
        if (!inst.host) inst.host = hosts_.lua.get();
        trees_.push_back(std::move(inst));
    }
}

void GameRuntime::LoadPrefabs() {
    prefs_ = PrefabLibrary{};
    if (cfg_.scriptBaseDir.empty()) return; // disk-less hosts have no prefab tree
    std::vector<std::string> files;
    ListFilesRecursive(cfg_.scriptBaseDir + "/prefabs", "", files);
    size_t loaded = 0;
    for (const std::string& rel : files) {
        if (!HasSuffix(rel, ".json")) continue;
        const std::string name = FileStem(rel);
        if (name.empty()) continue;
        std::string text = ReadScript(FullScriptPath("prefabs/" + rel));
        if (text.empty()) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: prefab '%s' cannot be read (skipped)", rel.c_str());
            continue;
        }
        core::Status st = prefs_.Add(name, text);
        if (!st.Ok()) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: prefab '%s' failed to load: %s (skipped)", rel.c_str(),
                         st.Error().c_str());
            continue;
        }
        ++loaded;
    }
    if (!files.empty()) {
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Debug,
                     "runtime: loaded %zu prefabs", loaded);
    }
}

void GameRuntime::BuildDrawList() {
    // Synchronize with the live entity set: scripts can Spawn/Despawn entities
    // (SpawnSprite, Despawn) while running, so drop dead draws and append new
    // mesh/sprite entities each call while keeping resolved items cached.
    draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                [this](const DrawItem& d) { return !world_.Alive(d.ent); }),
                 draws_.end());
    auto contains = [this](ecs::Entity e) {
        for (const DrawItem& d : draws_)
            if (d.ent == e) return true;
        return false;
    };
    auto view = world_.ViewAll<SceneMesh>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneMesh>(i);
        if (contains(ent)) continue; // already tracked (resolved state kept)
        const SceneMesh* m = world_.Get<SceneMesh>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!m || !t) continue; // a mesh without a transform draws nothing
        DrawItem item;
        item.ent = ent;
        item.meshKey = m->meshKey;
        item.lod = m->lod; // data-driven LOD chain spec; resolved at Draw time
        // Base material. A "gltf:" entity inherits the glTF node's baked PBR
        // material (albedo/metal-roughness/AO/emissive textures) so playtest
        // matches what the editor shows; otherwise start from a plain Lit
        // material. The entity's own color/metallic/roughness/texture fields
        // are then applied on top (mirroring EditorApp::ApplyMaterialParams),
        // so explicit material edits still win over the file's defaults.
        const bool gltfBase = m->meshKey.compare(0, 5, "gltf:") == 0 && cfg_.assets;
        if (gltfBase) {
            assets::GltfAsset gltf = cfg_.assets->LoadGLTF(FullAssetPath(m->meshKey.substr(5)));
            if (!gltf.nodes.empty())
                item.mat = gltf.nodes[0].material;
            else
                item.mat = gfx::Material::Lit({}, ParseColorHex(m->colorHex), 24.0f);
        } else {
            item.mat = gfx::Material::Lit({}, ParseColorHex(m->colorHex), 24.0f);
        }
        // Props that bake colors into vertex data keep a white material tint so
        // the baked colors show through (mirrors EditorApp::ApplyMaterialParams).
        const bool bakedColor = m->meshKey == "terrain" || m->meshKey == "tree" ||
                                m->meshKey == "house" || m->meshKey == "bush" ||
                                m->meshKey == "hero" || m->meshKey == "wolf" ||
                                m->meshKey == "npc" || m->meshKey.compare(0, 4, "npc:") == 0;
        item.mat.tint = bakedColor ? gfx::Color::White : ParseColorHex(m->colorHex);
        item.mat.metallic = m->metallic;
        item.mat.roughness = m->roughness;
        item.mat.aoStrength = m->ao;
        item.mat.emissiveIntensity = m->emissiveIntensity;
        if (cfg_.assets) {
            if (!m->albedoTex.empty())
                item.mat.albedo = cfg_.assets->LoadTexture(FullAssetPath(m->albedoTex)).Handle();
            if (!m->mrTex.empty())
                item.mat.metallicRoughness =
                    cfg_.assets->LoadTexture(FullAssetPath(m->mrTex)).Handle();
            if (!m->aoTex.empty())
                item.mat.occlusion = cfg_.assets->LoadTexture(FullAssetPath(m->aoTex)).Handle();
            if (!m->emissiveTex.empty())
                item.mat.emissive = cfg_.assets->LoadTexture(FullAssetPath(m->emissiveTex)).Handle();
        }
        draws_.push_back(std::move(item));
    }
    auto spriteView = world_.ViewAll<SceneSprite>();
    for (size_t i = 0; i < spriteView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneSprite>(i);
        if (contains(ent)) continue;
        const SceneSprite* s = world_.Get<SceneSprite>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!s || !t) continue; // a sprite without a transform draws nothing
        DrawItem item;
        item.ent = ent;
        item.isSprite = true;
        item.spriteTex = s->texture;
        item.flipX = s->flipX;
        item.flipY = s->flipY;
        item.mat = gfx::Material::Unlit({}, ParseColorHex(s->colorHex));
        draws_.push_back(std::move(item));
    }
    // P1-1 tilemap: every non-empty cell becomes a sprite draw item offset by
    // its cell position (the entity scale sets the cell size).
    auto tileView = world_.ViewAll<SceneTilemap>();
    for (size_t i = 0; i < tileView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneTilemap>(i);
        if (contains(ent)) continue;
        const SceneTilemap* tm = world_.Get<SceneTilemap>(ent);
        if (!tm) continue;
        for (int r = 0; r < tm->rows; ++r) {
            for (int c = 0; c < tm->cols; ++c) {
                const std::string& tex = tm->tiles[static_cast<size_t>(r) * tm->cols + c];
                if (tex.empty()) continue;
                DrawItem item;
                item.ent = ent;
                item.isSprite = true;
                item.spriteTex = tex;
                item.mat = gfx::Material::Unlit({});
                item.tileOffset = {static_cast<float>(c) + 0.5f,
                                   static_cast<float>(r) + 0.5f, 0.0f};
                draws_.push_back(std::move(item));
            }
        }
    }
    // P2-1 ground decals: a flat textured quad on the XZ plane.
    auto decalView = world_.ViewAll<SceneDecal>();
    for (size_t i = 0; i < decalView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneDecal>(i);
        if (contains(ent)) continue;
        const SceneDecal* d = world_.Get<SceneDecal>(ent);
        if (!d || d->texture.empty()) continue;
        DrawItem item;
        item.ent = ent;
        item.isDecal = true;
        item.spriteTex = d->texture;
        item.decalSize = d->size;
        item.mat = gfx::Material::Unlit({});
        item.mat.transparent = true;
        item.mat.tint = {1, 1, 1, d->alpha};
        draws_.push_back(std::move(item));
    }
}

void GameRuntime::ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer) {
    if (item.resolved || item.failed || !cfg_.assets) return;

    if (item.isSprite) {
        gfx::Texture tex = cfg_.assets->LoadTexture(FullAssetPath(item.spriteTex));
        if (!tex.Valid()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: sprite texture '%s' failed to load (skipped)",
                         item.spriteTex.c_str());
            item.failed = true;
            return;
        }
        item.mesh = gfx::Mesh::CreateQuad(renderer, 1.0f, 1.0f, "sprite");
        item.mat.albedo = tex.Handle();
        item.mat.transparent = true; // PNG sprites keep their alpha
        item.resolved = true;
        return;
    }
    if (item.isDecal) {
        gfx::Texture tex = cfg_.assets->LoadTexture(FullAssetPath(item.spriteTex));
        if (!tex.Valid()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: decal texture '%s' failed to load (skipped)",
                         item.spriteTex.c_str());
            item.failed = true;
            return;
        }
        item.mesh = gfx::Mesh::CreatePlane(renderer, item.decalSize, item.decalSize, 1, 1,
                                           "decal");
        item.mat.albedo = tex.Handle();
        item.resolved = true;
        return;
    }

    const std::string& key = item.meshKey;
    const SceneTerrain* terr = key == "terrain" ? world_.Get<SceneTerrain>(item.ent) : nullptr;
    gfx::Mesh mesh = ResolveMeshKey(renderer, key, terr);
    if (!mesh.Valid()) {
        const bool knownPrefix = key.compare(0, 4, "obj:") == 0 || key.compare(0, 5, "gltf:") == 0 ||
                                 key == "cube" || key == "sphere" || key == "plane" ||
                                 key == "terrain";
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                     knownPrefix ? "runtime: mesh '%s' failed to load (skipped)"
                                 : "runtime: meshKey '%s' has no known loader/procedural "
                                   "prefix (skipped)",
                     key.c_str());
        item.failed = true;
        return;
    }
    item.mesh = mesh;

    // LOD chain: level 0 is the base mesh; each entry resolves into a lower-
    // detail level at its distance. A level that fails to load is logged and
    // dropped �?the chain degrades to the levels that resolved.
    if (!item.lod.empty()) {
        item.chain.levels.push_back(item.mesh);
        for (const LodEntry& e : item.lod) {
            gfx::Mesh levelMesh = ResolveMeshKey(renderer, e.meshKey);
            if (!levelMesh.Valid()) {
                NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                             "runtime: LOD mesh '%s' failed to load (skipped; level dropped)",
                             e.meshKey.c_str());
                continue;
            }
            item.chain.levels.push_back(levelMesh);
            item.chain.thresholds.push_back(e.distance);
        }
    }
    item.resolved = true;
}

gfx::Mesh GameRuntime::ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
                                      const SceneTerrain* terrain) {
    gfx::Mesh mesh;
    if (key.compare(0, 4, "obj:") == 0) {
        mesh = cfg_.assets->LoadMeshOBJ(FullAssetPath(key.substr(4)));
    } else if (key.compare(0, 5, "gltf:") == 0) {
        assets::GltfAsset gltf = cfg_.assets->LoadGLTF(FullAssetPath(key.substr(5)));
        if (!gltf.nodes.empty()) mesh = gltf.nodes[0].mesh;
    } else if (key == "cube") {
        mesh = gfx::Mesh::CreateCube(renderer, 1.0f, 1.0f, 1.0f, "cube");
    } else if (key == "sphere") {
        mesh = gfx::Mesh::CreateSphere(renderer, 1.0f, 16, 10, "sphere");
    } else if (key == "plane") {
        mesh = gfx::Mesh::CreatePlane(renderer, 10.0f, 10.0f, 4, 4, "plane");
    } else if (key == "terrain") {
        if (terrain && terrain->heights.size() ==
                           static_cast<size_t>(terrain->segments + 1) *
                               (terrain->segments + 1)) {
            mesh = gfx::Mesh::CreateTerrain(renderer, terrain->segments, terrain->size,
                                            terrain->heights, terrain->heightScale, "terrain");
        } else {
            mesh = gfx::MakeTerrainMesh(renderer);
        }
    } else if (key == "tree") {
        mesh = gfx::MakeTreeMesh(renderer);
    } else if (key == "house") {
        mesh = gfx::MakeHouseMesh(renderer);
    } else if (key == "bush") {
        mesh = gfx::MakeBushMesh(renderer);
    } else if (key == "hero") {
        mesh = gfx::MakeHeroMesh(renderer);
    } else if (key == "wolf") {
        mesh = gfx::MakeWolfMesh(renderer);
    } else if (key.compare(0, 4, "npc:") == 0) {
        // "npc:r,g,b" encodes the villager's tunic tint (0-255 channels).
        int r = 128, g = 128, b = 128;
        std::sscanf(key.c_str() + 4, "%d,%d,%d", &r, &g, &b);
        mesh = gfx::MakeNPCMesh(renderer, {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f});
    } else if (key == "npc") {
        mesh = gfx::MakeNPCMesh(renderer, {0.5f, 0.5f, 0.6f, 1.0f});
    } else if (key == "rock") {
        mesh = gfx::Mesh::CreateSphere(renderer, 0.8f, 10, 7, "rock");
    } else if (key == "water") {
        mesh = gfx::Mesh::CreatePlane(renderer, 20.0f, 20.0f, 8, 8, "water");
    } else if (key == "road") {
        mesh = gfx::Mesh::CreatePlane(renderer, 1.0f, 1.0f, 1, 1, "road");
    }
    return mesh;
}

ecs::Entity GameRuntime::SpawnEntity(const std::string& kind, const math::Vec3& pos,
                                     const std::string& scriptPath) {
    ecs::Entity e = world_.Create();
    world_.Add<script::CTransformBind>(e, script::CTransformBind{pos});
    scriptCtx_.entityKinds[e] = kind;
    if (!scriptPath.empty()) {
        scene::SceneScript s;
        s.backend = "lua";
        s.path = scriptPath;
        world_.Add<SceneScript>(e, s);
        AttachOneScript(e, s);
    }
    return e;
}

void GameRuntime::SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed,
                                  float damage, float life, ecs::Entity caster) {
    Projectile p;
    p.pos = pos;
    p.dir = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    p.speed = speed > 0.0f ? speed : 12.0f;
    p.damage = damage;
    p.life = life > 0.0f ? life : 2.0f;
    p.caster = caster;
    projectiles_.push_back(p);
}

int GameRuntime::MeleeAttack(const math::Vec3& origin, const math::Vec3& dir, float range,
                             float arcDeg, float damage) {
    const math::Vec3 fwd = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    const float cosArc = std::cos(arcDeg * 0.5f * math::kDegToRad);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        const math::Vec3 to = t->pos - origin;
        const float dist = to.Length();
        if (dist > range || dist < 1e-4f) continue;
        if (math::Dot(to / dist, fwd) < cosArc) continue; // outside the arc
        h->hp = std::fmax(0.0f, h->hp - damage);
        ++hits;
    }
    return hits;
}

// Oriented attack box (OBB around Y): damages every SceneHealth entity whose
// position lies inside the yaw-rotated half-extents box. Returns hit count.
int GameRuntime::AttackBox(const math::Vec3& center, const math::Vec3& half, float yaw,
                           float damage) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        const math::Vec3 d = t->pos - center;
        // Rotate the target into box-local space (rotate by -yaw around Y).
        const float lx = c * d.x - s * d.z;
        const float ly = d.y;
        const float lz = s * d.x + c * d.z;
        if (std::fabs(lx) <= half.x && std::fabs(ly) <= half.y && std::fabs(lz) <= half.z) {
            h->hp = std::fmax(0.0f, h->hp - damage);
            ++hits;
        }
    }
    return hits;
}

void GameRuntime::ApplySkillStatuses(ecs::Entity target,
                                     const std::vector<SkillStatus>& statuses) {
    if (statuses.empty() || !world_.Alive(target)) return;
    if (!world_.Has<StatusComponent>(target)) world_.Add<StatusComponent>(target);
    StatusComponent* c = world_.Get<StatusComponent>(target);
    if (!c) return;
    for (const SkillStatus& st : statuses) {
        const uint32_t id = StatusIdByName(st.name);
        if (id != 0) ApplyStatus(*c, id, st.duration, st.magnitude);
    }
}

void GameRuntime::ApplyHit(ecs::Entity target, float damage,
                           const std::vector<SkillStatus>& statuses) {
    if (!world_.Alive(target)) return;
    if (SceneHealth* h = world_.Get<SceneHealth>(target)) {
        h->hp = std::fmax(0.0f, h->hp - damage);
    }
    ApplySkillStatuses(target, statuses);
}

void GameRuntime::TickStatuses(float dt) {
    auto view = world_.ViewAll<StatusComponent>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<StatusComponent>(i);
        StatusComponent* c = world_.Get<StatusComponent>(ent);
        if (!c) continue;
        TickStatus(*c, dt, [this, ent](uint32_t id, float magnitude) {
            SceneHealth* h = world_.Get<SceneHealth>(ent);
            if (!h || h->hp <= 0.0f) return;
            if (id == kStatusRegen) {
                // Regen magnitude is a heal amount per tick.
                h->hp = std::fmin(h->maxHp, h->hp + magnitude);
            } else {
                h->hp = std::fmax(0.0f, h->hp - magnitude);
            }
        });
    }
}

void GameRuntime::TickSkillCooldowns(float dt) {
    for (auto it = skillCooldowns_.begin(); it != skillCooldowns_.end();) {
        // Reconstruct the entity from the stable key; prune destroyed casters.
        const uint64_t key = it->first;
        ecs::Entity e{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFFu)};
        if (!world_.Alive(e)) {
            it = skillCooldowns_.erase(it);
            continue;
        }
        bool any = false;
        for (auto& kv : it->second) {
            kv.second = std::fmax(0.0f, kv.second - dt);
            if (kv.second > 0.0f) any = true;
        }
        if (!any)
            it = skillCooldowns_.erase(it);
        else
            ++it;
    }
}

bool GameRuntime::LoadSkills(const std::string& json, std::string* err) {
    return skills_.Load(json, err);
}

int GameRuntime::CastSkill(const std::string& name, const math::Vec3& origin,
                           const math::Vec3& dir, ecs::Entity caster) {
    const SkillDef* def = skills_.Find(name);
    if (!def) return 0;

    const uint64_t key = EntityKey(caster);
    auto& cds = skillCooldowns_[key];
    const auto cdIt = cds.find(name);
    if (cdIt != cds.end() && cdIt->second > 0.0f) return 0; // on cooldown

    // Mana: when the skill has a cost, the convention is a GameVar "mana"
    // (set by the scene script). Refuse without enough mana, subtract on cast.
    if (def->manaCost > 0.0f) {
        const script::Value mana = scriptCtx_.gameVars.Get("mana");
        const float have =
            mana.type == script::Value::Type::Number ? static_cast<float>(mana.number) : 0.0f;
        if (have < def->manaCost) return 0;
        scriptCtx_.gameVars.Set("mana", script::Value::Num(have - def->manaCost));
    }
    if (def->cooldown > 0.0f) cds[name] = def->cooldown;

    if (def->kind == "projectile") {
        Projectile p;
        p.pos = origin;
        p.dir = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
        p.speed = def->speed;
        p.damage = def->damage;
        p.life = def->life;
        p.range = def->range;
        p.caster = caster;
        p.statuses = def->statuses;
        projectiles_.push_back(p);
        return 1;
    }

    if (def->kind == "melee") {
        const math::Vec3 fwd = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
        const float cosArc = std::cos(def->arcDeg * 0.5f * math::kDegToRad);
        int hits = 0;
        auto view = world_.ViewAll<SceneHealth>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
            const SceneHealth* h = world_.Get<SceneHealth>(ent);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (!h || !t || h->hp <= 0.0f) continue;
            if (ent == caster) continue; // never self-hit
            const math::Vec3 to = t->pos - origin;
            const float dist = to.Length();
            if (dist > def->meleeRange || dist < 1e-4f) continue;
            if (math::Dot(to / dist, fwd) < cosArc) continue;
            ApplyHit(ent, def->damage, def->statuses);
            ++hits;
        }
        return 1; // the cast happened even when no target was in the arc
    }

    // "box": oriented attack box; yaw derived from the facing dir so a
    // script passes a direction vector like every other skill.
    const float yaw = std::atan2(dir.x, dir.z);
    AttackBox(origin, {def->boxHalfX, def->boxHalfY, def->boxHalfZ}, yaw, def->damage);
    return 1;
}

float GameRuntime::SkillCooldownLeft(const std::string& name, ecs::Entity caster) const {
    const auto it = skillCooldowns_.find(EntityKey(caster));
    if (it == skillCooldowns_.end()) return 0.0f;
    const auto cd = it->second.find(name);
    return cd == it->second.end() ? 0.0f : cd->second;
}

bool GameRuntime::HasStatus(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::HasStatus(*c, id) : false;
}

float GameRuntime::StatusMagnitude(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::StatusMagnitude(*c, id) : 0.0f;
}

bool GameRuntime::HasScriptFunction(const std::string& name) const {
    return (hosts_.lua && hosts_.lua->HasFunction(name)) ||
           (hosts_.js && hosts_.js->HasFunction(name));
}

bool GameRuntime::CallScriptFunction(const std::string& name,
                                     const std::vector<script::Value>& args) {
    // Deterministic lookup order: Lua first, then JS (an on_player_join etc.
    // defined by both backends resolves to the Lua one).
    script::IScriptHost* host = nullptr;
    if (hosts_.lua && hosts_.lua->HasFunction(name))
        host = hosts_.lua.get();
    else if (hosts_.js && hosts_.js->HasFunction(name))
        host = hosts_.js.get();
    if (!host) return false;
    scriptCtx_.currentEntity = {};
    const core::Result<script::Value> res = host->Call(name, args);
    if (!res.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                     "runtime: script function %s() failed: %s", name.c_str(),
                     host->LastError().message.c_str());
    }
    return res.Ok();
}

void GameRuntime::TickProjectiles(float dt) {
    for (auto it = projectiles_.begin(); it != projectiles_.end();) {
        Projectile& p = *it;
        const float step = p.speed * dt;
        p.pos += p.dir * step;
        p.traveled += step;
        p.life -= dt;
        // Data-driven skills can bound a projectile by travel distance.
        if (p.range > 0.0f && p.traveled >= p.range) {
            it = projectiles_.erase(it);
            continue;
        }
        // Damage the closest SceneHealth entity within the hit radius. Use a
        // horizontal-radius + vertical-band test (projectiles fly at chest
        // height while targets sit on the ground), so a fireball passing over
        // a grounded enemy still connects.
        float best = p.hitRadius;
        ecs::Entity target;
        auto view = world_.ViewAll<SceneHealth>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
            SceneHealth* h = world_.Get<SceneHealth>(ent);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (!h || !t || h->hp <= 0.0f) continue;
            if (ent == p.caster) continue; // never self-hit
            const math::Vec3 to = t->pos - p.pos;
            const float horiz = std::sqrt(to.x * to.x + to.z * to.z);
            if (horiz < best && std::fabs(to.y) < 2.0f) {
                best = horiz;
                target = ent;
            }
        }
        if (target.IsValid()) {
            ApplyHit(target, p.damage, p.statuses);
            it = projectiles_.erase(it);
            continue;
        }
        if (p.life <= 0.0f) {
            it = projectiles_.erase(it);
            continue;
        }
        ++it;
    }
}

ecs::Entity GameRuntime::FindNamedEntity(const std::string& name) {
    auto view = world_.ViewAll<SceneName>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneName>(i);
        const SceneName* n = world_.Get<SceneName>(ent);
        if (n && n->name == name) return ent;
    }
    return {};
}

std::pair<float, float> GameRuntime::EntityHealth(ecs::Entity ent) const {
    const SceneHealth* h = world_.Get<SceneHealth>(ent);
    return h ? std::pair<float, float>{h->hp, h->maxHp} : std::pair<float, float>{0, 0};
}

float GameRuntime::GameVar(const std::string& name) const {
    script::Value v = scriptCtx_.gameVars.Get(name);
    return v.type == script::Value::Type::Number ? static_cast<float>(v.number) : 0.0f;
}

void GameRuntime::Draw(gfx::Renderer& renderer, const gfx::Camera& camera) {
    if (!running_ || !cfg_.assets) return; // sim-only runtime draws nothing
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    gfx::Camera cam = camera;
    bool usedCameraEntity = false;
    world_.ViewAll<SceneCamera, SceneTransform>().ForEach(
        [&](ecs::Entity, const SceneCamera& c, const SceneTransform& t) {
            if (usedCameraEntity) return;
            usedCameraEntity = true;
            cam.position = t.pos;
            cam.target = t.pos + t.rot.Rotate({0, 0, -1});
            cam.up = {0, 1, 0};
            cam.ortho = c.ortho;
            cam.fovY = c.fov * math::kDegToRad;
        });
    // Project at the ACTIVE scene viewport's aspect (a dock sub-rect in the
    // editor, the full target in the standalone player) so the runtime render
    // matches whatever rasterization rect the host set up - otherwise the
    // playtest FOV would differ from the edit-mode viewport.
    renderer.SetCamera(cam, renderer.SceneAspect());
    // Scripts may have spawned/despawned sprite entities since the last frame.
    BuildDrawList();
    // P2-3: sprites render back-to-front by their sortOrder component (2D
    // games); 3D depth-tested meshes are unaffected by the stable order.
    std::vector<size_t> order(draws_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const SceneSortOrder* sa = world_.Get<SceneSortOrder>(draws_[a].ent);
        const SceneSortOrder* sb = world_.Get<SceneSortOrder>(draws_[b].ent);
        return (sa ? sa->z : 0.0f) < (sb ? sb->z : 0.0f);
    });
    size_t dead = 0;
    for (size_t idx : order) {
        DrawItem& item = draws_[idx];
        if (!world_.Alive(item.ent)) {
            ++dead; // scripts can Despawn entities mid-playtest
            continue;
        }
        if (hiddenEntities_.count(EntityKey(item.ent)) != 0) continue; // SetVisible(false)
        if (!item.resolved) ResolveDrawItem(item, renderer);
        if (!item.resolved || item.failed) continue;
        if (!world_.Get<SceneTransform>(item.ent)) continue;
        math::Mat4 model = LocalToWorld(item.ent);
        if (item.tileOffset.LengthSq() > 0.0f)
            model = model * math::Mat4::Translation(item.tileOffset);
        if (item.isDecal) {
            // Lift the quad a hair above the surface it projects onto so depth
            // testing keeps it visible (no z-fighting on flat ground).
            model = model * math::Mat4::Translation({0.0f, 0.02f, 0.0f});
        }
        const math::Vec3 worldPos{model.m[12], model.m[13], model.m[14]};
        if (item.isSprite) {
            // Flip mirrors the quad around its center: a negative local scale
            // keeps the texture upright and needs no UV/shader changes.
            if (item.flipX || item.flipY)
                model = model * math::Mat4::Scale({item.flipX ? -1.0f : 1.0f,
                                                   item.flipY ? -1.0f : 1.0f, 1.0f});
            renderer.DrawMesh(item.mesh, item.mat, model);
        } else {
            renderer.DrawMesh(SelectLodMesh(item.mesh, item.chain, worldPos, camera.position),
                              item.mat, model);
        }
    }
    // Skill projectiles (fireballs): bright glowing orbs.
    if (!projectiles_.empty()) {
        if (!fireballMesh_.Valid()) fireballMesh_ = gfx::MakeFireballMesh(renderer);
        gfx::Material fmat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
        fmat.emissiveIntensity = 2.5f;
        for (const Projectile& p : projectiles_) {
            renderer.DrawMesh(fireballMesh_, fmat, math::Mat4::Translation(p.pos));
        }
    }
    // Compact when a fifth of the draw list belongs to dead entities.
    if (dead && dead * 5 > draws_.size()) {
        draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                    [this](const DrawItem& i) { return !world_.Alive(i.ent); }),
                     draws_.end());
    }

    // 2D script canvas: a global on_render() handler draws the game with the
    // DrawRect/DrawRectOutline/DrawText bindings (design units 1280x720). The
    // runtime flushes the buffer into the renderer's 2D overlay so 2D games
    // (e.g. the PvZ project) need zero C++ gameplay code.
    if (hosts_.lua || hosts_.js) {
        scriptCtx_.draw2d = &draw2d_;
        // Snapshot the host's live 2D mapping (Set2DViewport) so on_update's
        // InputMousePos() and UI hit-tests keep design coordinates between
        // renders (the renderer resets its 2D viewport after the frame).
        uiScale_ = renderer.UIScale();
        uiOffset_ = renderer.UI2DOffset();
        scriptCtx_.screenToUi = [this](const math::Vec2& p) {
            return (p - uiOffset_) / uiScale_;
        };
        draw2d_.clear();
        scriptCtx_.currentEntity = {};
        // Global handlers can be defined by either backend; Lua wins ties
        // (deterministic order).
        for (script::IScriptHost* h : {hosts_.lua.get(), hosts_.js.get()}) {
            if (!h || !h->HasFunction("on_render")) continue;
            const core::Result<script::Value> res = h->Call("on_render", {});
            if (!res.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                             "runtime: on_render() failed: %s",
                             h->LastError().message.c_str());
            }
        }
        scriptCtx_.draw2d = nullptr;
        FlushDraw2D(renderer);
    }

    // Data-driven UI document: drawn by DrawUI() AFTER the frame is composited
    // (menus/HUD keep authored colors instead of being tone-mapped with the
    // 3D scene). Button clicks are edge-triggered per frame so scripts can
    // query UIClicked(name) from on_update on the next tick.
    uiClickedNames_.clear();
    if (uiDoc_) {
        if (cfg_.input && cfg_.input->MousePressed(platform::MouseButton::Left)) {
            math::Vec2 p = cfg_.input->MousePos();
            if (scriptCtx_.screenToUi) p = scriptCtx_.screenToUi(p);
            if (ui::UiNode* hit = uiDoc_->HitTest(p);
                hit && hit->type == ui::UiNodeType::Button) {
                uiClickedNames_.insert(hit->name);
            }
        }
    }
}

void GameRuntime::DrawUI(gfx::Renderer& renderer) {
    if (!running_ || !cfg_.assets || !uiDoc_ || !cfg_.font2d.Valid()) return;
    scriptCtx_.screenToUi = [this](const math::Vec2& p) {
        return (p - uiOffset_) / uiScale_;
    };
    uiDoc_->Draw(renderer, cfg_.font2d);
}

void GameRuntime::LoadLocales() {
    loc_ = core::Localization();
    if (cfg_.localesDir.empty()) return;
    std::vector<std::string> files;
    ListFilesRecursive(cfg_.localesDir, "", files);
    size_t loaded = 0;
    for (const std::string& rel : files) {
        if (!HasSuffix(rel, ".json")) continue;
        std::string text = ReadScript(FullScriptPath(rel));
        if (text.empty()) continue;
        std::string err;
        if (!loc_.LoadTable(text, &err)) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: locale '%s' failed to load: %s", rel.c_str(), err.c_str());
            continue;
        }
        ++loaded;
    }
    if (loaded > 0) {
        std::string langs;
        for (const std::string& l : loc_.Languages()) {
            if (!langs.empty()) langs += ",";
            langs += l;
        }
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                     "runtime: loaded %zu locale file(s), languages: %s", loaded,
                     langs.c_str());
    }
}

math::Mat4 GameRuntime::LocalToWorld(ecs::Entity e) const {
    math::Mat4 m = math::Mat4::Identity();
    for (int depth = 0; depth < 8 && e.IsValid(); ++depth) {
        const SceneTransform* t = world_.Get<SceneTransform>(e);
        if (!t) break;
        m = math::Mat4::Translation(t->pos) * t->rot.ToMat4() * math::Mat4::Scale(t->scale) * m;
        const SceneParentLink* link = world_.Get<SceneParentLink>(e);
        e = link ? link->parent : ecs::Entity{};
    }
    return m;
}

void GameRuntime::FlushDraw2D(gfx::Renderer& renderer) {
    for (const script::Draw2DCmd& c : draw2d_) {
        switch (c.kind) {
            case script::Draw2DCmd::Kind::Rect:
                renderer.DrawQuad({c.x, c.y}, {c.w, c.h}, {c.r, c.g, c.b, c.a},
                                  c.texture);
                break;
            case script::Draw2DCmd::Kind::RectOutline:
                renderer.DrawRectOutline({c.x, c.y, c.w, c.h}, c.thickness,
                                         {c.r, c.g, c.b, c.a});
                break;
            case script::Draw2DCmd::Kind::Text:
                if (cfg_.font2d.Valid())
                    renderer.DrawText(cfg_.font2d, c.text, {c.x, c.y}, c.size,
                                      {c.r, c.g, c.b, c.a}, c.centerX, c.centerY);
                break;
        }
    }
}

void GameRuntime::Tick(float dt) {
    if (!running_) return;
    // P1-2 debugger: a breakpoint hit latches the host's paused flag during a
    // script call; stop advancing the simulation so the editor can inspect and
    // step before resuming.
    if (hosts_.lua && hosts_.lua->DebuggerPaused()) return;

    // Both backends share the engine-injected simulated clock.
    if (hosts_.lua) hosts_.lua->SetSimClock(simTime_);
    if (hosts_.js) hosts_.js->SetSimClock(simTime_);
    for (ScriptInst& inst : scripts_) {
        if (!world_.Alive(inst.ent)) continue;
        if (inst.onUpdate == 0) continue; // this chunk defines no on_update
        CallEntityFunctionHandle(inst, inst.onUpdate, "on_update",
                                 {EntityToValue(inst.ent), script::Value::Num(dt)});
    }

    size_t deadTrees = 0;
    for (BtInst& inst : trees_) {
        if (!world_.Alive(inst.ent)) {
            ++deadTrees; // scripts can Despawn entities mid-playtest
            continue;
        }
        bt::Context ctx(scriptCtx_.gameVars, &inst.board);
        ctx.entity = EntityKey(inst.ent);
        ctx.dt = dt;
        // run_script / script_bool nodes execute a named global function on
        // the tree entity's script backend (wired here for the first time; the
        // hook previously existed but nothing set it).
        ctx.callScript = [this, &inst](const std::string& fn, uint64_t ent) {
            return CallScriptOnTree(inst, fn, ent);
        };
        ctx.timers.swap(inst.timers); // carry over accumulated wait/cooldown state
        inst.tree->Tick(ctx);
        ctx.timers.swap(inst.timers); // persist it for the next tick
        inst.activePath = ctx.activePath; // debug highlight: deepest node that ran
    }
    // Compact when a fifth of the trees belong to dead entities.
    if (deadTrees && deadTrees * 5 > trees_.size()) {
        trees_.erase(std::remove_if(trees_.begin(), trees_.end(),
                                    [this](const BtInst& i) { return !world_.Alive(i.ent); }),
                     trees_.end());
    }

    // Fixed-step physics: accumulate the frame delta and advance the world at
    // 60 Hz so collision resolution and scripts stay deterministic regardless
    // of frame rate. Cap the catch-up to avoid a spiral of death after a hitch.
    physicsAccum_ += dt;
    constexpr float kPhysicsStep = 1.0f / 60.0f;
    int physicsSteps = 0;
    while (physicsAccum_ >= kPhysicsStep && physicsSteps < 4) {
        physics_->Step(kPhysicsStep, math::Vec3{0.0f, kGravityY, 0.0f});
        physicsAccum_ -= kPhysicsStep;
        ++physicsSteps;
    }
    if (physicsSteps == 4) physicsAccum_ = 0.0f;
    SyncSceneBodies();
    TickTweens(dt);
    TickStatuses(dt);
    TickSkillCooldowns(dt);
    TickProjectiles(dt);
    simTime_ += dt;

    // ChangeScene deferral: a script's ChangeScene call must not destroy the
    // Lua host mid-call, so the swap happens here, after every script handler
    // has returned. Start() resets the world/host with the same config.
    if (!pendingScene_.empty()) {
        const std::string path = pendingScene_;
        pendingScene_.clear();
        const std::string text = ReadScript(FullScriptPath(path));
        if (text.empty()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Error,
                         "runtime: ChangeScene cannot read '%s'", path.c_str());
        } else {
            core::Status st = Start(text, cfg_);
            if (!st.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Error,
                             "runtime: ChangeScene to '%s' failed: %s", path.c_str(),
                             st.Error().c_str());
            } else {
                NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Info,
                             "runtime: changed scene to '%s'", path.c_str());
            }
        }
    }
}

script::Value GameRuntime::EntityBlackboardValue(const ecs::Entity& ent,
                                                 const std::string& key) const {
    for (const BtInst& inst : trees_) {
        if (inst.ent == ent) return inst.board.Get(EntityKey(ent), key);
    }
    return script::Value::Nil();
}

std::string GameRuntime::ActiveTreePath(const ecs::Entity& ent) const {
    for (const BtInst& inst : trees_) {
        if (inst.ent == ent) return inst.activePath;
    }
    return {};
}

gfx::Mesh GameRuntime::MeshForEntity(const ecs::Entity& ent,
                                     const gfx::Camera& camera) const {
    for (const DrawItem& item : draws_) {
        if (item.ent != ent || !item.resolved || item.failed) continue;
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!t) return gfx::Mesh{};
        return SelectLodMesh(item.mesh, item.chain, t->pos, camera.position);
    }
    return gfx::Mesh{};
}

std::string GameRuntime::FullScriptPath(const std::string& path) const {
    if (path.empty() || cfg_.scriptBaseDir.empty()) return path;
    return cfg_.scriptBaseDir + "/" + path;
}

std::string GameRuntime::FullAssetPath(const std::string& path) const {
    if (path.empty() || cfg_.assetBaseDir.empty()) return path;
    // Absolute paths (drive letter or leading separator) pass through unchanged.
    if (path.size() >= 2 && path[1] == ':') return path;
    if (path[0] == '/') return path;
    return cfg_.assetBaseDir + "/" + path;
}

std::string GameRuntime::ReadScript(const std::string& path) const {
    if (cfg_.readScript) return cfg_.readScript(path);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::string out;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return out;
}

} // namespace neon::scene
