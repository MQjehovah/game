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
    ComponentRegistry reg;
    RegisterBuiltinComponents(reg, /*assets=*/nullptr);
    LoadPrefabs(); // scene entities may reference prefabs by name (packed games)
    auto inst = Instantiate(world_, parsed.Value(), prefs_, reg);
    if (!inst.Ok()) return core::Status::Err("runtime: " + inst.Error());

    scriptCtx_.world = &world_;
    scriptCtx_.physics = &physics_;
    scriptCtx_.input = cfg_.input;
    scriptCtx_.playSfx = cfg_.playSfx;
    scriptCtx_.entityKinds.clear();
    // Data files (levels/*.json etc.) resolve like scripts: project dir on
    // disk, or the unpacked dir for packed games (ReadScript honors the pack
    // reader override).
    scriptCtx_.readData = [this](const std::string& path) {
        return ReadScript(FullScriptPath(path));
    };
    scriptCtx_.loadTexture = [this](const std::string& path) {
        if (!cfg_.assets || path.empty()) return gfx::TextureHandle{};
        return cfg_.assets->LoadTexture(FullAssetPath(path)).Handle();
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

    host_ = script::CreateLuaHost();
    if (!host_) {
        Stop();
        return core::Status::Err("runtime: failed to create script host");
    }
    if (!host_->Init()) {
        Stop();
        return core::Status::Err("runtime: failed to initialize script host");
    }
    script::RegisterEngineBindings(*host_, scriptCtx_);
    host_->SetRngSeed(cfg_.rngSeed ? cfg_.rngSeed : 1u); // 0 aliases seed 1
    host_->SetSimClock(0.0);

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
    scripts_.clear();
    trees_.clear();
    draws_.clear();
    projectiles_.clear();
    skillCooldowns_.clear();
    loadedScripts_.clear();
    scriptFailed_.clear();
    if (host_) {
        host_->Shutdown();
        host_.reset();
    }
    world_.Clear();
    physics_.Clear();
    scriptCtx_ = script::ScriptContext{};
    prefs_ = PrefabLibrary{};
    running_ = false;
    simTime_ = 0.0;
}

void GameRuntime::AttachScripts() {
    if (!host_) return;
    auto view = world_.ViewAll<SceneScript>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScript>(i);
        const SceneScript* s = world_.Get<SceneScript>(ent);
        if (s) AttachOneScript(ent, *s);
    }
}

bool GameRuntime::AttachOneScript(ecs::Entity ent, const SceneScript& s) {
    if (!host_ || s.backend != "lua") return false;

    // Defense-in-depth: a hand-crafted pack could reference ".." or an
    // absolute path to read arbitrary local files. Reject such scripts.
    if (neon::core::IsUnsafeRelPath(s.path)) {
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                     "runtime: skipping script '%s' (unsafe path)", s.path.c_str());
        return false;
    }

    const std::string full = FullScriptPath(s.path);
    if (scriptFailed_.count(full)) {
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                     "runtime: skipping script '%s' (previous load failed)", full.c_str());
        return false;
    }

    // Load + run the chunk once per unique path (defines the global
    // functions); a missing file / syntax error skips every entity that
    // references it without failing the whole runtime.
    if (!loadedScripts_.count(full)) {
        std::string source = ReadScript(full);
        if (source.empty()) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: cannot read script '%s' (skipped)", full.c_str());
            scriptFailed_.insert(full);
            return false;
        }
        if (!host_->Load(source)) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' failed to compile: %s (skipped)",
                         full.c_str(), host_->LastError().message.c_str());
            scriptFailed_.insert(full);
            return false;
        }
        if (!host_->Run().Ok()) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                         "runtime: script '%s' failed to run: %s (skipped)",
                         full.c_str(), host_->LastError().message.c_str());
            scriptFailed_.insert(full);
            return false;
        }
        loadedScripts_.insert(full);
    }

    scripts_.push_back({ent, s.path, 0, 0, false});
    ScriptInst& inst = scripts_.back();

    // Per-entity script vars become Lua globals so on_start/on_update can
    // read them (e.g. `aggro`). Globals persist, so across entities the
    // last-set value wins for all of them (documented single-host caveat).
    if (s.vars.IsObject()) {
        for (const auto& kv : s.vars.Members()) {
            host_->SetGlobal(kv.first, bt::JsonToValue(kv.second));
        }
    }

    // Capture this chunk's handlers so later chunks cannot shadow them.
    if (const auto h = host_->CaptureFunction("on_start"); h.Ok()) inst.onStart = h.Value();
    if (const auto h = host_->CaptureFunction("on_update"); h.Ok()) inst.onUpdate = h.Value();
    if (inst.onStart != 0) {
        CallEntityFunctionHandle(inst, inst.onStart, "on_start", {EntityToValue(ent)});
    }
    return true;
}

void GameRuntime::CallEntityFunctionHandle(ScriptInst& inst, uint64_t handle,
                                           const char* fn,
                                           const std::vector<script::Value>& args) {
    if (!host_ || handle == 0) return;
    // The input bindings resolve per-entity input through the entity being
    // updated (multi-player: each player's script reads its OWN client input).
    scriptCtx_.currentEntity = inst.ent;
    const auto res = host_->CallCaptured(handle, args);
    scriptCtx_.currentEntity = {};
    if (!res.Ok() && !inst.errorLogged) {
        inst.errorLogged = true;
        NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                     "runtime: script '%s' %s() failed: %s", inst.path.c_str(), fn,
                     host_->LastError().message.c_str());
    }
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
    auto view = world_.ViewAll<SceneMesh>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneMesh>(i);
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
}

void GameRuntime::ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer) {
    if (item.resolved || item.failed || !cfg_.assets) return;

    const std::string& key = item.meshKey;
    gfx::Mesh mesh = ResolveMeshKey(renderer, key);
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

gfx::Mesh GameRuntime::ResolveMeshKey(gfx::Renderer& renderer, const std::string& key) {
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
        mesh = gfx::MakeTerrainMesh(renderer);
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
    return host_ && host_->HasFunction(name);
}

bool GameRuntime::CallScriptFunction(const std::string& name,
                                     const std::vector<script::Value>& args) {
    if (!host_ || !host_->HasFunction(name)) return false;
    scriptCtx_.currentEntity = {};
    const core::Result<script::Value> res = host_->Call(name, args);
    if (!res.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                     "runtime: script function %s() failed: %s", name.c_str(),
                     host_->LastError().message.c_str());
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
    if (renderer.ScreenHeight() > 0) {
        float aspect = static_cast<float>(renderer.ScreenWidth()) / renderer.ScreenHeight();
        renderer.SetCamera(camera, aspect);
    }
    size_t dead = 0;
    for (DrawItem& item : draws_) {
        if (!world_.Alive(item.ent)) {
            ++dead; // scripts can Despawn entities mid-playtest
            continue;
        }
        if (!item.resolved) ResolveDrawItem(item, renderer);
        if (!item.resolved || item.failed) continue;
        const SceneTransform* t = world_.Get<SceneTransform>(item.ent);
        if (!t) continue;
        math::Mat4 model =
            math::Mat4::Translation(t->pos) * t->rot.ToMat4() * math::Mat4::Scale(t->scale);
        renderer.DrawMesh(SelectLodMesh(item.mesh, item.chain, t->pos, camera.position),
                          item.mat, model);
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
    if (host_) {
        scriptCtx_.draw2d = &draw2d_;
        scriptCtx_.screenToUi = [&renderer](const math::Vec2& p) {
            return renderer.ScreenToUI(p);
        };
        draw2d_.clear();
        scriptCtx_.currentEntity = {};
        if (host_->HasFunction("on_render")) {
            const core::Result<script::Value> res = host_->Call("on_render", {});
            if (!res.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                             "runtime: on_render() failed: %s",
                             host_->LastError().message.c_str());
            }
        }
        scriptCtx_.draw2d = nullptr;
        FlushDraw2D(renderer);
    }
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

    if (host_) {
        host_->SetSimClock(simTime_);
        for (ScriptInst& inst : scripts_) {
            if (!world_.Alive(inst.ent)) continue;
            if (inst.onUpdate == 0) continue; // this chunk defines no on_update
            CallEntityFunctionHandle(inst, inst.onUpdate, "on_update",
                                     {EntityToValue(inst.ent), script::Value::Num(dt)});
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

    physics_.Step(dt, math::Vec3{0.0f, kGravityY, 0.0f});
    TickStatuses(dt);
    TickSkillCooldowns(dt);
    TickProjectiles(dt);
    simTime_ += dt;
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
