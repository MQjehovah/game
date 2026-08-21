#include "neon/scene/game_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"
#include "neon/core/pack.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/scene/scene_file.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
    // not prefixes — a scene with an unresolvable key still plays headless.
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
    scriptCtx_.entityKinds.clear();

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
    BuildDrawList();

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
        if (!s || s->backend != "lua") continue;

        // Defense-in-depth: a hand-crafted pack could reference ".." or an
        // absolute path to read arbitrary local files. Reject such scripts.
        if (neon::core::IsUnsafeRelPath(s->path)) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                         "runtime: skipping script '%s' (unsafe path)", s->path.c_str());
            continue;
        }

        const std::string full = FullScriptPath(s->path);
        if (scriptFailed_.count(full)) {
            NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Warn,
                         "runtime: skipping script '%s' (previous load failed)", full.c_str());
            continue;
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
                continue;
            }
            if (!host_->Load(source)) {
                NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                             "runtime: script '%s' failed to compile: %s (skipped)",
                             full.c_str(), host_->LastError().message.c_str());
                scriptFailed_.insert(full);
                continue;
            }
            if (!host_->Run().Ok()) {
                NEON_LOG_CAT(neon::core::LogCategory::Script, neon::core::LogLevel::Error,
                             "runtime: script '%s' failed to run: %s (skipped)",
                             full.c_str(), host_->LastError().message.c_str());
                scriptFailed_.insert(full);
                continue;
            }
            loadedScripts_.insert(full);
        }

        scripts_.push_back({ent, s->path});
        ScriptInst& inst = scripts_.back();

        // Per-entity script vars become Lua globals so on_start/on_update can
        // read them (e.g. `aggro`). Globals persist, so across entities the
        // last-set value wins for all of them (documented single-host caveat).
        if (s->vars.IsObject()) {
            for (const auto& kv : s->vars.Members()) {
                host_->SetGlobal(kv.first, bt::JsonToValue(kv.second));
            }
        }

        if (host_->HasFunction("on_start")) {
            CallEntityFunction("on_start", inst, {EntityToValue(ent)});
        }
    }
}

void GameRuntime::CallEntityFunction(const char* fn, ScriptInst& inst,
                                     const std::vector<script::Value>& args) {
    if (!host_) return;
    auto res = host_->Call(fn, args);
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
        item.mat = gfx::Material::Lit({}, ParseColorHex(m->colorHex), 24.0f);
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
    // dropped — the chain degrades to the levels that resolved.
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
        // Flat-ground fallback: the editor's heightfield terrain has no file;
        // render a large flat plane so playtest stays visually useful.
        mesh = gfx::Mesh::CreatePlane(renderer, 60.0f, 60.0f, 24, 24, "terrain");
    }
    return mesh;
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
    // Compact when a fifth of the draw list belongs to dead entities.
    if (dead && dead * 5 > draws_.size()) {
        draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                    [this](const DrawItem& i) { return !world_.Alive(i.ent); }),
                     draws_.end());
    }
}

void GameRuntime::Tick(float dt) {
    if (!running_) return;

    if (host_) {
        host_->SetSimClock(simTime_);
        if (host_->HasFunction("on_update")) {
            for (ScriptInst& inst : scripts_) {
                if (!world_.Alive(inst.ent)) continue;
                CallEntityFunction("on_update", inst,
                                   {EntityToValue(inst.ent), script::Value::Num(dt)});
            }
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
