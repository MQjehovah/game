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
#include "neon/physics/jolt_world.hpp"
#include "neon/plugin/backend.hpp"
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
    // B1 NavGrid: a scene can declare a data-driven navigation grid via
    // level.navgrid (a .navgrid.json asset path). Load it and wire it into the
    // script context so the NavFindPath Lua binding can path-find around world
    // obstacles. A missing/invalid asset just leaves the grid unset (the AI
    // falls back to beeline movement).
    {
        const core::Json* lv = parsed.Value().level.IsObject() ? &parsed.Value().level : nullptr;
        const core::Json* ng = lv ? lv->Get("navgrid") : nullptr;
        if (ng && ng->IsString() && !ng->GetString().empty()) {
            const std::string text = ReadScript(FullScriptPath(ng->GetString()));
            if (!text.empty()) {
                auto parsedNav = nav::NavGrid::FromJson(text);
                if (parsedNav.Ok()) SetNavGrid(parsedNav.Value());
                else NEON_LOG_WARN("runtime: nav grid '%s' invalid: %s", ng->GetString().c_str(),
                                   parsedNav.Error().c_str());
            }
        }
    }
    // ScriptRuntime reads sources through the same cfg_-backed readers as the
    // rest of the runtime (ReadScript honors the pack override + VFS).
    scriptRuntime_.Configure(
        {cfg_.scriptBaseDir, [this](const std::string& p) { return ReadScript(p); }});
    // BtRuntime resolves "bt:<name>" tree references through the same reader.
    btRuntime_.Configure(
        {cfg_.scriptBaseDir, [this](const std::string& p) { return ReadScript(p); }});
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
    // cfg_.pluginBaseDir/plugins — swappable without relinking. The owning
    // PhysicsBackend is kept alive until this runtime is destroyed (it owns the
    // DLL), and is declared before physics_ so the world dies before the library.
    // Microkernel seam (P-B): prefer an injected physics world from the service
    // registry (non-owning — the module owns it). Falls back to the
    // self-contained creation below when no service is registered. Ownership
    // (world + optional plugin backend) is handed to the PhysicsBridge, which
    // preserves the destroy ordering (world before library).
    if (cfg_.services) {
        if (physics::World* w = cfg_.services->Get<physics::World>())
            physics_.SetWorld(
                std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
                    w, [](physics::World*) {}),  // non-owning
                nullptr);
    }
    if (!physics_.World()) {
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> world(
        new physics::World(), [](physics::World* w) { delete w; });
#ifdef NEON_ENABLE_JOLT
    if (cfg_.physicsBackend == "jolt") {
        world = std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
            new physics::JoltWorld(), [](physics::World* w) { delete w; });
    }
#endif
    std::unique_ptr<plugin::PhysicsBackend> pluginBackend;
    if (cfg_.physicsBackend.rfind("plugin:", 0) == 0 && !cfg_.pluginBaseDir.empty()) {
        const std::string backendName = cfg_.physicsBackend.substr(7);
        pluginBackend = plugin::LoadNativePhysicsBackend(backendName, cfg_.pluginBaseDir);
        if (pluginBackend) {
            std::unique_ptr<physics::World, std::function<void(physics::World*)>> pluginWorld =
                pluginBackend->CreateWorld();
            if (pluginWorld) {
                world = std::move(pluginWorld);
            } else {
                pluginBackend.reset();
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
    physics_.SetWorld(std::move(world), std::move(pluginBackend));
    }  // if (!physics_.World())
    NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Info,
                 "runtime: physics backend '%s' (%zu rigid bodies cap)",
                 cfg_.physicsBackend.c_str(), physics_.BodyCount());
    compReg_ = ComponentRegistry{};
    RegisterBuiltinComponents(compReg_, /*assets=*/nullptr);
    // The PrefabSystem only knows how to build the instance SceneFile; the
    // world/component-factory/script-attach half stays here (it needs world_,
    // compReg_, ScriptRuntime::AttachOne) and is injected as the instantiate
    // callback.
    prefabs_.SetInstantiate([this](const SceneFile& scene) -> ecs::Entity {
        auto inst = Instantiate(world_, scene, prefabs_.Library(), compReg_);
        if (!inst.Ok() || inst.Value() != 1) return {};
        // Locate the created entity by its unique name (baked into the entity
        // name by PrefabSystem::Spawn), then attach its script components
        // (AttachScripts only runs at Start for scene entities; the prefab's
        // on_start fires here, with its custom components already set).
        if (scene.entities.empty()) return {};
        const std::string& uniqueName = scene.entities.front().name;
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
        if (const SceneScript* s = world_.Get<SceneScript>(out))
            scriptRuntime_.AttachOne(out, *s, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
        if (const SceneScripts* list = world_.Get<SceneScripts>(out)) {
            for (const SceneScript& s : list->items)
                scriptRuntime_.AttachOne(out, s, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
        }
        return out;
    });
    LoadPrefabs(); // scene entities may reference prefabs by name (packed games)
    LoadLocales(); // Loc() string tables (best effort; missing dir = no-op)
    auto inst = Instantiate(world_, parsed.Value(), prefabs_.Library(), compReg_);
    if (!inst.Ok()) return core::Status::Err("runtime: " + inst.Error());
    physics_.RegisterBodies(world_);
    physics_.RegisterCharacters(world_);
    RegisterAudioSources();

    scriptCtx_.world = &world_;
    scriptCtx_.physics = physics_.World();
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
    // Owned by uiSystem_ (UiSystem; Task 11) - a thin forwarder to the seam.
    if (cfg_.uiSystem) {
        uiSystem_.Set(cfg_.uiSystem);
    } else {
        ui::DocumentUiConfig ucfg;
        ucfg.readFile = [this](const std::string& path) {
            return ReadScript(FullScriptPath(path));
        };
        uiSystem_.Set(
            std::shared_ptr<ui::IUiSystem>(ui::CreateDocumentUiSystem(ucfg).release()));
    }
    scriptCtx_.uiShow = [this](const std::string& path) { return uiSystem_.Show(path); };
    scriptCtx_.uiHide = [this]() { uiSystem_.Hide(); };
    scriptCtx_.uiClicked = [this](const std::string& name) { return uiSystem_.Clicked(name); };
    scriptCtx_.uiSetText = [this](const std::string& name, const std::string& text) {
        uiSystem_.SetText(name, text);
    };
    scriptCtx_.uiSetFill = [this](const std::string& name, float fill) {
        uiSystem_.SetFill(name, fill);
    };
    scriptCtx_.uiSetVisible = [this](const std::string& name, bool visible) {
        uiSystem_.SetVisible(name, visible);
    };
    scriptCtx_.uiSetColor = [this](const std::string& name, float r, float g, float b,
                                   float a) { uiSystem_.SetColor(name, r, g, b, a); };
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
            scriptRuntime_.AttachOne(e, sc, scriptCtx_,
                                     {hosts_.lua.get(), hosts_.js.get()}); // loads once, captures + runs on_start
        }
        return e;
    };
    // Sequence-frame sprite animation: update the entity's SceneSprite frames
    // and reset its draw item's frame clock so the new animation starts fresh
    // (the draw-item half lives in DrawSystem::SetSpriteFrames; Task 16).
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
        drawSystem_.SetSpriteFrames(e, frames, fps);
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
        drawSystem_.SetSpriteSheet(e, sheet, count, fps);
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
    // The action map itself lives in ScriptRuntime (script-facing state).
    script::InputMap* inputMap = scriptRuntime_.InputMap();
    *inputMap = script::InputMap::Defaults();
    const std::string inputJson = ReadScript(FullScriptPath("input.json"));
    if (!inputJson.empty()) {
        std::string mapErr;
        if (!inputMap->Load(inputJson, &mapErr)) {
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                         "runtime: input.json failed to load: %s (defaults kept)",
                         mapErr.c_str());
        }
    }
    scriptCtx_.inputMap = inputMap;
    inputMap->Reset(); // clear G7-3 timing state across playtest restarts
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
        // next SyncBodies() snaps the entity back and characters walk
        // through walls that only physics knows about.
        physics_.SetBodyPosition(world_, e, p);
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
        scriptRuntime_.AttachOne(e, s, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
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

    // Fresh script-attach state per Start (Stop() already reset it; keep the
    // explicit reset so AttachScripts always starts from a clean slate).
    scriptRuntime_.Clear();

    // Runtime plugins: gameplay/system modules in <scriptBaseDir>/plugins
    // (Lua or JS). They get their own isolated hosts sharing the engine
    // bindings; failures are logged per plugin, never fatal. PluginSystem
    // owns the whole lifecycle (load -> on_load/on_start, tick, stop).
    plugins_.Load(cfg_.scriptBaseDir, [this](const std::string& p) { return ReadScript(p); },
                  &scriptCtx_, cfg_.rngSeed ? cfg_.rngSeed : 1u);

    AttachScripts();
    btRuntime_.AttachAll(world_, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
    // Draw subsystem (Task 16): inject the cfg_-derived asset/path state (fresh
    // per Start; ChangeScene restarts reuse the same DrawSystem instance) so the
    // draw-list build + Draw resolve through the runtime's AssetManager.
    drawSystem_.Configure({cfg_.assets, cfg_.asyncMeshLoad,
                           [this](const std::string& p) { return FullAssetPath(p); }});
    // Headless hosts (servers / sim tests) have no renderer: skip the draw
    // list entirely. Draw() is also a no-op without cfg_.assets, so both the
    // flag and a null AssetManager keep the runtime window-free.
    if (!cfg_.headless) drawSystem_.Build(world_, animations_);

    running_ = true;
    simTime_ = 0.0;
    NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                 "runtime: started (%zu entities, %zu scripts, %zu trees, %zu draws)",
                 EntityCount(), ScriptCount(), BehaviorTreeCount(), drawSystem_.DrawCount());
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
    uiSystem_.Set(nullptr);
    scriptRuntime_.Clear();
    btRuntime_.Clear();
    drawSystem_.Clear();
    animations_.Clear();
    projectiles_.Clear();
    tweens_.Clear();
    sceneParticles_.Reset();
    lagComp_.Clear();
    signalHandlers_.clear();
    pendingScene_.clear();
    plugins_.Shutdown();
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
    physics_.Clear();
    scriptCtx_ = script::ScriptContext{};
    prefabs_.Clear();
    running_ = false;
    simTime_ = 0.0;
}

// G8-3: plays every scene audio source once at its entity's position through
// the host's playSfx3D hook (one-shot; ambient looping needs a loop-3D hook).
// Entity/audio initialization — NOT physics — so it stays on GameRuntime
// (Task 15: the physics bridge owns rigidbody/character registration only).
void GameRuntime::RegisterAudioSources() {
    if (!cfg_.playSfx3D) return; // headless hosts have no audio sink
    world_.ViewAll<SceneAudioSource, SceneTransform>().ForEach(
        [this](ecs::Entity, const SceneAudioSource& a, const SceneTransform& t) {
            if (a.sound.empty()) return;
            cfg_.playSfx3D(a.sound, t.pos);
        });
}

void GameRuntime::AttachScripts() {
    if (!hosts_.lua) return;
    // Collect the scene's script components (single + Unity-style multi) in
    // order; the load/dedup/capture half lives in ScriptRuntime::AttachAll.
    std::vector<std::pair<ecs::Entity, SceneScript>> scripts;
    auto view = world_.ViewAll<SceneScript>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScript>(i);
        const SceneScript* s = world_.Get<SceneScript>(ent);
        if (s) scripts.emplace_back(ent, *s);
    }
    // Unity-style multiple scripts: every entry in a "scripts" component
    // attaches like a single script component.
    auto multiView = world_.ViewAll<SceneScripts>();
    for (size_t i = 0; i < multiView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScripts>(i);
        const SceneScripts* list = world_.Get<SceneScripts>(ent);
        if (!list) continue;
        for (const SceneScript& s : list->items) scripts.emplace_back(ent, s);
    }
    scriptRuntime_.AttachAll(scripts, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
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
        scriptRuntime_.AttachOne(e, s, scriptCtx_, {hosts_.lua.get(), hosts_.js.get()});
    }
    return e;
}

ecs::Entity GameRuntime::SpawnPrefab(const std::string& name, const math::Vec3& pos) {
    return prefabs_.Spawn(name, pos);
}

void GameRuntime::TickAnimations(float dt) {
    // Task 12: the per-entity animation subsystem moved into AnimationSystem
    // (entityKey -> AnimState, registered at ResolveDrawItem). Headless hosts
    // never register states, so this iterates an empty table - no idle
    // animation work (C2).
    animations_.Tick(dt);
    // Floating combat texts age out.
    hud_.Tick(dt);
}

bool GameRuntime::HasScriptFunction(const std::string& name) const {
    return scriptRuntime_.HasFunction({hosts_.lua.get(), hosts_.js.get()}, name);
}

bool GameRuntime::CallScriptFunction(const std::string& name,
                                     const std::vector<script::Value>& args) {
    return scriptRuntime_.CallFunction(scriptCtx_, {hosts_.lua.get(), hosts_.js.get()},
                                       name, args);
}

bool GameRuntime::DispatchPluginEvent(const std::string& name,
                                      const std::vector<script::Value>& args) {
    return plugins_.DispatchEvent(name, args);
}

bool GameRuntime::RunPluginCommand(const std::string& name,
                                   const std::vector<script::Value>& args,
                                   std::string* error) {
    return plugins_.RunCommand(name, args, error);
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
    // can branch on the return value (DrawSystem::HasSkinned, Task 16).
    bool skinned = drawSystem_.HasSkinned(e);
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
    // Drop any cached instance state so the next BuildDrawList sync re-issues
    // it (fresh cross-fade from the current pose). Same semantics as the old
    // DrawItem clear (animName/animClip reset on the entity's draw item).
    animations_.InvalidateOverride(EntityKey(e));
    return true;
}

bool GameRuntime::AttachStateMachine(ecs::Entity e, const std::string& path) {
    if (!world_.Alive(e)) return false;
    const uint64_t key = EntityKey(e);
    // Requires a RESOLVED skinned draw item (equivalent to the old draw-item
    // lookup: d && d->skinned && d->skinned->Valid()).
    if (!animations_.HasBinding(key)) return false;
    const std::string text = ReadScript(FullScriptPath(path));
    if (text.empty()) return false;
    auto res = anim::LoadStateMachineJson(text);
    if (!res.Ok()) {
        NEON_LOG_ERROR("asm: '%s': %s", path.c_str(), res.Error().c_str());
        return false;
    }
    return animations_.AttachStateMachine(
        key, std::make_shared<anim::AnimationStateMachine>(res.Value()));
}

void GameRuntime::SetAnimParam(ecs::Entity e, const std::string& name, float value) {
    animations_.SetParam(EntityKey(e), name, value);
}

float GameRuntime::AnimationProgress(ecs::Entity e) const {
    return animations_.Progress(EntityKey(e));
}

bool GameRuntime::AnimationFinished(ecs::Entity e) const {
    return animations_.Finished(EntityKey(e));
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
    // DrawSystem owns the draw-list build + the whole render orchestration
    // (Task 16). The already-split systems and the shared runtime state
    // (world_/scriptCtx_/hosts_/hiddenEntities_/uiScale_/uiOffset_ + the post-FX
    // toggles from SetPostFx) are wired in as parameters; the runtime keeps the
    // public entry point so the editor/player call sites are unchanged.
    drawSystem_.Draw(
        renderer, camera,
        DrawSystem::DrawParams{previewZoom, postSsao_, postVolumetric_, postSsr_,
                               postSsaoIntensity_, postVolumetricIntensity_,
                               postSsrIntensity_},
        world_, scriptCtx_, hosts_.lua.get(), hosts_.js.get(), hiddenEntities_, hud_,
        sceneTree_, animations_, projectiles_, sceneParticles_, scriptCanvas_,
        uiScale_, uiOffset_);
}

void GameRuntime::DrawUI(gfx::Renderer& renderer) {
    if (!running_ || !cfg_.assets || !uiSystem_.Raw() || !cfg_.font2d.Valid()) return;
    scriptCtx_.screenToUi = [this](const math::Vec2& p) {
        return (p - uiOffset_) / uiScale_;
    };
    // The live design-space viewport drives layout: the UI adapts to whatever
    // 2D mapping the host installed (fixed 1280x720 letterbox or a dynamic
    // width under a constant-height mapping).
    uiSystem_.Draw(renderer, cfg_.font2d,
                   [this](const std::string& p) {
                       if (!cfg_.assets || p.empty()) return gfx::Texture{};
                       return cfg_.assets->LoadTexture(FullAssetPath(p));
                   },
                   renderer.UIDesignSize());
}

// G1-3 scene-tree API + world-transform cache: forwarded to sceneTree_
// (SceneTreeSystem; Task 9). Public surface unchanged (editor/tests).
std::vector<ecs::Entity> GameRuntime::GetChildren(ecs::Entity parent) {
    return sceneTree_.GetChildren(world_, parent);
}

std::vector<ecs::Entity> GameRuntime::GetDescendants(ecs::Entity root) {
    return sceneTree_.GetDescendants(world_, root);
}

void GameRuntime::RebuildWorldTransforms() {
    sceneTree_.Rebuild(world_);
}

math::Mat4 GameRuntime::CachedLocalToWorld(ecs::Entity e) const {
    return sceneTree_.CachedLocalToWorld(e);
}

void GameRuntime::FlushCanvas(gfx::Renderer& renderer) {
    if (scriptCanvas_.Empty()) return;
    scriptCanvas_.Flush(renderer, cfg_.font2d);
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
                 {typeid(SkinnedModel), typeid(HudSystem::FloatText)});
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
    if (cfg_.input) scriptRuntime_.InputMap()->Update(dt, *cfg_.input);

    // UI input runs INSIDE the tick, BEFORE on_update: the click edge
    // (IInput::EndTick resets it at the end of every tick) is still live
    // here, and scripts read UIClicked() in the very same tick.
    if (uiSystem_.Raw()) {
        const bool clickEdge =
            cfg_.input && cfg_.input->MousePressed(platform::MouseButton::Left);
        math::Vec2 pointer = cfg_.input ? cfg_.input->MousePos() : math::Vec2{};
        if (scriptCtx_.screenToUi) pointer = scriptCtx_.screenToUi(pointer);
        uiSystem_.Update(pointer, clickEdge);
    }

    // Both backends share the engine-injected simulated clock.
    if (hosts_.lua) hosts_.lua->SetSimClock(simTime_);
    if (hosts_.js) hosts_.js->SetSimClock(simTime_);
    {
        core::ScopedTimer scriptsTimer("runtime.scripts");
        // Dispatches every alive entity's on_update(ent, dt). Index-based
        // inside ScriptRuntime (SpawnPrefab may push new instances mid-loop;
        // new ones act the same frame they spawn).
        scriptRuntime_.Tick(dt, world_, scriptCtx_);
        // Runtime plugin systems tick after scene scripts (same sim clock).
        plugins_.Tick(dt, simTime_);
    }
    // UI clicks are latch-until-consumed (see IUiSystem::ConsumeClicks): now
    // that every on_update has read Clicked() this tick, clear the latch so
    // the same click never fires twice.
    uiSystem_.ConsumeClicks();

    // Behavior trees: advance every attached tree's entity (Task 14). Moved
    // into BtRuntime (trees_ + per-tick Context construction + dead-tree
    // compaction); GameRuntime forwards with the shared world_/scriptCtx_.
    btRuntime_.Tick(dt, world_, scriptCtx_);

    // Fixed-step physics (PhysicsBridge, Task 15): accumulate the frame delta
    // and advance the world at 60 Hz so collision resolution and scripts stay
    // deterministic regardless of frame rate. The catch-up cap (4 steps) avoids
    // a spiral of death after a hitch. SyncBodies writes the simulated
    // positions back into the entities' transforms.
    physics_.Step(dt, math::Vec3{0.0f, kGravityY, 0.0f});
    physics_.SyncBodies(world_);
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
    // clock with the FIXED tick dt (deterministic; headless hosts have no draw
    // items and simply skip). Draw swaps the texture from the current frame.
    // Forwards to drawSystem_ (DrawSystem; Task 16).
    drawSystem_.AdvanceSprites(dt);

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
    return btRuntime_.BlackboardValue(ent, key);
}

std::string GameRuntime::ActiveTreePath(const ecs::Entity& ent) const {
    return btRuntime_.ActivePath(ent);
}

} // namespace neon::scene
