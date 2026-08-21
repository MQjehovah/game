#include "neon/scene/game_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/scene/scene_file.hpp"

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

} // namespace

core::Status GameRuntime::Start(const std::string& sceneJson, GameRuntimeConfig cfg) {
    Stop(); // idempotent: a Start always begins from a fresh state

    auto parsed = SceneFile::Parse(sceneJson);
    if (!parsed.Ok()) return core::Status::Err("runtime: " + parsed.Error());

    // Mesh keys are resolved lazily at Draw time (file-backed "obj:"/"gltf:"
    // plus procedural primitives), so instantiation validates structure but
    // not prefixes — a scene with an unresolvable key still plays headless.
    ComponentRegistry reg;
    RegisterBuiltinComponents(reg, /*assets=*/nullptr);
    PrefabLibrary prefs;
    auto inst = Instantiate(world_, parsed.Value(), prefs, reg);
    if (!inst.Ok()) return core::Status::Err("runtime: " + inst.Error());

    cfg_ = std::move(cfg);

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
    } else {
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                     "runtime: meshKey '%s' has no known loader/procedural prefix (skipped)",
                     key.c_str());
        item.failed = true;
        return;
    }
    if (!mesh.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                     "runtime: mesh '%s' failed to load (skipped)", key.c_str());
        item.failed = true;
        return;
    }
    item.mesh = mesh;
    item.resolved = true;
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
        renderer.DrawMesh(item.mesh, item.mat, model);
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
