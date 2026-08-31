#include "neon/scene/game_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

#include "neon/assets/asset_manager.hpp"
#include "neon/assets/asset_path.hpp"
#include "neon/assets/mesh_format.hpp"
#include "neon/assets/asset_variants.hpp"
#include "neon/core/log.hpp"
#include "neon/core/pack.hpp"
#include "neon/core/profiler.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/gfx/terrain.hpp"
#include "neon/io/vfs.hpp"
#include "neon/kernel/registry.hpp"
#include "neon/scene/scene_file.hpp"
#include "gameplay_lib.hpp"

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

// OverlapSphere/OverlapBox hits -> Lua array of {entity={id,gen}, x=, y=, z=}.
// The entity is serialized with the same {id, gen} shape EntityFromValue parses,
// so scripts can round-trip `h.entity` into GetHealth/SetPosition/ApplyStatus.
script::Value OverlapHitsToValue(const std::vector<GameRuntime::HealthHit>& hits) {
    script::Value out = script::Value::Tbl();
    out.table->array.reserve(hits.size());
    for (const auto& h : hits) {
        script::Value item = script::Value::Tbl();
        item.table->fields.emplace_back("entity", EntityToValue(h.entity));
        item.table->fields.emplace_back("x", script::Value::Num(h.pos.x));
        item.table->fields.emplace_back("y", script::Value::Num(h.pos.y));
        item.table->fields.emplace_back("z", script::Value::Num(h.pos.z));
        out.table->array.push_back(std::move(item));
    }
    return out;
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
// load a packed game's assets/prefabs/ tree from the unpacked directory.
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
    // G5-4-4(�?): register the per-frame component sub-task system graph once
    // (idempotent across Stop->Start cycles).
    InitSystemGraph();
    // Script VFX particle sprite: a soft radial glow (project assets ship one
    // at assets/sprites/glow.png; missing file degrades to a white quad).
    if (cfg_.assets)
        sceneParticles_.InitParticleTexture(*cfg_.assets,
                                            FullAssetPath("assets/sprites/glow.png"));
    // Create the physics world: Jolt when requested and compiled, else the
    // deterministic custom solver (server / headless tests). A "plugin:<name>"
    // backend (G5-1) loads the solver from a native middleware DLL/SO under
    // cfg_.pluginBaseDir/plugins �?swappable without relinking. The owning
    // PhysicsBackend is kept alive until this runtime is destroyed (it owns the
    // DLL), and is declared before physics_ so the world dies before the library.
    // Microkernel seam (P-B): prefer an injected physics world from the service
    // registry (non-owning — the module owns it). Falls back to the
    // self-contained creation below when no service is registered.
    if (cfg_.services) {
        if (physics::World* w = cfg_.services->Get<physics::World>())
            physics_ = std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
                w, [](physics::World*) {});  // non-owning
    }
    if (!physics_) {
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
    }  // if (!physics_)
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
    RegisterAudioSources();

    scriptCtx_.world = &world_;
    scriptCtx_.physics = physics_.get();
    scriptCtx_.input = cfg_.input;
    scriptCtx_.loc = &loc_;
    scriptCtx_.playSfx = cfg_.playSfx;
    scriptCtx_.playMusic = cfg_.playMusic;
    scriptCtx_.playSfx3D = cfg_.playSfx3D;
    scriptCtx_.setAudioListener = cfg_.setAudioListener;
    scriptCtx_.setBusVolume = cfg_.setBusVolume;
    scriptCtx_.emitParticles = [this](const gfx::EmitterConfig& cfg) {
        sceneParticles_.Emit(cfg);
    };
    scriptCtx_.entityKinds.clear();
    // Data files (levels/*.json etc.) resolve like scripts: project dir on
    // disk, or the unpacked dir for packed games (ReadScript honors the pack
    // reader override).
    scriptCtx_.readData = [this](const std::string& path) {
        return ReadScript(FullScriptPath(path));
    };
    // Replaceable UI system: injected via cfg_.uiSystem wins; otherwise the
    // default document-backed system reading through the same VFS/disk source
    // as scripts (G7-1: packed games load ui/*.ui.json straight from the pack).
    if (cfg_.uiSystem) {
        ui_ = cfg_.uiSystem;
    } else {
        ui::DocumentUiConfig ucfg;
        ucfg.readFile = [this](const std::string& path) {
            return ReadScript(FullScriptPath(path));
        };
        ui_.reset(ui::CreateDocumentUiSystem(ucfg).release());
    }
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
    scriptCtx_.uiSetColor = [this](const std::string& name, float r, float g, float b,
                                   float a) { UISetColor(name, r, g, b, a); };
    scriptCtx_.loadTexture = [this](const std::string& path) {
        if (!cfg_.assets || path.empty()) return gfx::TextureHandle{};
        return cfg_.assets->LoadTexture(FullAssetPath(path)).Handle();
    };
    scriptCtx_.tweenStart = [this](ecs::Entity e, int prop, const math::Vec3& from,
                                   const math::Vec3& to, float time, int easing) {
        tweens_.Start(e, prop, from, to, time, easing);
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
    // GetGroundHeight(x, z): sample the first scene terrain's world Y at a
    // WORLD (x,z). The terrain heightmap is stored in local [-size/2, size/2]
    // coordinates, so we offset by the terrain entity's world position before
    // sampling. Returns 0 when no terrain/heightmap exists.
    scriptCtx_.groundHeight = [this](float x, float z) -> float {
        auto view = world_.ViewAll<SceneTerrain>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity ent = world_.EntityAt<SceneTerrain>(i);
            const SceneTerrain* terr = world_.Get<SceneTerrain>(ent);
            if (!terr || terr->heights.empty() || terr->segments < 1) continue;
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            const float localX = x - (t ? t->pos.x : 0.0f);
            const float localZ = z - (t ? t->pos.z : 0.0f);
            return gfx::SampleTerrainHeight(terr->heights, terr->segments, terr->size,
                                            localX, localZ) *
                       terr->heightScale +
                   (t ? t->pos.y : 0.0f);
        }
        return 0.0f;
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
    // Sequence-frame sprite animation: update the entity's SceneSprite frames
    // and reset its DrawItem frame clock so the new animation starts fresh.
    scriptCtx_.setSpriteFrames = [this](ecs::Entity e, const std::vector<std::string>& frames,
                                        float fps) {
        if (!world_.Alive(e)) return;
        if (SceneSprite* s = world_.Get<SceneSprite>(e)) {
            s->frames = frames;
            s->fps = fps;
            s->loop = true;
            s->sheet.clear();
            s->sheetFrames = 0;
        }
        for (DrawItem& d : draws_) {
            if (d.ent != e) continue;
            d.spriteFrames = frames;
            d.spriteFps = fps;
            d.spriteLoop = true;
            d.spriteAnimTime = 0.0f;
            d.spriteFrame = -1;
            d.spriteTex = frames.empty() || frames[0].empty() ? std::string() : frames[0];
            d.sheetTex.clear();
            d.sheetFrames = 0;
            d.resolved = false;
            break;
        }
    };
    // Spritesheet variant of the above (one atlas texture, sub-rects).
    scriptCtx_.setSpriteSheet = [this](ecs::Entity e, const std::string& sheet, int count,
                                       float fps) {
        if (!world_.Alive(e) || sheet.empty() || count <= 0) return;
        if (SceneSprite* s = world_.Get<SceneSprite>(e)) {
            s->sheet = sheet;
            s->sheetFrames = count;
            s->fps = fps;
            s->loop = true;
            s->frames.clear();
        }
        for (DrawItem& d : draws_) {
            if (d.ent != e) continue;
            d.sheetTex = sheet;
            d.sheetFrames = count;
            d.spriteFps = fps;
            d.spriteLoop = true;
            d.spriteAnimTime = 0.0f;
            d.spriteFrame = -1;
            d.spriteTex = sheet;
            d.spriteFrames.clear();
            d.resolved = false;
            break;
        }
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
    inputMap_.Reset(); // clear G7-3 timing state across playtest restarts
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
        // A8: a scripted move must also move the physics body, otherwise the
        // next SyncSceneBodies() snaps the entity back and characters walk
        // through walls that only physics knows about.
        if (const SceneRigidBody* rb = world_.Get<SceneRigidBody>(e)) {
            if (rb->bodyId != 0) physics_->SetPosition({rb->bodyId}, p);
        }
        if (const SceneCharacter* c = world_.Get<SceneCharacter>(e)) {
            if (c->bodyId != 0) physics_->SetPosition({c->bodyId}, p);
        }
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
    scriptCtx_.sceneGetMaxHp = [this](ecs::Entity e) {
        const SceneHealth* h = world_.Get<SceneHealth>(e);
        return h ? h->maxHp : -1.0f;
    };
    scriptCtx_.sceneSetHp = [this](ecs::Entity e, float hp) {
        if (SceneHealth* h = world_.Get<SceneHealth>(e)) h->hp = hp;
    };
    // Status-effect hooks (M2 combat core): scripts apply/query/remove
    // buffs+debuffs through ApplyStatus/HasStatus/StatusMagnitude/RemoveStatus.
    scriptCtx_.sceneApplyStatus = [this](ecs::Entity e, uint32_t id, float dur, float mag,
                                         float interval) {
        if (!world_.Alive(e)) return;
        if (!world_.Has<StatusComponent>(e)) world_.Add<StatusComponent>(e);
        if (StatusComponent* c = world_.Get<StatusComponent>(e))
            ApplyStatus(*c, id, dur, mag, interval);
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
    // Arbitrary data-component access is wired here so the script layer never
    // links against the scene module (breaks the scene <-> script dependency
    // cycle).
    scriptCtx_.entityComponent = [this](ecs::Entity e, const std::string& name,
                                        core::Json* out) {
        if (!out || !world_.Alive(e)) return false;
        const scene::SceneData* sd = world_.Get<scene::SceneData>(e);
        if (!sd) return false;
        for (const auto& kv : sd->components) {
            if (kv.first == name) {
                *out = kv.second;
                return true;
            }
        }
        return false;
    };
    scriptCtx_.setEntityComponent = [this](ecs::Entity e, const std::string& name,
                                           const core::Json& data) {
        if (!world_.Alive(e)) return;
        if (!world_.Has<scene::SceneData>(e)) world_.Add<scene::SceneData>(e);
        if (scene::SceneData* sd = world_.Get<scene::SceneData>(e)) {
            for (auto& kv : sd->components) {
                if (kv.first == name) {
                    kv.second = data;
                    return;
                }
            }
            sd->components.emplace_back(name, data);
        }
    };
    // M1 gameplay hooks: per-entity animation + HUD anchors + floating text.
    scriptCtx_.playAnimation = [this](ecs::Entity e, const std::string& clip, bool loop,
                                      float fade, float speed) {
        return PlayAnimation(e, clip, loop, fade, speed);
    };
    scriptCtx_.animProgress = [this](ecs::Entity e) { return AnimationProgress(e); };
    scriptCtx_.animFinished = [this](ecs::Entity e) { return AnimationFinished(e); };
    scriptCtx_.attachStateMachine = [this](ecs::Entity e, const std::string& path) {
        return AttachStateMachine(e, path);
    };
    scriptCtx_.setAnimParam = [this](ecs::Entity e, const std::string& name, float value) {
        SetAnimParam(e, name, value);
    };
    scriptCtx_.worldToScreen = [this](const math::Vec3& w, float& ox, float& oy) {
        return hud_.WorldToScreen(w, ox, oy);
    };
    scriptCtx_.worldFromScreen = [this](const math::Vec2& d, float& ox, float& oy) {
        return hud_.ScreenToWorld(d, ox, oy);
    };
    scriptCtx_.uiViewportSize = [this]() {
        return math::Vec2{hud_.DesignWidth(), hud_.DesignHeight()};
    };
    scriptCtx_.spawnFloatText = [this](const math::Vec3& w, const std::string& t, bool crit,
                                       float life) { hud_.SpawnFloatText(w, t, crit, life); };
    scriptCtx_.setEntityPlate = [this](ecs::Entity e, const std::string& name, float hp) {
        if (!world_.Alive(e)) return;
        hud_.SetEntityPlate(e, name, hp);
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
        for (const HudSystem::ScreenAnchor& a : hud_.ScreenAnchors()) {
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
        for (const auto& kv : hud_.EntityPlates()) {
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
        for (const HudSystem::FloatText& f : hud_.FloatTexts()) {
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
    scriptCtx_.overlapSphere = [this](const math::Vec3& c, float r, uint32_t rewind) {
        return OverlapHitsToValue(OverlapSphere(c, r, rewind));
    };
    scriptCtx_.overlapBox = [this](const math::Vec3& c, const math::Vec3& half, float yaw,
                                   uint32_t rewind) {
        return OverlapHitsToValue(OverlapBox(c, half, yaw, rewind));
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
                                        float damage, float life, ecs::Entity caster, float range,
                                        float hitRadius,
                                        const std::vector<script::SkillStatusData>& statuses) {
        std::vector<scene::SkillStatus> sceneStatuses;
        sceneStatuses.reserve(statuses.size());
        for (const script::SkillStatusData& s : statuses) {
            scene::SkillStatus st;
            st.id = s.id;
            st.duration = s.duration;
            st.magnitude = s.magnitude;
            st.tickInterval = s.tickInterval;
            sceneStatuses.push_back(st);
        }
        SpawnProjectile(pos, dir, speed, damage, life, caster, range, hitRadius, sceneStatuses);
    };

    // Lua is the canonical backend (the editor's debugger targets it); a
    // failure there aborts the runtime. The JS backend is optional: when its
    // host fails to initialize, JS-scripted entities are skipped with a log.
    //
    // Microkernel seam (P-B): prefer an injected script host (Lua backend) from
    // the service registry. The module already Init()'d it; here we only wire
    // bindings + deterministic seed/clock. Falls back to self-contained creation.
    if (cfg_.services) {
        if (script::IScriptHost* h = cfg_.services->Get<script::IScriptHost>()) {
            hosts_.lua = ScriptHosts::NonOwning(h);
            injectedScriptHost_ = true;
        }
    }
    if (!hosts_.lua) {
        hosts_.lua = script::CreateLuaHost();
        if (!hosts_.lua) {
            Stop();
            return core::Status::Err("runtime: failed to create script host");
        }
        if (!hosts_.lua->Init()) {
            Stop();
            return core::Status::Err("runtime: failed to initialize script host");
        }
    }
    script::RegisterEngineBindings(*hosts_.lua, scriptCtx_);
    hosts_.lua->SetRngSeed(cfg_.rngSeed ? cfg_.rngSeed : 1u); // 0 aliases seed 1
    hosts_.lua->SetSimClock(0.0);

    // 注入引擎内嵌 Gameplay 基础库（项目脚本之前执行）。
    if (hosts_.lua) {
        if (hosts_.lua->Load(neon::embedded::kGameplayLibLua))
            (void)hosts_.lua->Run();
        else
            NEON_LOG_ERROR("runtime: Gameplay 基础库加载失败: %s",
                           hosts_.lua->LastError().message.c_str());
    }

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
    chunkHandlers_.clear();

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

void GameRuntime::EmitParticles(const gfx::EmitterConfig& cfg) {
    sceneParticles_.Emit(cfg);
}

// Script-facing forwarder to the ProjectileSystem (the bindings and the
// spawnProjectile ScriptContext hook call this; the simulation + rendering
// live entirely inside ProjectileSystem).
void GameRuntime::SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed,
                                  float damage, float life, ecs::Entity caster, float range,
                                  float hitRadius, const std::vector<SkillStatus>& statuses) {
    projectiles_.Spawn(pos, dir, speed, damage, life, caster, range, hitRadius, statuses);
}

void GameRuntime::Stop() {
    ui_.reset();
    physicsAccum_ = 0.0f;
    scripts_.clear();
    trees_.clear();
    draws_.clear();
    projectiles_.Clear();
    tweens_.Clear();
    sceneParticles_.Reset();
    lagComp_.Clear();
    signalHandlers_.clear();
    pendingScene_.clear();
    loadedScripts_.clear();
    scriptFailed_.clear();
    chunkHandlers_.clear(); // handles die with the hosts below
    if (plugins_) {
        plugins_->Stop();
        plugins_.reset();
    }
    if (hosts_.lua) {
        if (!injectedScriptHost_) hosts_.lua->Shutdown();  // the module owns an injected host
        hosts_.lua.reset();
        injectedScriptHost_ = false;
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
            // B14: static bodies never move after registration -- skip the
            // per-frame GetPosition (Jolt map lookup) entirely. Dynamic bodies
            // (and characters below) keep syncing.
            if (!rb.dynamic) return;
            t.pos = physics_->GetPosition({rb.bodyId});
            (void)e;
        });
    world_.ViewAll<SceneCharacter, SceneTransform>().ForEach(
        [this](ecs::Entity, const SceneCharacter& c, SceneTransform& t) {
            if (c.bodyId == 0) return;
            t.pos = physics_->GetPosition({c.bodyId});
        });
}

// G8-3: plays every scene audio source once at its entity's position through
// the host's playSfx3D hook (one-shot; ambient looping needs a loop-3D hook).
void GameRuntime::RegisterAudioSources() {
    if (!cfg_.playSfx3D) return; // headless hosts have no audio sink
    world_.ViewAll<SceneAudioSource, SceneTransform>().ForEach(
        [this](ecs::Entity, const SceneAudioSource& a, const SceneTransform& t) {
            if (a.sound.empty()) return;
            cfg_.playSfx3D(a.sound, t.pos);
        });
}

bool GameRuntime::ShowUI(const std::string& path) {
    return ui_ && ui_->Show(path);
}

void GameRuntime::HideUI() {
    if (ui_) ui_->Hide();
}

void GameRuntime::UISetText(const std::string& name, const std::string& text) {
    if (ui_) ui_->SetText(name, text);
}

void GameRuntime::UISetFill(const std::string& name, float fill) {
    if (ui_) ui_->SetFill(name, fill);
}

void GameRuntime::UISetVisible(const std::string& name, bool visible) {
    if (ui_) ui_->SetVisible(name, visible);
}

void GameRuntime::UISetColor(const std::string& name, float r, float g, float b, float a) {
    if (ui_) ui_->SetColor(name, r, g, b, a);
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
        // otherwise double-run the wrong counter every tick).
        host->SetGlobal("on_start", script::Value::Nil());
        host->SetGlobal("on_update", script::Value::Nil());
        if (!host->Run().Ok()) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' failed to run: %s (skipped)",
                         full.c_str(), host->LastError().message.c_str());
            scriptFailed_.insert(loadKey);
            return false;
        }
        // Capture THIS chunk's handlers right here, while its globals are
        // still the ones it declared, and cache the handles. A captured
        // function handle keeps referencing the original function value even
        // after a later chunk overwrites the globals, so every attach below
        // reuses its OWN chunk's handlers instead of re-capturing whatever
        // chunk owns on_start/on_update at attach time (spawned peas/zombies
        // used to silently run the SUN script and fall out of the sky).
        ChunkHandlers ch;
        if (const auto h = host->CaptureFunction("on_start"); h.Ok()) ch.onStart = h.Value();
        if (const auto h = host->CaptureFunction("on_update"); h.Ok()) ch.onUpdate = h.Value();
        chunkHandlers_[loadKey] = ch;
        loadedScripts_.insert(loadKey);
    }

    scripts_.push_back({ent, s.path, host, 0, 0, false, script::Value::Tbl()});
    ScriptInst& inst = scripts_.back();

    // A6: the component's declared vars live on the INSTANCE now. They are
    // injected into the host globals just before each call and read back
    // after, so entities with the same script keep independent vars.
    if (s.vars.IsObject()) {
        for (const auto& kv : s.vars.Members()) {
            inst.vars.table->fields.emplace_back(kv.first, bt::JsonToValue(kv.second));
        }
    }

    // Reuse the cached handles of THIS chunk (see the capture comment above).
    const ChunkHandlers& ch = chunkHandlers_[loadKey];
    inst.onStart = ch.onStart;
    inst.onUpdate = ch.onUpdate;
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
    // Capture everything we need BEFORE the call: the script call below may
    // SpawnPrefab()/Despawn(), which can reallocate `scripts_` and invalidate
    // the `inst` reference. host/path/vars are local copies so no code path
    // touches `inst` after the call.
    script::IScriptHost* host = inst.host;
    const std::string path = inst.path;
    const ecs::Entity ent = inst.ent;
    host->SetCurrentScript(path);
    // A6: inject this instance's declared vars before the call and save them
    // back after, giving per-entity isolation over the shared global namespace.
    // The vars Value is copied up front: the copy shares the same heap table
    // (Value holds a shared_ptr), so writes through the copy still reach the
    // (possibly relocated) instance's vars.
    const script::Value vars = inst.vars; // shared_ptr copy: same table
    const bool hasVars = vars.type == script::Value::Type::Table && vars.table;
    if (hasVars) {
        for (const auto& kv : vars.table->fields) host->SetGlobal(kv.first, kv.second);
    }
    const auto res = host->CallCaptured(handle, args);
    if (hasVars) {
        for (auto& kv : vars.table->fields) {
            if (auto g = host->GetGlobal(kv.first); g.Ok()) kv.second = g.Value();
        }
    }
    scriptCtx_.currentEntity = {};
    // `inst` may be dangling here (the script could have reallocated scripts_
    // via SpawnPrefab); re-find the instance by entity to preserve the
    // once-per-instance error-log dedup without touching the stale reference.
    if (!res.Ok()) {
        bool logged = false;
        for (auto& e : scripts_) {
            if (e.ent == ent) {
                if (e.errorLogged) logged = true;
                e.errorLogged = true;
                break;
            }
        }
        if (!logged) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' %s() failed: %s", path.c_str(), fn,
                         host->LastError().message.c_str());
        }
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
        // ("bt:<name>") resolving to assets/behaviors/<name>.bt.json under the
        // script base dir. The same ReadScript path used for Lua scripts honors
        // the readScript override (pack readers) and the disk reader, so packed
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
            treeText = ReadScript(FullScriptPath("assets/behaviors/" + name + ".bt.json"));
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

void GameRuntime::TickAnimations(float dt) {
    for (DrawItem& d : draws_) {
        if (!d.skinned || !d.skinned->Valid()) continue;
        // G5-4-4(�?): data-driven animation state machine. Advance it (params
        // from the script-set map), then map the current state's clip onto the
        // existing animClip/animTime override path below �?no pose surgery.
        if (d.animSM) {
            if (!d.animSMBound) {
                anim::BindStateMachineClips(*d.animSM, d.skinned->clips);
                d.animSMBound = true;
                if (!d.animSM->States().empty()) {
                    // Start on the first state so Update() has a current_.
                    const std::string first = d.animSM->States()[0].name;
                    d.animSM->Play(first);
                    d.animSMState = first;
                    const anim::AnimationClip* c = d.animSM->StateClip(0);
                    if (c) {
                        d.animClip = c;
                        d.animName = d.animSM->States()[0].clipName;
                        d.animLoop = true;
                        d.animSpeed = 1.0f;
                        d.animTime = 0.0f;
                        d.animHasOverride = true;
                    }
                }
            }
            for (const auto& [name, value] : d.animSMParams)
                d.animSM->SetParam(name, value);
            d.animSM->Update(dt);
            const std::string state = d.animSM->CurrentState();
            if (!state.empty() && state != d.animSMState) {
                d.animSMState = state;
                const anim::AnimationClip* clip = nullptr;
                std::string clipName;
                for (const anim::AnimState& s : d.animSM->States())
                    if (s.name == state) {
                        clip = s.clip;
                        clipName = s.clipName;
                        break;
                    }
                if (clip) {
                    d.animClip = clip;
                    d.animName = clipName;
                    d.animLoop = true;
                    d.animSpeed = 1.0f;
                    d.animTime = 0.0f;
                    d.animHasOverride = true;
                }
            }
            // Fall through: the override branch below advances animTime for the
            // current state's clip.
        }
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
    hud_.Tick(dt);
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

bool GameRuntime::AttachStateMachine(ecs::Entity e, const std::string& path) {
    if (!world_.Alive(e)) return false;
    DrawItem* d = nullptr;
    for (DrawItem& di : draws_)
        if (di.ent == e) {
            d = &di;
            break;
        }
    if (!d || !d->skinned || !d->skinned->Valid()) return false;
    const std::string text = ReadScript(FullScriptPath(path));
    if (text.empty()) return false;
    auto res = anim::LoadStateMachineJson(text);
    if (!res.Ok()) {
        NEON_LOG_ERROR("asm: '%s': %s", path.c_str(), res.Error().c_str());
        return false;
    }
    d->animSM = std::make_shared<anim::AnimationStateMachine>(res.Value());
    d->animSMState.clear();
    d->animSMBound = false;
    d->animSMParams.clear();
    return true;
}

void GameRuntime::SetAnimParam(ecs::Entity e, const std::string& name, float value) {
    for (DrawItem& di : draws_)
        if (di.ent == e && di.animSM) {
            di.animSMParams[name] = value;
            return;
        }
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
    return hud_.WorldToScreen(world, outX, outY);
}

bool GameRuntime::ScreenToWorld(const math::Vec2& screen, float& outX, float& outY) const {
    return hud_.ScreenToWorld(screen, outX, outY);
}

float GameRuntime::DesignWidth() const {
    return hud_.DesignWidth();
}

void GameRuntime::SpawnFloatText(const math::Vec3& world, const std::string& text, bool crit,
                                 float life) {
    hud_.SpawnFloatText(world, text, crit, life);
}

void GameRuntime::SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac) {
    if (!world_.Alive(e)) return;
    hud_.SetEntityPlate(e, name, hpFrac);
}

void GameRuntime::SetPostFx(bool ssao, bool volumetric, bool ssr,
                            float ssaoIntensity, float volIntensity,
                            float ssrIntensity) {
    postSsao_ = ssao;
    postVolumetric_ = volumetric;
    postSsr_ = ssr;
    postSsaoIntensity_ = ssaoIntensity;
    postVolumetricIntensity_ = volIntensity;
    postSsrIntensity_ = ssrIntensity;
}

void GameRuntime::Draw(gfx::Renderer& renderer, const gfx::Camera& camera,
                       float previewZoom) {
    if (!running_ || !cfg_.assets) return; // sim-only runtime draws nothing
    core::ScopedTimer drawTimer("runtime.draw");
    // Post-process FX overrides (mirrors the editor toggles so play matches).
    renderer.SetSsaoEnabled(postSsao_);
    renderer.SetSsaoIntensity(postSsaoIntensity_);
    renderer.SetVolumetricEnabled(postVolumetric_);
    renderer.SetVolumetricIntensity(postVolumetricIntensity_);
    renderer.SetSsrEnabled(postSsr_);
    renderer.SetSsrIntensity(postSsrIntensity_);
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    gfx::Camera cam = camera;
    bool usedCameraEntity = false;
    // Script-driven FPS game camera: while the "cameraMouseLock" GameVar is
    // truthy, the script owns the rendered view through cameraFocus (placed at
    // eye + viewDir * cameraDist by the controller) plus cameraYaw/cameraPitch/
    // cameraDist �?the same GameVars the host orbit cameras publish. This
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
    // The camera is FAITHFUL: the scene camera's orthoSize/fov is exactly
    // what the scene authored (no fit-outside overrides); the aspect follows
    // the active scene viewport, so the world fills the dock.
    const float drawAspect = renderer.SceneAspect();
    // Snapshot the resolved camera + viewport pixels: WorldToScreen/
    // ScreenToWorld and GetViewportSize answer script queries between renders
    // from this state. UI/world space is plain viewport PIXELS (no design
    // resolution - relative layout adapts, px stays px).
    const math::Rect2& sceneVp = renderer.SceneViewport();
    hud_.CaptureView(cam, drawAspect,
                     sceneVp.w > 0.0f ? renderer.UIDesignSize().x : 1280.0f,
                     sceneVp.h > 0.0f ? renderer.UIDesignSize().y : 720.0f);
    // Project at the ACTIVE scene viewport's aspect (a dock sub-rect in the
    // editor, the full target in the standalone player) so the runtime render
    // matches whatever rasterization rect the host set up - otherwise the
    // playtest FOV would differ from the edit-mode viewport.
    renderer.SetCamera(cam, drawAspect);
    // G3: the editor may have already run the cascade shadow pass with its free
    // orbit camera this frame (before play resolved its game camera). Re-run it
    // for the RESOLVED camera so the shadow frusta track the game view, not the
    // editor's; otherwise orbiting the editor camera slides the shadows.
    renderer.RefreshShadowPass();
    // Data-driven scene environment: apply the scene's DirectionalLight +
    // AmbientLight objects (Unity-style) so every host renders the same scene
    // the same way (the editor's playtest and the standalone player both go
    // through Draw). Fog is pushed far for ortho/2D cameras so the flat sprites
    // are never tinted by a depth gradient.
    {
        const scene::SceneLight* directional = nullptr;
        const scene::SceneLight* ambient = nullptr;
        world_.ViewAll<scene::SceneLight>().ForEach(
            [&](ecs::Entity, const scene::SceneLight& l) {
                if (l.type == "directional" && !directional) directional = &l;
                else if (l.type == "ambient" && !ambient) ambient = &l;
            });
        renderer.SetSky({0.28f, 0.38f, 0.58f, 1.0f}, {0.55f, 0.65f, 0.8f, 1.0f});
        if (cam.ortho) {
            renderer.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 1e9f, 1e10f);
        } else {
            renderer.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 60.0f, 220.0f);
        }
        if (directional) {
            const gfx::Color sun{directional->color.r * directional->intensity,
                                 directional->color.g * directional->intensity,
                                 directional->color.b * directional->intensity,
                                 directional->color.a};
            renderer.SetDirectionalLight(directional->sunDir, sun, 0.0f);
        } else {
            renderer.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {0.8f, 0.8f, 0.8f}, 0.0f);
        }
        if (ambient) renderer.SetAmbientLight(ambient->color, ambient->ambientStrength);
        renderer.DrawSky();
    }
    // Scripts may have spawned/despawned sprite entities since the last frame.
    BuildDrawList();
    // G1-3: refresh the world-transform cache (parent-before-child, arbitrary
    // depth) once per frame; the BVH pass and the draw loop read it instead of
    // re-walking parent chains per entity.
    RebuildWorldTransforms();
    // M1 HUD anchors: project every drawn entity's world position (plus a
    // per-plate head offset) into design units for on_render scripts. Cached
    // once per frame; WorldToScreen() below uses the same matrices. The
    // world-position pass (plate AABB head offsets) stays here; HudSystem
    // projects the resulting entity+world pairs into screen anchors.
    {
        std::vector<std::pair<ecs::Entity, math::Vec3>> anchorEnts;
        anchorEnts.reserve(draws_.size());
        for (const DrawItem& d : draws_) {
            const math::Mat4 model = CachedLocalToWorld(d.ent);
            math::Vec3 wp{model.m[3], model.m[7], model.m[11]};
            // Head offset: lift the anchor above the model bounds when the
            // entity carries a plate (the script stamps names via
            // SetEntityPlate; default 0 keeps the raw position).
            auto pit = hud_.EntityPlates().find(EntityKey(d.ent));
            if (pit != hud_.EntityPlates().end() && pit->second.hpFrac >= 0.0f) {
                const SceneTransform* tr = world_.Get<SceneTransform>(d.ent);
                if (tr) {
                    // Plate tracks the RENDERED mesh, which for a skinned rig
                    // can sit off the entity pivot (the wolf's bones place the
                    // body away from its origin). Compute the world AABB with
                    // the same transform chain Draw() uses �?model *
                    // part.localTransform * bone matrix �?and center the bar on
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
            anchorEnts.emplace_back(d.ent, wp);
        }
        hud_.UpdateAnchors(anchorEnts);
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
            ResolveOrSkip(item, renderer);
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
                SelectLodMesh(item.mesh, item.chain, worldPos, cam.position);
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
        ResolveOrSkip(item, renderer);
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
            gfx::Mesh drawMesh = SelectLodMesh(item.mesh, item.chain, worldPos, cam.position);
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
                renderer.DrawSkinnedMesh(part.mesh, part.material, model,
                                         bones, static_cast<int>(bones.size()));
        } else if (item.isSprite) {
            if (item.billboard) {
                // Camera-facing quad (world-space VFX): rebuild the model
                // basis from the view vector each frame. A 2D front-ortho
                // camera degenerates to the identity basis, so 2D sprites are
                // unaffected.
                math::Vec3 fwd = cam.position - worldPos;
                const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
                if (fl > 0.0001f) fwd = fwd * (1.0f / fl);
                math::Vec3 right = math::Cross(math::Vec3{0.0f, 1.0f, 0.0f}, fwd);
                const float rl = std::sqrt(right.x * right.x + right.y * right.y +
                                           right.z * right.z);
                if (rl > 0.0001f) right = right * (1.0f / rl);
                math::Vec3 up = math::Cross(fwd, right);
                // Scale magnitude recovered from the composed model matrix.
                const float sx = std::sqrt(model.m[0] * model.m[0] + model.m[4] * model.m[4] +
                                           model.m[8] * model.m[8]);
                const float sy = std::sqrt(model.m[1] * model.m[1] + model.m[5] * model.m[5] +
                                           model.m[9] * model.m[9]);
                math::Mat4 bb;
                bb.m[0] = right.x * sx; bb.m[4] = right.y * sx; bb.m[8] = right.z * sx;
                bb.m[1] = up.x * sy;    bb.m[5] = up.y * sy;    bb.m[9] = up.z * sy;
                bb.m[2] = fwd.x;        bb.m[6] = fwd.y;        bb.m[10] = fwd.z;
                bb.m[12] = worldPos.x;  bb.m[13] = worldPos.y;  bb.m[14] = worldPos.z;
                renderer.DrawMesh(item.mesh, item.mat, bb);
            } else {
                // Flip mirrors the quad around its center: a negative local scale
                // keeps the texture upright and needs no UV/shader changes.
                if (item.flipX || item.flipY)
                    model = model * math::Mat4::Scale({item.flipX ? -1.0f : 1.0f,
                                                       item.flipY ? -1.0f : 1.0f, 1.0f});
                renderer.DrawMesh(item.mesh, item.mat, model);
            }
        } else {
            renderer.DrawMesh(SelectLodMesh(item.mesh, item.chain, worldPos, cam.position),
                              item.mat, model);
        }
    }
    flushBatches();
    // Skill projectiles (fireballs): bright glowing orbs (ProjectileSystem
    // lazily builds the shared fireball mesh on first use).
    projectiles_.Draw(renderer);
    // G2-3 vegetation: instanced plant meshes + far yaw-billboard impostors.
    // Use the RESOLVED scene camera (`cam`, which may have been overridden by a
    // scene Camera3D entity driven by the game script) rather than the raw
    // fallback `camera` param. Otherwise the far-tree impostor cards (which
    // yaw to face the camera) would follow the host's free/orbit camera, so
    // right-drag orbits the foliage billboards while the world stays put.
    DrawVegetation(renderer, cam);
    // Script VFX particles: world-space camera-facing billboards (additive +
    // alpha batches), drawn INSIDE the HDR target so bright particles bloom.
    sceneParticles_.Draw(renderer);
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
        // G5-4-4: the on_render canvas is HUD �?the host flushes it AFTER the
        // scene is composited (FlushCanvas), so its colors stay exactly as
        // authored instead of being tone-mapped/bloomed with the 3D scene.
        // FlushDraw2D is called from FlushCanvas (post-EndScene).
    }

    // Data-driven UI (IUiSystem) input/click handling runs in Tick (before
    // on_update) - see the Update call there. DrawUI below only renders.
}

void GameRuntime::DrawUI(gfx::Renderer& renderer) {
    if (!running_ || !cfg_.assets || !ui_ || !cfg_.font2d.Valid()) return;
    scriptCtx_.screenToUi = [this](const math::Vec2& p) {
        return (p - uiOffset_) / uiScale_;
    };
    // The live design-space viewport drives layout: the UI adapts to whatever
    // 2D mapping the host installed (fixed 1280x720 letterbox or a dynamic
    // width under a constant-height mapping).
    ui_->Draw(renderer, cfg_.font2d,
              [this](const std::string& p) {
                  if (!cfg_.assets || p.empty()) return gfx::Texture{};
                  return cfg_.assets->LoadTexture(FullAssetPath(p));
              },
              renderer.UIDesignSize());
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
                // Textured quads use downward-v UVs (top row = v0): DrawQuad's
                // DEFAULT is the GL bottom-up convention, which drew every
                // script-canvas DrawSprite upside down.
                renderer.DrawQuad({c.x, c.y}, {c.w, c.h}, {c.r, c.g, c.b, c.a},
                                  c.texture, {0.0f, 0.0f}, {1.0f, 1.0f});
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

void GameRuntime::FlushCanvas(gfx::Renderer& renderer) {
    if (draw2d_.empty()) return;
    FlushDraw2D(renderer);
}

void GameRuntime::InitSystemGraph() {
    if (systemsReady_) return;
    systemsReady_ = true;

    // Thin wrappers turn the runtime's per-frame component sub-tasks into
    // ecs::System instances. Their declared reads/writes are the state each
    // touches �?component typeids for ECS data, and the owning runtime members
    // (DrawItem / Tween / Projectile / cooldown map) for host-owned state. The
    // scheduler derives conflict edges from these, so independent systems run
    // in parallel while write-conflicting ones (statuses/projectiles both write
    // SceneHealth) stay in registration order �?exactly the historical serial
    // semantics, preserved in both Run modes.
    struct FnSystem : ecs::System {
        std::function<void(float, ecs::World&)> fn;
        void Update(float dt, ecs::World& w) override { fn(dt, w); }
    };
    auto sys = [](std::function<void(float, ecs::World&)> fn) {
        auto s = std::make_shared<FnSystem>();
        s->fn = std::move(fn);
        return s;
    };

    // Registration order IS the serial order: tweens, animations, statuses,
    // projectiles (matches the pre-scheduler Tick body).
    systems_.Add("tweens",
                 sys([this](float d, ecs::World& w) { tweens_.Tick(d, w); }),
                 {typeid(SceneTransform)}, {typeid(SceneTransform), typeid(TweenSystem::Tween)});
    systems_.Add("animations",
                 sys([this](float d, ecs::World&) { TickAnimations(d); }),
                 {typeid(SkinnedModel)},
                 {typeid(DrawItem), typeid(SkinnedModel), typeid(HudSystem::FloatText)});
    systems_.Add("statuses",
                 sys([this](float d, ecs::World& w) { status_.Tick(d, w, hosts_.lua.get()); }),
                 {typeid(StatusComponent)},
                 {typeid(StatusComponent), typeid(SceneHealth)});
    systems_.Add("projectiles",
                 sys([this](float d, ecs::World& w) {
                     projectiles_.Tick(d, w, sceneParticles_.Particles());
                 }),
                 {typeid(SceneHealth), typeid(SceneTransform)},
                 {typeid(SceneHealth), typeid(ProjectileSystem::Projectile)});
}

void GameRuntime::Tick(float dt) {
    if (!running_) return;
    core::ScopedTimer tickTimer("runtime.tick");
    // P1-2 debugger: a breakpoint hit latches the host's paused flag during a
    // script call; stop advancing the simulation so the editor can inspect and
    // step before resuming.
    if (hosts_.lua && hosts_.lua->DebuggerPaused()) return;

    sceneParticles_.Update(dt); // world-space VFX particles (script EmitParticles)

    // G7-3: advance the input map's timing clock before scripts query actions,
    // so double-tap / long-press edges are fresh this frame.
    if (cfg_.input) inputMap_.Update(dt, *cfg_.input);

    // UI input runs INSIDE the tick, BEFORE on_update: the click edge
    // (IInput::EndTick resets it at the end of every tick) is still live
    // here, and scripts read UIClicked() in the very same tick.
    if (ui_) {
        const bool clickEdge =
            cfg_.input && cfg_.input->MousePressed(platform::MouseButton::Left);
        math::Vec2 pointer = cfg_.input ? cfg_.input->MousePos() : math::Vec2{};
        if (scriptCtx_.screenToUi) pointer = scriptCtx_.screenToUi(pointer);
        ui_->Update(pointer, clickEdge);
    }

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
    // UI clicks are latch-until-consumed (see IUiSystem::ConsumeClicks): now
    // that every on_update has read Clicked() this tick, clear the latch so
    // the same click never fires twice.
    if (ui_) ui_->ConsumeClicks();

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
    // G5-4-4(�?): the per-frame component sub-tasks run as ECS systems through
    // the SystemScheduler. Serial (default) preserves the exact historical
    // order (tweens -> animations -> statuses -> projectiles);
    // parallelSystems=true lets independent systems overlap on the worker pool.
    // Conflict edges come from the systems' declared component reads/writes
    // (e.g. StatusSystem and the projectile system both write SceneHealth, so
    // they stay in registration order even in parallel mode) �?the parallel
    // result is bit-identical to the serial reference (validated by
    // TestRuntimeM1's determinism check).
    systems_.Run(dt, world_, cfg_.parallelSystems);
    simTime_ += dt;

    // Sequence-frame sprite animation: advance each animated sprite draw item's
    // clock with the FIXED tick dt (deterministic; headless hosts have no
    // draws_ and simply skip). Draw swaps the texture from the current frame.
    if (!draws_.empty()) {
        for (DrawItem& d : draws_) {
            if (!d.isSprite) continue;
            const bool sheet = !d.sheetTex.empty() && d.sheetFrames > 0;
            if (d.spriteFps <= 0.0f) continue;
            if (!sheet && d.spriteFrames.empty()) continue;
            d.spriteAnimTime += dt;
            int frame = static_cast<int>(d.spriteAnimTime * d.spriteFps);
            const int n = sheet ? d.sheetFrames : static_cast<int>(d.spriteFrames.size());
            if (d.spriteLoop) {
                frame = frame % n;
            } else {
                frame = frame < n ? frame : n - 1;
            }
            if (frame != d.spriteFrame) {
                d.spriteFrame = frame;
                if (sheet) {
                    d.spriteTex = d.sheetTex; // texture unchanged; UV window changes
                } else {
                    d.spriteTex = d.spriteFrames[static_cast<size_t>(frame)];
                }
                d.resolved = false; // re-resolve (new frame texture / UV quad)
            }
        }
    }

    // G3-4: snapshot authoritative poses for lag-compensated hit tests. The
    // ring reuses its snapshot maps (B10): no per-tick heap allocation, and
    // eviction is a head advance, not a vector front-erase.
    {
        std::vector<std::pair<uint64_t, math::Vec3>> poses;
        auto view = world_.ViewAll<SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            const ecs::Entity ent = world_.EntityAt<SceneTransform>(i);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (t) poses.emplace_back(EntityKey(ent), t->pos);
        }
        auto bindView = world_.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < bindView.Size(); ++i) {
            const ecs::Entity ent = world_.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* b = world_.Get<script::CTransformBind>(ent);
            if (b) poses.emplace_back(EntityKey(ent), b->pos);
        }
        lagComp_.Record(poses);
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

} // namespace neon::scene
