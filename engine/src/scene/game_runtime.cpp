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
#include "neon/core/profiler.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/gfx/terrain.hpp"
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

// Two materials render identically (same shader/textures/scalars/flags), so
// their entities can share one instanced draw. Exact float equality is fine:
// materials are copied from the same resolved source, and materials that
// merely have numerically identical values are safe to batch.
bool SameMaterial(const gfx::Material& a, const gfx::Material& b) {
    return a.shader.id == b.shader.id && a.albedo.id == b.albedo.id &&
           a.metallicRoughness.id == b.metallicRoughness.id &&
           a.occlusion.id == b.occlusion.id && a.emissive.id == b.emissive.id &&
           a.tint.r == b.tint.r && a.tint.g == b.tint.g && a.tint.b == b.tint.b &&
           a.tint.a == b.tint.a && a.shininess == b.shininess && a.metallic == b.metallic &&
           a.roughness == b.roughness && a.aoStrength == b.aoStrength &&
           a.emissiveIntensity == b.emissiveIntensity && a.lit == b.lit &&
           a.transparent == b.transparent && a.doubleSided == b.doubleSided &&
           a.alphaTest == b.alphaTest && a.alphaCutoff == b.alphaCutoff;
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
    // Data-driven skills table (M1): hosts pass the skills.json text.
    if (!cfg_.skillsJson.empty()) {
        std::string err;
        if (!skills_.Load(cfg_.skillsJson, &err))
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: skills.json rejected (%s); CastSkill table empty",
                         err.c_str());
    }
    // Create the physics world: Jolt when requested and compiled, else the
    // deterministic custom solver (server / headless tests). A "plugin:<name>"
    // backend (G5-1) loads the solver from a native middleware DLL/SO under
    // cfg_.pluginBaseDir/plugins — swappable without relinking. The owning
    // PhysicsBackend is kept alive until this runtime is destroyed (it owns the
    // DLL), and is declared before physics_ so the world dies before the library.
    physics_ = std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
        new physics::World(), [](physics::World* w) { delete w; });
#ifdef NEON_ENABLE_JOLT
    if (cfg_.physicsBackend == "jolt") {
        physics_ = std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
            new physics::JoltWorld(), [](physics::World* w) { delete w; });
    }
#endif
    if (cfg_.physicsBackend.rfind("plugin:", 0) == 0 && !cfg_.pluginBaseDir.empty()) {
        const std::string backendName = cfg_.physicsBackend.substr(7);
        std::unique_ptr<plugin::PhysicsBackend> backend =
            plugin::LoadNativePhysicsBackend(backendName, cfg_.pluginBaseDir);
        if (backend) {
            std::unique_ptr<physics::World, std::function<void(physics::World*)>> world =
                backend->CreateWorld();
            if (world) {
                pluginPhysics_ = std::move(backend); // keep the DLL resident
                physics_ = std::move(world);
            } else {
                NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                             "runtime: plugin physics backend '%s' created no world; "
                             "falling back to custom",
                             backendName.c_str());
            }
        } else {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: no native physics backend '%s' under '%s/plugins'; "
                         "falling back to custom",
                         backendName.c_str(), cfg_.pluginBaseDir.c_str());
        }
    }
    NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Info,
                 "runtime: physics backend '%s' (%zu rigid bodies cap)",
                 cfg_.physicsBackend.c_str(), physics_->BodyCount());
    compReg_ = ComponentRegistry{};
    RegisterBuiltinComponents(compReg_, /*assets=*/nullptr);
    LoadPrefabs(); // scene entities may reference prefabs by name (packed games)
    LoadLocales(); // Loc() string tables (best effort; missing dir = no-op)
    auto inst = Instantiate(world_, parsed.Value(), prefs_, compReg_);
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
    scriptCtx_.spawnPrefab = [this](const std::string& name, const math::Vec3& pos) {
        return SpawnPrefab(name, pos);
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
    scriptCtx_.setScale = [this](ecs::Entity e, const math::Vec3& s) {
        if (SceneTransform* t = world_.Get<SceneTransform>(e)) t->scale = s;
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
    // M1 gameplay hooks: per-entity animation + HUD anchors + floating text.
    scriptCtx_.playAnimation = [this](ecs::Entity e, const std::string& clip, bool loop,
                                      float fade, float speed) {
        return PlayAnimation(e, clip, loop, fade, speed);
    };
    scriptCtx_.animProgress = [this](ecs::Entity e) { return AnimationProgress(e); };
    scriptCtx_.animFinished = [this](ecs::Entity e) { return AnimationFinished(e); };
    scriptCtx_.worldToScreen = [this](const math::Vec3& w, float& ox, float& oy) {
        return WorldToScreen(w, ox, oy);
    };
    scriptCtx_.spawnFloatText = [this](const math::Vec3& w, const std::string& t, bool crit,
                                       float life) { SpawnFloatText(w, t, crit, life); };
    scriptCtx_.setEntityPlate = [this](ecs::Entity e, const std::string& name, float hp) {
        SetEntityPlate(e, name, hp);
    };
    scriptCtx_.screenAnchors = [this]() -> script::Value {
        auto vec3 = [](const math::Vec3& v) {
            script::Value t = script::Value::Tbl();
            t.table->fields.emplace_back("x", script::Value::Num(v.x));
            t.table->fields.emplace_back("y", script::Value::Num(v.y));
            t.table->fields.emplace_back("z", script::Value::Num(v.z));
            return t;
        };
        script::Value arr = script::Value::Tbl();
        for (const ScreenAnchor& a : screenAnchors_) {
            script::Value t = script::Value::Tbl();
            // entity key -> handle table {id, gen}
            const uint32_t id = static_cast<uint32_t>(a.entity >> 32);
            const uint32_t gen = static_cast<uint32_t>(a.entity & 0xFFFFFFFFu);
            script::Value eh = script::Value::Tbl();
            eh.table->fields.emplace_back("id", script::Value::Num(id));
            eh.table->fields.emplace_back("gen", script::Value::Num(gen));
            t.table->fields.emplace_back("entity", std::move(eh));
            t.table->fields.emplace_back("x", script::Value::Num(a.x));
            t.table->fields.emplace_back("y", script::Value::Num(a.y));
            t.table->fields.emplace_back("onscreen", script::Value::Bool(a.onscreen));
            t.table->fields.emplace_back("world", vec3(a.world));
            arr.table->array.push_back(std::move(t));
        }
        return arr;
    };
    scriptCtx_.entityPlates = [this]() -> script::Value {
        script::Value t = script::Value::Tbl();
        for (const auto& kv : plates_) {
            script::Value p = script::Value::Tbl();
            p.table->fields.emplace_back("name", script::Value::Str(kv.second.name));
            p.table->fields.emplace_back("hp", script::Value::Num(kv.second.hpFrac));
            // Script-friendly key: "id_gen" (matches what ScreenAnchors exposes
            // as the entity handle, so HUD scripts can join the two lists).
            const uint32_t id = static_cast<uint32_t>(kv.first >> 32);
            const uint32_t gen = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
            t.table->fields.emplace_back(std::to_string(id) + "_" + std::to_string(gen),
                                         std::move(p));
        }
        return t;
    };
    scriptCtx_.floatTexts = [this]() -> script::Value {
        auto vec3 = [](const math::Vec3& v) {
            script::Value t = script::Value::Tbl();
            t.table->fields.emplace_back("x", script::Value::Num(v.x));
            t.table->fields.emplace_back("y", script::Value::Num(v.y));
            t.table->fields.emplace_back("z", script::Value::Num(v.z));
            return t;
        };
        script::Value arr = script::Value::Tbl();
        for (const FloatText& f : floatTexts_) {
            script::Value t = script::Value::Tbl();
            t.table->fields.emplace_back("world", vec3(f.world));
            t.table->fields.emplace_back("text", script::Value::Str(f.text));
            t.table->fields.emplace_back("crit", script::Value::Bool(f.crit));
            t.table->fields.emplace_back("age", script::Value::Num(f.age));
            t.table->fields.emplace_back("life", script::Value::Num(f.life));
            arr.table->array.push_back(std::move(t));
        }
        return arr;
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

    // Runtime plugins: gameplay/system modules in <scriptBaseDir>/plugins
    // (Lua or JS). They get their own isolated hosts sharing the engine
    // bindings; failures are logged per plugin, never fatal.
    plugins_ = std::make_unique<plugin::RuntimePluginManager>();
    plugin::RuntimePluginManager::Config pc;
    pc.baseDir = cfg_.scriptBaseDir.empty() ? "." : cfg_.scriptBaseDir;
    pc.readFile = [this](const std::string& p) { return ReadScript(p); };
    pc.ctx = &scriptCtx_;
    pc.gameVars = &scriptCtx_.gameVars;
    pc.rngSeed = cfg_.rngSeed ? cfg_.rngSeed : 1u;
    plugins_->Load(pc);
    plugins_->Start();

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
    poseHistory_.clear();
    autoRewindTicks_ = 0;
    signalHandlers_.clear();
    pendingScene_.clear();
    loadedScripts_.clear();
    scriptFailed_.clear();
    if (plugins_) {
        plugins_->Stop();
        plugins_.reset();
    }
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
    // M1: sync per-entity animation overrides into their draw items (existing
    // items get name updates; resolved clip pointers re-resolve on change).
    for (DrawItem& d : draws_) {
        const SceneAnimOverride* ov = world_.Get<SceneAnimOverride>(d.ent);
        if (!ov || !ov->active) {
            d.animHasOverride = false;
            d.animClip = nullptr;
            d.animName.clear();
            continue;
        }
        if (d.animName != ov->clip) {
            d.animName = ov->clip;
            d.animClip = nullptr; // re-resolve in TickAnimations
            d.animLoop = ov->loop;
            d.animSpeed = ov->speed;
            d.animFadeTotal = ov->crossFade;
        }
        d.animHasOverride = true;
    }
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
        // G2-3 chunked-LOD terrain: a terrain entity with chunkGridDiv > 0
        // becomes gridDiv x gridDiv patch draw items (each with its own LodChain
        // + per-patch distance LOD), instead of one monolithic terrain mesh.
        const SceneTerrain* terr = world_.Get<SceneTerrain>(ent);
        if (m->meshKey == "terrain" && terr && terr->chunkGridDiv > 0 &&
            !terr->heights.empty()) {
            const int gd = math::IClamp(terr->chunkGridDiv, 1, 16);
            const float half = terr->size * 0.5f;
            const float chunkSize = terr->size / static_cast<float>(gd);
            for (int gz = 0; gz < gd; ++gz) {
                for (int gx = 0; gx < gd; ++gx) {
                    DrawItem c;
                    c.ent = ent;
                    c.meshKey = "terrain";
                    c.isTerrainChunk = true;
                    c.chunkGridX = gx;
                    c.chunkGridZ = gz;
                    c.chunkGridDiv = gd;
                    c.chunkCenterLocal = {-half + (gx + 0.5f) * chunkSize, 0.0f,
                                          -half + (gz + 0.5f) * chunkSize};
                    c.mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
                    c.mat.doubleSided = true; // hide the LOD-crack skirt
                    draws_.push_back(std::move(c));
                }
            }
            continue;
        }
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
        // M1: carry a live animation override onto a newly-tracked item.
        if (const SceneAnimOverride* ov = world_.Get<SceneAnimOverride>(ent);
            ov && ov->active) {
            item.animHasOverride = true;
            item.animName = ov->clip;
            item.animLoop = ov->loop;
            item.animSpeed = ov->speed;
            item.animFadeTotal = ov->crossFade;
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
        // 2D sprites are lit so the scene's ambient/sun/lights affect them.
        item.mat = gfx::Material::Lit({}, ParseColorHex(s->colorHex), 8.0f);
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

    // G2-3 chunked-LOD terrain patch: build that chunk's LodChain and remember
    // its local centre for per-patch distance LOD selection in Draw().
    if (item.isTerrainChunk) {
        const SceneTerrain* terr = world_.Get<SceneTerrain>(item.ent);
        if (!terr || terr->heights.empty()) {
            item.failed = true;
            return;
        }
        gfx::TerrainChunkMesh chunk = gfx::BuildTerrainChunk(
            renderer, terr->heights, terr->segments, terr->size, terr->heightScale,
            item.chunkGridDiv, item.chunkGridX, item.chunkGridZ, terr->chunkLodLevels,
            terr->chunkBaseSubdiv);
        if (chunk.chain.levels.empty()) {
            item.failed = true;
            return;
        }
        item.mesh = chunk.chain.levels[0];
        item.chain = chunk.chain;
        item.mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
        item.mat.doubleSided = true; // hide the LOD-crack skirt
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

    // Animated skinned glTF: resolve the full model (all skinned mesh parts +
    // skeleton + clips) so Draw() can use bone matrices. LOD chains are not
    // supported for skinned models (the file's parts are the model).
    if (mesh.Skinned()) {
        core::Result<SkinnedModel> sm =
            LoadSkinnedModel(*cfg_.assets, FullAssetPath(key.substr(5)));
        if (!sm.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: skinned model '%s' failed to resolve: %s", key.c_str(),
                         sm.Error().c_str());
            item.failed = true;
            return;
        }
        item.skinned = std::make_shared<SkinnedModel>(std::move(sm.Value()));
        item.resolved = true;
        return;
    }

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

gfx::Mesh GameRuntime::VegetationMesh(gfx::Renderer& renderer, const std::string& meshKey) {
    return ResolveMeshKey(renderer, meshKey);
}

void GameRuntime::DrawVegetation(gfx::Renderer& renderer, const gfx::Camera& camera) {
    if (!cfg_.assets) return;
    // Prune cache entries whose terrain entity has despawned since the last
    // frame (script Spawn/Despawn).
    for (auto it = vegCache_.begin(); it != vegCache_.end();) {
        if (!world_.Alive(it->second.ent)) {
            it = vegCache_.erase(it);
        } else {
            ++it;
        }
    }

    auto view = world_.ViewAll<SceneTerrain>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneTerrain>(i);
        const SceneTerrain* terr = world_.Get<SceneTerrain>(ent);
        if (!terr || terr->vegMeshKey.empty() || terr->vegCount == 0 || terr->heights.empty())
            continue;

        const uint64_t key = EntityKey(ent);
        auto it = vegCache_.find(key);
        VegField* f = nullptr;
        if (it != vegCache_.end()) {
            f = &it->second;
        } else {
            f = &(vegCache_[key] = VegField{});
            f->ent = ent;
        }

        if (!f->built) {
            f->mesh = ResolveMeshKey(renderer, terr->vegMeshKey);
            f->size = std::max(terr->vegSize, 0.05f);
            f->impostorDistance = std::max(terr->vegImpostorDistance, 1.0f);
            f->mat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
            f->mat.doubleSided = true;
            f->impostorMat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
            f->impostorMat.doubleSided = true;
            if (f->mesh.Valid()) {
                // Size the billboard card from the plant mesh's bounds.
                const math::AABB& b = f->mesh.Bounds();
                float bw = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * f->size;
                float bh = (b.max.y - b.min.y) * f->size;
                if (bw < 0.1f) bw = f->size;
                if (bh < 0.1f) bh = f->size * 2.0f;
                f->impostor = gfx::MakeImpostorQuad(renderer, bw, bh, {0.16f, 0.48f, 0.16f, 1.0f});
            }
            core::Rng rng(terr->vegSeed ? terr->vegSeed : 1u);
            gfx::VegetationConfig vcfg;
            vcfg.count = terr->vegCount;
            vcfg.minHeight = terr->vegMinHeight;
            vcfg.maxHeight = terr->vegMaxHeight;
            vcfg.maxSlope = terr->vegMaxSlope;
            vcfg.size = f->size;
            f->positions = gfx::ScatterVegetation(terr->heights, terr->segments, terr->size,
                                                  terr->heightScale, vcfg, rng);
            f->built = true;
        }
        if (f->failed || !f->mesh.Valid() || f->positions.empty()) continue;

        const math::Mat4 terrainModel = CachedLocalToWorld(ent);
        std::vector<math::Mat4> plantModels;
        std::vector<math::Mat4> impostorModels;
        plantModels.reserve(f->positions.size());
        impostorModels.reserve(f->positions.size() / 4 + 1);
        for (const math::Vec3& local : f->positions) {
            const math::Vec3 p = terrainModel.TransformPoint(local);
            const float dist = math::Distance(p, camera.position);
            if (dist > f->impostorDistance) {
                // Y-yaw billboard so the card faces the camera on the XZ plane.
                const float yaw = std::atan2(camera.position.x - p.x, camera.position.z - p.z);
                impostorModels.push_back(math::Mat4::Translation(p) *
                                         math::Quat::FromEuler(0.0f, yaw, 0.0f).ToMat4());
            } else {
                plantModels.push_back(math::Mat4::Translation(p) *
                                      math::Mat4::Scale({f->size, f->size, f->size}));
            }
        }
        if (!plantModels.empty())
            renderer.DrawMeshInstanced(f->mesh, f->mat, plantModels.data(),
                                       static_cast<uint32_t>(plantModels.size()));
        if (!impostorModels.empty())
            renderer.DrawMeshInstanced(f->impostor, f->impostorMat, impostorModels.data(),
                                       static_cast<uint32_t>(impostorModels.size()));
    }
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

ecs::Entity GameRuntime::SpawnPrefab(const std::string& name, const math::Vec3& pos) {
    if (name.empty() || !prefs_.Get(name).Ok()) {
        NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                     "runtime: SpawnPrefab: unknown prefab '%s'", name.c_str());
        return {};
    }
    // Build a one-entity scene that references the prefab and overrides the
    // transform, then reuse the exact Instantiate pipeline (prefab expansion,
    // component factories, custom-component SceneData storage).
    static uint64_t spawnCounter = 1;
    const std::string uniqueName = name + "_" + std::to_string(spawnCounter++);
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    core::Json ent;
    ent.type_ = core::Json::Type::Object;
    core::Json nameJ;
    nameJ.type_ = core::Json::Type::String;
    nameJ.string_ = uniqueName;
    ent.object_["name"] = std::move(nameJ);
    core::Json prefabJ;
    prefabJ.type_ = core::Json::Type::String;
    prefabJ.string_ = name;
    ent.object_["prefab"] = std::move(prefabJ);
    core::Json comps;
    comps.type_ = core::Json::Type::Object;
    core::Json tr;
    tr.type_ = core::Json::Type::Object;
    core::Json posArr;
    posArr.type_ = core::Json::Type::Array;
    for (float v : {pos.x, pos.y, pos.z}) {
        core::Json num;
        num.type_ = core::Json::Type::Number;
        num.number_ = v;
        posArr.array_.push_back(std::move(num));
    }
    tr.object_["pos"] = std::move(posArr);
    comps.object_["transform"] = std::move(tr);
    ent.object_["components"] = std::move(comps);
    arr.array_.push_back(std::move(ent));
    root.object_["entities"] = std::move(arr);

    auto parsed = SceneFile::Parse(core::JsonWriter::Write(root));
    if (!parsed.Ok()) return {};
    auto inst = Instantiate(world_, parsed.Value(), prefs_, compReg_);
    if (!inst.Ok() || inst.Value() != 1) return {};

    // Locate the created entity by its unique name, then attach its script
    // components (AttachScripts only runs at Start for scene entities; the
    // prefab's on_start fires here, with its custom components already set).
    ecs::Entity out;
    auto names = world_.ViewAll<SceneName>();
    for (size_t i = 0; i < names.Size(); ++i) {
        ecs::Entity ent2 = world_.EntityAt<SceneName>(i);
        const SceneName* sn = world_.Get<SceneName>(ent2);
        if (sn && sn->name == uniqueName) {
            out = ent2;
            break;
        }
    }
    if (!out.IsValid()) return {};
    if (const SceneScript* s = world_.Get<SceneScript>(out)) AttachOneScript(out, *s);
    if (const SceneScripts* list = world_.Get<SceneScripts>(out)) {
        for (const SceneScript& s : list->items) AttachOneScript(out, s);
    }
    return out;
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

// G3-4: the position a hit test uses for `ent` - the pose it had
// `rewindTicks` fixed ticks ago when history exists, else its CURRENT pose
// (fresh spawns / shallow history degrade gracefully to the plain path).
bool GameRuntime::LagCompPosition(ecs::Entity e, uint32_t rewindTicks,
                                  math::Vec3& out) const {
    if (rewindTicks > 0 && !poseHistory_.empty()) {
        const size_t n = poseHistory_.size();
        const size_t idx = rewindTicks >= n ? 0 : n - 1 - rewindTicks;
        const auto& snap = poseHistory_[idx];
        const auto it = snap.find(EntityKey(e));
        if (it != snap.end()) {
            out = it->second;
            return true;
        }
    }
    return false;
}

// Shared arc-hit test used by MeleeAttack (auto rewind), MeleeAttackLagComp
// (explicit rewind) and CastSkill's melee skill. Damage always lands on the
// CURRENT entity; only the position test is rewound.
int GameRuntime::MeleeAttackImpl(const math::Vec3& origin, const math::Vec3& dir, float range,
                                 float arcDeg, float damage, uint32_t rewindTicks,
                                 ecs::Entity exclude,
                                 const std::vector<SkillStatus>& statuses) {
    const math::Vec3 fwd = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    const float cosArc = std::cos(arcDeg * 0.5f * math::kDegToRad);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        if (ent == exclude) continue; // never self-hit
        math::Vec3 hitPos = t->pos;
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, hitPos);
        const math::Vec3 to = hitPos - origin;
        // Horizontal-range + vertical-band hit test (like projectiles): the
        // attack originates at chest height while targets sit on the ground,
        // so a 3D distance would shrink the effective reach (a wolf at 1.8m
        // horizontal, 1.5m below the origin, is ~2.3m away in 3D and misses a
        // 2.2m swing).
        const float horiz = std::sqrt(to.x * to.x + to.z * to.z);
        if (horiz > range || horiz < 1e-4f) continue;
        if (std::fabs(to.y) > 2.0f) continue;
        const math::Vec3 toDir{to.x / horiz, 0.0f, to.z / horiz};
        const math::Vec3 fwdDir{fwd.x, 0.0f, fwd.z};
        if (math::Dot(toDir, fwdDir) < cosArc) continue; // outside the arc
        if (statuses.empty())
            h->hp = std::fmax(0.0f, h->hp - damage);
        else
            ApplyHit(ent, damage, statuses);
        ++hits;
    }
    return hits;
}

int GameRuntime::MeleeAttack(const math::Vec3& origin, const math::Vec3& dir, float range,
                             float arcDeg, float damage) {
    return MeleeAttackImpl(origin, dir, range, arcDeg, damage, autoRewindTicks_);
}

int GameRuntime::MeleeAttackLagComp(const math::Vec3& origin, const math::Vec3& dir, float range,
                                    float arcDeg, float damage, uint32_t rewindTicks) {
    return MeleeAttackImpl(origin, dir, range, arcDeg, damage, rewindTicks);
}

// Oriented attack box (OBB around Y): damages every SceneHealth entity whose
// position lies inside the yaw-rotated half-extents box. Returns hit count.
int GameRuntime::AttackBoxImpl(const math::Vec3& center, const math::Vec3& half, float yaw,
                               float damage, uint32_t rewindTicks) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 hitPos = t->pos;
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, hitPos);
        const math::Vec3 d = hitPos - center;
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

int GameRuntime::AttackBox(const math::Vec3& center, const math::Vec3& half, float yaw,
                           float damage) {
    return AttackBoxImpl(center, half, yaw, damage, autoRewindTicks_);
}

int GameRuntime::AttackBoxLagComp(const math::Vec3& center, const math::Vec3& half, float yaw,
                                  float damage, uint32_t rewindTicks) {
    return AttackBoxImpl(center, half, yaw, damage, rewindTicks);
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
            } else if (id == kStatusSlow) {
                // Slow is a movement modifier read by scripts (magnitude =
                // speed factor); it deals no tick damage.
            } else {
                h->hp = std::fmax(0.0f, h->hp - magnitude);
            }
        });
    }
}

void GameRuntime::TickAnimations(float dt) {
    for (DrawItem& d : draws_) {
        if (!d.skinned || !d.skinned->Valid()) continue;
        if (d.animHasOverride) {
            // Resolve the clip pointer on first use (or after a re-resolve).
            if (!d.animClip) {
                const std::string needle = d.animName;
                for (const anim::AnimationClip& c : d.skinned->clips) {
                    // Case-insensitive substring, first hit wins (deterministic).
                    std::string hay = c.name;
                    for (char& ch : hay)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    std::string low = needle;
                    for (char& ch : low)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (hay.find(low) != std::string::npos) {
                        d.animClip = &c;
                        break;
                    }
                }
                if (d.animClip) {
                    // Capture the fade source pose from the shared default
                    // loop so the cross-fade starts where the model is now.
                    d.animFromPose = d.skinned->skeleton.BindPose();
                    if (d.skinned->defaultClip >= 0 && d.skinned->animator.Clip())
                        d.skinned->animator.Clip()->Sample(d.skinned->animator.Time(),
                                                           d.animFromPose);
                    d.animTime = 0.0f;
                    d.animFade = d.animFadeTotal;
                }
            }
            if (d.animClip) {
                d.animTime += dt * d.animSpeed;
                if (d.animFade > 0.0f) d.animFade = std::fmax(0.0f, d.animFade - dt);
                if (d.animLoop && d.animClip->duration > 0.0f)
                    d.animTime = std::fmod(d.animTime, d.animClip->duration);
                else if (d.animTime > d.animClip->duration)
                    d.animTime = d.animClip->duration; // one-shot clamps at end
            }
        } else {
            d.skinned->Update(dt);
        }
    }
    // Floating combat texts age out.
    for (auto it = floatTexts_.begin(); it != floatTexts_.end();) {
        it->age += dt;
        if (it->age >= it->life) it = floatTexts_.erase(it);
        else ++it;
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
        // G3-4: the skill hit test honours the auto lag-comp rewind set by
        // the server (targets tested at the pose they had `autoRewindTicks_`
        // ticks ago); damage lands on the current entity.
        MeleeAttackImpl(origin, dir, def->meleeRange, def->arcDeg, def->damage,
                        autoRewindTicks_, caster, def->statuses);
        return 1; // the cast happened even when no target was in the arc
    }

    // "box": oriented attack box; yaw derived from the facing dir so a
    // script passes a direction vector like every other skill.
    const float yaw = std::atan2(dir.x, dir.z);
    AttackBoxImpl(origin, {def->boxHalfX, def->boxHalfY, def->boxHalfZ}, yaw, def->damage,
                  autoRewindTicks_);
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

bool GameRuntime::DispatchPluginEvent(const std::string& name,
                                      const std::vector<script::Value>& args) {
    if (!plugins_) return false;
    plugins_->DispatchEvent(name, args);
    return true;
}

bool GameRuntime::RunPluginCommand(const std::string& name,
                                   const std::vector<script::Value>& args,
                                   std::string* error) {
    return plugins_ && plugins_->RunCommand(name, args, error);
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

// --- M1: per-entity animation + HUD anchors ---------------------------------

bool GameRuntime::PlayAnimation(ecs::Entity e, const std::string& clip, bool loop,
                                float crossFade, float speed) {
    if (!world_.Alive(e)) return false;
    // Only skinned entities can play clips; refuse anything else so scripts
    // can branch on the return value.
    bool skinned = false;
    for (const DrawItem& d : draws_)
        if (d.ent == e && d.skinned && d.skinned->Valid()) skinned = true;
    const SceneMesh* mesh = world_.Get<SceneMesh>(e);
    if (!skinned && (!mesh || mesh->meshKey.compare(0, 5, "gltf:") != 0)) return false;
    SceneAnimOverride* ov = world_.Get<SceneAnimOverride>(e);
    if (!ov) {
        ov = &world_.Add<SceneAnimOverride>(e);
        *ov = SceneAnimOverride{};
    }
    ov->clip = clip;
    ov->loop = loop;
    ov->crossFade = crossFade;
    ov->speed = speed;
    ov->active = true;
    // Drop any cached instance state so the next BuildDrawList re-issues it
    // (fresh cross-fade from the current pose).
    for (DrawItem& d : draws_)
        if (d.ent == e) {
            d.animName.clear();
            d.animClip = nullptr;
        }
    return true;
}

float GameRuntime::AnimationProgress(ecs::Entity e) const {
    for (const DrawItem& d : draws_) {
        if (!(d.ent == e) || !d.animHasOverride || !d.animClip) continue;
        if (d.animClip->duration <= 0.0f) return 1.0f;
        float p = d.animTime / d.animClip->duration;
        return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    }
    return 0.0f;
}

bool GameRuntime::AnimationFinished(ecs::Entity e) const {
    for (const DrawItem& d : draws_) {
        if (!(d.ent == e) || !d.animHasOverride || !d.animClip) continue;
        return !d.animLoop && d.animTime >= d.animClip->duration;
    }
    return false;
}

bool GameRuntime::WorldToScreen(const math::Vec3& world, float& outX, float& outY) const {
    if (!lastViewProjValid_) return false;
    math::Vec4 clip = lastViewProj_.TransformVec4(math::Vec4(world.x, world.y, world.z, 1.0f));
    if (clip.w <= 0.01f) return false;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    outX = (nx * 0.5f + 0.5f) * gfx::Renderer::kDesignWidth;
    outY = (0.5f - ny * 0.5f) * gfx::Renderer::kDesignHeight;
    return true;
}

void GameRuntime::SpawnFloatText(const math::Vec3& world, const std::string& text, bool crit,
                                 float life) {
    FloatText ft;
    ft.world = world;
    ft.text = text;
    ft.crit = crit;
    ft.life = life > 0.05f ? life : 1.2f;
    floatTexts_.push_back(ft);
    if (floatTexts_.size() > 64) floatTexts_.erase(floatTexts_.begin()); // cap
}

void GameRuntime::SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac) {
    if (!world_.Alive(e)) return;
    EntityPlate p;
    p.name = name;
    p.hpFrac = hpFrac;
    plates_[EntityKey(e)] = p;
}

void GameRuntime::Draw(gfx::Renderer& renderer, const gfx::Camera& camera,
                       float previewZoom) {
    if (!running_ || !cfg_.assets) return; // sim-only runtime draws nothing
    core::ScopedTimer drawTimer("runtime.draw");
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    gfx::Camera cam = camera;
    bool usedCameraEntity = false;
    // Script-driven FPS game camera: while the "cameraMouseLock" GameVar is
    // truthy, the script owns the rendered view through cameraFocus (placed at
    // eye + viewDir * cameraDist by the controller) plus cameraYaw/cameraPitch/
    // cameraDist — the same GameVars the host orbit cameras publish. This
    // overrides any scene Camera3D entity, so the runtime renders through the
    // player's eye in both the standalone player and the editor playtest.
    const script::Value fpsLock = scriptCtx_.gameVars.Get("cameraMouseLock");
    const bool fpsCam = (fpsLock.type == script::Value::Type::Number &&
                         fpsLock.number != 0.0) ||
                        (fpsLock.type == script::Value::Type::Bool && fpsLock.boolean);
    if (fpsCam) {
        math::Vec3 focus;
        const script::Value focusVar = scriptCtx_.gameVars.Get("cameraFocus");
        if (focusVar.type == script::Value::Type::Table && focusVar.table) {
            for (const auto& kv : focusVar.table->fields) {
                if (kv.second.type != script::Value::Type::Number) continue;
                if (kv.first == "x") focus.x = static_cast<float>(kv.second.number);
                else if (kv.first == "y") focus.y = static_cast<float>(kv.second.number);
                else if (kv.first == "z") focus.z = static_cast<float>(kv.second.number);
            }
        }
        float yaw = 0.0f, pitch = 0.0f, dist = 2.0f;
        const script::Value yawVar = scriptCtx_.gameVars.Get("cameraYaw");
        if (yawVar.type == script::Value::Type::Number) yaw = static_cast<float>(yawVar.number);
        const script::Value pitchVar = scriptCtx_.gameVars.Get("cameraPitch");
        if (pitchVar.type == script::Value::Type::Number)
            pitch = math::Clamp(static_cast<float>(pitchVar.number), -1.3f, 1.3f);
        const script::Value distVar = scriptCtx_.gameVars.Get("cameraDist");
        if (distVar.type == script::Value::Type::Number && distVar.number > 0.0)
            dist = math::Clamp(static_cast<float>(distVar.number), 2.0f, 80.0f);
        math::Vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                          std::cos(yaw) * std::cos(pitch)};
        cam.position = focus + offset * dist;
        cam.target = focus;
        cam.up = {0, 1, 0};
        cam.ortho = false;
        usedCameraEntity = true;
    }
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    world_.ViewAll<SceneCamera, SceneTransform>().ForEach(
        [&](ecs::Entity, const SceneCamera& c, const SceneTransform& t) {
            if (usedCameraEntity) return;
            usedCameraEntity = true;
            cam.position = t.pos;
            cam.target = t.pos + t.rot.Rotate({0, 0, -1});
            cam.up = {0, 1, 0};
            cam.ortho = c.ortho;
            cam.fovY = c.fov * math::kDegToRad;
            cam.orthoSize = c.orthoSize > 0.0f ? c.orthoSize : 10.0f;
            // Editor whole-view zoom: shrink the ortho size so sprites grow
            // with the same factor the host applies to the 2D UI overlay.
            if (cam.ortho && previewZoom > 0.0f)
                cam.orthoSize /= previewZoom;
        });
    // Project at the ACTIVE scene viewport's aspect (a dock sub-rect in the
    // editor, the full target in the standalone player) so the runtime render
    // matches whatever rasterization rect the host set up - otherwise the
    // playtest FOV would differ from the edit-mode viewport.
    renderer.SetCamera(cam, renderer.SceneAspect());
    // Scripts may have spawned/despawned sprite entities since the last frame.
    BuildDrawList();
    // G1-3: refresh the world-transform cache (parent-before-child, arbitrary
    // depth) once per frame; the BVH pass and the draw loop read it instead of
    // re-walking parent chains per entity.
    RebuildWorldTransforms();
    // M1 HUD anchors: project every drawn entity's world position (plus a
    // per-plate head offset) into design units for on_render scripts. Cached
    // once per frame; WorldToScreen() below uses the same matrices.
    {
        lastViewProj_ = cam.ViewProjection(renderer.SceneAspect());
        lastViewProjValid_ = true;
        screenAnchors_.clear();
        for (const DrawItem& d : draws_) {
            const math::Mat4 model = CachedLocalToWorld(d.ent);
            math::Vec3 wp{model.m[3], model.m[7], model.m[11]};
            // Head offset: lift the anchor above the model bounds when the
            // entity carries a plate (the script stamps names via
            // SetEntityPlate; default 0 keeps the raw position).
            auto pit = plates_.find(EntityKey(d.ent));
            if (pit != plates_.end() && pit->second.hpFrac >= 0.0f) {
                const SceneTransform* tr = world_.Get<SceneTransform>(d.ent);
                if (tr) {
                    // Plate tracks the RENDERED mesh, which for a skinned rig
                    // can sit off the entity pivot (the wolf's bones place the
                    // body away from its origin). Compute the world AABB with
                    // the same transform chain Draw() uses — model *
                    // part.localTransform * bone matrix — and center the bar on
                    // it, just above the top.
                    math::AABB wb;
                    bool have = false;
                    auto expand = [&](const math::Vec3& p) {
                        if (!have) {
                            wb.min = wb.max = p;
                            have = true;
                        } else {
                            wb.min.x = std::fmin(wb.min.x, p.x);
                            wb.min.y = std::fmin(wb.min.y, p.y);
                            wb.min.z = std::fmin(wb.min.z, p.z);
                            wb.max.x = std::fmax(wb.max.x, p.x);
                            wb.max.y = std::fmax(wb.max.y, p.y);
                            wb.max.z = std::fmax(wb.max.z, p.z);
                        }
                    };
                    auto expandBox = [&](const math::AABB& lb, const math::Mat4& m) {
                        const math::Vec3 corners[8] = {
                            {lb.min.x, lb.min.y, lb.min.z}, {lb.max.x, lb.min.y, lb.min.z},
                            {lb.min.x, lb.max.y, lb.min.z}, {lb.max.x, lb.max.y, lb.min.z},
                            {lb.min.x, lb.min.y, lb.max.z}, {lb.max.x, lb.min.y, lb.max.z},
                            {lb.min.x, lb.max.y, lb.max.z}, {lb.max.x, lb.max.y, lb.max.z}};
                        for (const math::Vec3& c : corners) {
                            const math::Vec4 q = m.TransformVec4({c.x, c.y, c.z, 1.0f});
                            if (q.w != 0.0f) expand({q.x / q.w, q.y / q.w, q.z / q.w});
                        }
                    };
                    if (d.skinned && d.skinned->Valid()) {
                        // Mirror Draw()'s bone selection (override clip vs the
                        // model's default) so the anchor matches this frame.
                        std::vector<math::Mat4> bones;
                        if (d.animHasOverride && d.animClip) {
                            anim::Pose pose = d.skinned->skeleton.BindPose();
                            d.animClip->Sample(d.animTime, pose);
                            if (d.animFade > 0.0f && d.animFadeTotal > 0.0f &&
                                d.animFromPose.t.size() == pose.t.size())
                                pose.Lerp(d.animFromPose, pose,
                                          1.0f - d.animFade / d.animFadeTotal);
                            bones = d.skinned->skeleton.ComputeBoneMatrices(pose);
                        } else {
                            bones = d.skinned->BoneMatrices();
                        }
                        if (!bones.empty()) {
                            // CPU-skin the actual vertices so the plate hugs the
                            // RENDERED mesh (a rig can place the body off the
                            // entity pivot; box-corner transforms over all bones
                            // inflate the bounds, so exact per-vertex is safest).
                            for (const auto& part : d.skinned->parts) {
                                const std::vector<gfx::Vertex3D>& verts = part.mesh.CpuVerts();
                                if (verts.empty()) continue;
                                const math::Mat4 m = model * part.localTransform;
                                for (const gfx::Vertex3D& v : verts) {
                                    math::Vec4 sk{0.0f, 0.0f, 0.0f, 0.0f};
                                    for (int k = 0; k < 4; ++k) {
                                        if (v.w[k] <= 0.0f) continue;
                                        const int ji = static_cast<int>(v.j[k]);
                                        if (ji < 0 ||
                                            ji >= static_cast<int>(bones.size()))
                                            continue;
                                        const math::Vec4 q =
                                            bones[static_cast<size_t>(ji)].TransformVec4(
                                                {v.pos.x, v.pos.y, v.pos.z, 1.0f});
                                        sk.x += v.w[k] * q.x;
                                        sk.y += v.w[k] * q.y;
                                        sk.z += v.w[k] * q.z;
                                        sk.w += v.w[k] * q.w;
                                    }
                                    if (sk.w == 0.0f) continue;
                                    const math::Vec4 world = m.TransformVec4(
                                        {sk.x / sk.w, sk.y / sk.w, sk.z / sk.w, 1.0f});
                                    if (world.w != 0.0f)
                                        expand({world.x / world.w, world.y / world.w,
                                                world.z / world.w});
                                }
                            }
                        } else {
                            for (const auto& part : d.skinned->parts) {
                                const math::AABB lb = part.mesh.Bounds();
                                if (lb.max.y <= lb.min.y) continue;
                                expandBox(lb, model * part.localTransform);
                            }
                        }
                    } else if (d.mesh.Valid()) {
                        const math::AABB lb = d.mesh.Bounds();
                        if (lb.max.y > lb.min.y) expandBox(lb, model);
                    }
                    if (have) {
                        wp.x = (wb.min.x + wb.max.x) * 0.5f;
                        wp.z = (wb.min.z + wb.max.z) * 0.5f;
                        wp.y = wb.max.y + 0.2f * tr->scale.y;
                    } else {
                        wp.y = wp.y + 1.9f * tr->scale.y;
                    }
                }
            }
            math::Vec4 clip =
                lastViewProj_.TransformVec4(math::Vec4(wp.x, wp.y, wp.z, 1.0f));
            ScreenAnchor a;
            a.entity = EntityKey(d.ent);
            a.world = wp;
            if (clip.w > 0.01f) {
                const float nx = clip.x / clip.w, ny = clip.y / clip.w;
                // Design-space (1280x720) coordinates: the same mapping the
                // renderer's 2D overlay (and on_render) draws with.
                a.x = (nx * 0.5f + 0.5f) * gfx::Renderer::kDesignWidth;
                a.y = (0.5f - ny * 0.5f) * gfx::Renderer::kDesignHeight;
                a.onscreen = nx >= -1.2f && nx <= 1.2f && ny >= -1.2f && ny <= 1.2f;
            }
            screenAnchors_.push_back(a);
        }
    }
    // P2-3: sprites render back-to-front by their sortOrder component (2D
    // games); 3D depth-tested meshes are unaffected by the stable order.
    drawOrder_.resize(draws_.size());
    for (size_t i = 0; i < drawOrder_.size(); ++i) drawOrder_[i] = i;
    std::stable_sort(drawOrder_.begin(), drawOrder_.end(), [&](size_t a, size_t b) {
        const SceneSortOrder* sa = world_.Get<SceneSortOrder>(draws_[a].ent);
        const SceneSortOrder* sb = world_.Get<SceneSortOrder>(draws_[b].ent);
        return (sa ? sa->z : 0.0f) < (sb ? sb->z : 0.0f);
    });
    // Instanced batching: opaque static meshes with the same mesh + material
    // group into one instanced draw call. Only when the depth buffer works -
    // the no-depth fallback relies on painter's order, which batching would
    // change. Flush whenever a non-batchable item interrupts the run so the
    // relative order of opaque vs transparent/skinned draws never changes.
    const bool canBatch = renderer.DepthTestAvailable();
    // G1-2: build a per-frame BVH of batchable items and pre-cull the camera
    // frustum, so instanced draws only receive visible instances (the
    // renderer then skips its own per-instance test). Uses the renderer's own
    // Frustum::Intersects test, so the visible set is identical to before.
    drawBvh_.Clear();
    bvhVisible_.assign(draws_.size(), 0);
    if (canBatch) {
        for (size_t idx : drawOrder_) {
            DrawItem& item = draws_[idx];
            if (!world_.Alive(item.ent)) continue;
            if (hiddenEntities_.count(EntityKey(item.ent)) != 0) continue;
            if (!item.resolved) ResolveDrawItem(item, renderer);
            if (!item.resolved || item.failed) continue;
            if (!world_.Get<SceneTransform>(item.ent)) continue;
            if (item.skinned || item.isSprite || item.isDecal || item.mat.transparent ||
                item.mat.shader.Valid() || !item.mesh.Valid())
                continue;
            const math::Mat4 model = CachedLocalToWorld(item.ent);
            // Column-vector convention: the translation is the last COLUMN of
            // the row-major matrix (m[3], m[7], m[11]), not the last row.
            // G2-3: a terrain chunk uses its patch centre (not the terrain
            // origin) so distance LOD is chosen per patch.
            const math::Vec3 worldPos = item.isTerrainChunk
                                            ? model.TransformPoint(item.chunkCenterLocal)
                                            : math::Vec3{model.m[3], model.m[7], model.m[11]};
            const gfx::Mesh drawMesh =
                SelectLodMesh(item.mesh, item.chain, worldPos, camera.position);
            if (!drawMesh.Valid()) continue;
            drawBvh_.Insert(static_cast<math::Bvh::Id>(idx),
                            math::TransformAABB(drawMesh.Bounds(), model));
        }
        if (!drawBvh_.Empty())
            drawBvh_.QueryFrustum(renderer.ViewFrustum(),
                                  [&](math::Bvh::Id id) { bvhVisible_[id] = 1; });
    }
    drawBatches_.clear();
    batchModels_.clear();
    auto flushBatches = [&]() {
        if (drawBatches_.empty()) return;
        for (const DrawBatch& b : drawBatches_) {
            if (b.count == 0) continue;
            renderer.DrawMeshInstanced(b.mesh, b.mat, batchModels_.data() + b.start, b.count,
                                       /*frustumCull=*/false);
        }
        drawBatches_.clear();
        batchModels_.clear();
    };
    size_t dead = 0;
    for (size_t idx : drawOrder_) {
        DrawItem& item = draws_[idx];
        if (!world_.Alive(item.ent)) {
            ++dead; // scripts can Despawn entities mid-playtest
            continue;
        }
        if (hiddenEntities_.count(EntityKey(item.ent)) != 0) continue; // SetVisible(false)
        if (!item.resolved) ResolveDrawItem(item, renderer);
        if (!item.resolved || item.failed) continue;
        if (!world_.Get<SceneTransform>(item.ent)) continue;
        math::Mat4 model = CachedLocalToWorld(item.ent);
        if (item.tileOffset.LengthSq() > 0.0f)
            model = model * math::Mat4::Translation(item.tileOffset);
        if (item.isDecal) {
            // Lift the quad a hair above the surface it projects onto so depth
            // testing keeps it visible (no z-fighting on flat ground).
            model = model * math::Mat4::Translation({0.0f, 0.02f, 0.0f});
        }
        // Column-vector convention: translation lives in the last column
        // (m[3], m[7], m[11]); reading m[12..14] returned ~0 and broke LOD
        // distance selection + decal placement.
        // G2-3: a terrain chunk uses its patch centre (not the terrain origin)
        // so distance LOD is chosen per patch.
        const math::Vec3 worldPos = item.isTerrainChunk
                                        ? model.TransformPoint(item.chunkCenterLocal)
                                        : math::Vec3{model.m[3], model.m[7], model.m[11]};
        // Batchable: opaque static mesh with the built-in shader. Skinned
        // (per-entity bone matrices), sprites, decals, transparent materials
        // and custom shaders keep the per-entity path.
        const bool batchable = canBatch && !item.skinned && !item.isSprite && !item.isDecal &&
                               !item.mat.transparent && !item.mat.shader.Valid() &&
                               item.mesh.Valid();
        if (batchable) {
            if (!bvhVisible_.empty() && bvhVisible_[idx] == 0) continue; // pre-culled
            gfx::Mesh drawMesh = SelectLodMesh(item.mesh, item.chain, worldPos, camera.position);
            if (!drawMesh.Valid()) continue;
            int batchIndex = -1;
            for (size_t bi = 0; bi < drawBatches_.size(); ++bi) {
                if (drawBatches_[bi].mesh.Handle().vao == drawMesh.Handle().vao &&
                    SameMaterial(drawBatches_[bi].mat, item.mat)) {
                    batchIndex = static_cast<int>(bi);
                    break;
                }
            }
            if (batchIndex < 0) {
                DrawBatch b;
                b.mesh = drawMesh;
                b.mat = item.mat;
                b.start = static_cast<uint32_t>(batchModels_.size());
                batchIndex = static_cast<int>(drawBatches_.size());
                drawBatches_.push_back(b);
            }
            batchModels_.push_back(model);
            drawBatches_[static_cast<size_t>(batchIndex)].count++;
            continue;
        }
        flushBatches(); // keep relative order with non-batched draws
        if (item.skinned && item.skinned->Valid()) {
            std::vector<math::Mat4> bones;
            if (item.animHasOverride && item.animClip) {
                anim::Pose pose = item.skinned->skeleton.BindPose();
                item.animClip->Sample(item.animTime, pose);
                if (item.animFade > 0.0f && item.animFadeTotal > 0.0f &&
                    item.animFromPose.t.size() == pose.t.size())
                    pose.Lerp(item.animFromPose, pose,
                              1.0f - item.animFade / item.animFadeTotal);
                bones = item.skinned->skeleton.ComputeBoneMatrices(pose);
            } else {
                bones = item.skinned->BoneMatrices();
            }
            for (const auto& part : item.skinned->parts)
                renderer.DrawSkinnedMesh(part.mesh, part.material, model * part.localTransform,
                                         bones, static_cast<int>(bones.size()));
        } else if (item.isSprite) {
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
    flushBatches();
    // Skill projectiles (fireballs): bright glowing orbs.
    if (!projectiles_.empty()) {
        if (!fireballMesh_.Valid()) fireballMesh_ = gfx::MakeFireballMesh(renderer);
        gfx::Material fmat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
        fmat.emissiveIntensity = 2.5f;
        for (const Projectile& p : projectiles_) {
            renderer.DrawMesh(fireballMesh_, fmat, math::Mat4::Translation(p.pos));
        }
    }
    // G2-3 vegetation: instanced plant meshes + far yaw-billboard impostors.
    DrawVegetation(renderer, camera);
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

std::vector<ecs::Entity> GameRuntime::GetChildren(ecs::Entity parent) {
    std::vector<ecs::Entity> out;
    if (!parent.IsValid()) return out;
    auto view = world_.ViewAll<SceneParentLink, SceneTransform>();
    for (size_t i = 0; i < view.Size(); ++i) {
        const ecs::Entity child = world_.EntityAt<SceneParentLink>(i);
        const SceneParentLink* link = world_.Get<SceneParentLink>(child);
        if (link && link->parent == parent) out.push_back(child);
    }
    return out;
}

std::vector<ecs::Entity> GameRuntime::GetDescendants(ecs::Entity root) {
    std::vector<ecs::Entity> out;
    std::vector<ecs::Entity> stack = GetChildren(root);
    std::set<uint64_t> visited;
    while (!stack.empty()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        if (!visited.insert(EntityKey(e)).second) continue; // cycle guard
        out.push_back(e);
        const std::vector<ecs::Entity> kids = GetChildren(e);
        stack.insert(stack.end(), kids.begin(), kids.end());
    }
    return out;
}

void GameRuntime::RebuildWorldTransforms() {
    worldTransforms_.clear();
    if (!running_) return;

    // parent EntityKey -> children (entities with a SceneTransform whose
    // SceneParentLink points at it). Roots = entities with a SceneTransform
    // and no live parent link.
    std::unordered_map<uint64_t, std::vector<ecs::Entity>> children;
    std::vector<ecs::Entity> roots;
    auto linkView = world_.ViewAll<SceneParentLink, SceneTransform>();
    for (size_t i = 0; i < linkView.Size(); ++i) {
        const ecs::Entity child = world_.EntityAt<SceneParentLink>(i);
        const SceneParentLink* link = world_.Get<SceneParentLink>(child);
        if (!link) continue;
        if (world_.Alive(link->parent))
            children[EntityKey(link->parent)].push_back(child);
        else
            roots.push_back(child); // dangling parent: treat as a root
    }
    auto transformView = world_.ViewAll<SceneTransform>();
    for (size_t i = 0; i < transformView.Size(); ++i) {
        const ecs::Entity e = world_.EntityAt<SceneTransform>(i);
        const SceneParentLink* link = world_.Get<SceneParentLink>(e);
        if (!link || !world_.Alive(link->parent)) roots.push_back(e);
    }

    // Iterative DFS from roots: a parent's world is always computed before its
    // children are visited, so arbitrary tree depth is handled without the old
    // 8-level walk cap.
    std::vector<ecs::Entity> stack;
    for (const ecs::Entity& r : roots) {
        if (worldTransforms_.count(EntityKey(r)) != 0) continue;
        stack.push_back(r);
        while (!stack.empty()) {
            const ecs::Entity e = stack.back();
            stack.pop_back();
            if (worldTransforms_.count(EntityKey(e)) != 0) continue; // cycle guard
            const SceneTransform* t = world_.Get<SceneTransform>(e);
            if (!t) continue;
            math::Mat4 world = math::Mat4::Translation(t->pos) * t->rot.ToMat4() *
                               math::Mat4::Scale(t->scale);
            const SceneParentLink* link = world_.Get<SceneParentLink>(e);
            if (link && world_.Alive(link->parent)) {
                const auto pit = worldTransforms_.find(EntityKey(link->parent));
                if (pit != worldTransforms_.end()) world = pit->second * world;
            }
            worldTransforms_[EntityKey(e)] = world;
            const auto cit = children.find(EntityKey(e));
            if (cit != children.end())
                for (const ecs::Entity& c : cit->second) stack.push_back(c);
        }
    }
}

math::Mat4 GameRuntime::CachedLocalToWorld(ecs::Entity e) const {
    const auto it = worldTransforms_.find(EntityKey(e));
    return it == worldTransforms_.end() ? math::Mat4::Identity() : it->second;
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
    core::ScopedTimer tickTimer("runtime.tick");
    // P1-2 debugger: a breakpoint hit latches the host's paused flag during a
    // script call; stop advancing the simulation so the editor can inspect and
    // step before resuming.
    if (hosts_.lua && hosts_.lua->DebuggerPaused()) return;

    // Both backends share the engine-injected simulated clock.
    if (hosts_.lua) hosts_.lua->SetSimClock(simTime_);
    if (hosts_.js) hosts_.js->SetSimClock(simTime_);
    {
        core::ScopedTimer scriptsTimer("runtime.scripts");
        // Index-based: SpawnPrefab (called from a script) can push new
        // ScriptInst entries into scripts_ mid-loop; an iterator would be
        // invalidated. New instances are processed in the same tick, which is
        // the expected "spawned this frame acts this frame" semantics.
        for (size_t si = 0; si < scripts_.size(); ++si) {
            ScriptInst& inst = scripts_[si];
            if (!world_.Alive(inst.ent)) continue;
            if (inst.onUpdate == 0) continue; // this chunk defines no on_update
            CallEntityFunctionHandle(inst, inst.onUpdate, "on_update",
                                     {EntityToValue(inst.ent), script::Value::Num(dt)});
        }
        // Runtime plugin systems tick after scene scripts (same sim clock).
        if (plugins_) {
            plugins_->SetSimTime(simTime_);
            plugins_->Tick(dt);
        }
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
    {
        core::ScopedTimer physicsTimer("runtime.physics");
        physicsAccum_ += dt;
        constexpr float kPhysicsStep = 1.0f / 60.0f;
        int physicsSteps = 0;
        while (physicsAccum_ >= kPhysicsStep && physicsSteps < 4) {
            physics_->Step(kPhysicsStep, math::Vec3{0.0f, kGravityY, 0.0f});
            physicsAccum_ -= kPhysicsStep;
            ++physicsSteps;
        }
        if (physicsSteps == 4) physicsAccum_ = 0.0f;
    }
    SyncSceneBodies();
    TickTweens(dt);
    TickAnimations(dt);
    TickStatuses(dt);
    TickSkillCooldowns(dt);
    TickProjectiles(dt);
    simTime_ += dt;

    // G3-4: snapshot authoritative poses for lag-compensated hit tests. The
    // oldest snapshot is dropped once the ring reaches capacity (~1s @60Hz).
    {
        std::unordered_map<uint64_t, math::Vec3> snap;
        auto view = world_.ViewAll<SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            const ecs::Entity ent = world_.EntityAt<SceneTransform>(i);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (t) snap[EntityKey(ent)] = t->pos;
        }
        auto bindView = world_.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < bindView.Size(); ++i) {
            const ecs::Entity ent = world_.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* b = world_.Get<script::CTransformBind>(ent);
            if (b) snap[EntityKey(ent)] = b->pos;
        }
        poseHistory_.push_back(std::move(snap));
        if (poseHistory_.size() > kLagCompHistoryTicks)
            poseHistory_.erase(poseHistory_.begin());
    }

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
