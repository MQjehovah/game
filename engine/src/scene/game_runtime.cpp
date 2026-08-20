#include "neon/scene/game_runtime.hpp"

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

    scriptSources_.clear();
    scriptFailed_.clear();

    AttachScripts();
    AttachTrees();
    BuildDrawList();

    running_ = true;
    simTime_ = 0.0f;
    NEON_LOG_INFO("runtime: started (%zu entities, %zu scripts, %zu trees, %zu draws)",
                  EntityCount(), ScriptCount(), BehaviorTreeCount(), draws_.size());
    return core::Status::Ok(true);
}

void GameRuntime::Stop() {
    scripts_.clear();
    trees_.clear();
    draws_.clear();
    scriptSources_.clear();
    scriptFailed_.clear();
    if (host_) {
        host_->Shutdown();
        host_.reset();
    }
    world_.Clear();
    physics_.Clear();
    scriptCtx_ = script::ScriptContext{};
    running_ = false;
    simTime_ = 0.0f;
}

void GameRuntime::AttachScripts() {
    if (!host_) return;
    auto view = world_.ViewAll<SceneScript>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneScript>(i);
        const SceneScript* s = world_.Get<SceneScript>(ent);
        if (!s || s->backend != "lua") continue;

        const std::string full = FullScriptPath(s->path);
        if (scriptFailed_[full]) {
            NEON_LOG_WARN("runtime: skipping script '%s' (previous load failed)", full.c_str());
            continue;
        }

        // Load + run the chunk once per unique path (defines the global
        // functions); a missing file / syntax error skips every entity that
        // references it without failing the whole runtime.
        if (scriptSources_.find(full) == scriptSources_.end()) {
            std::string source = ReadScript(full);
            if (source.empty()) {
                NEON_LOG_ERROR("runtime: cannot read script '%s' (skipped)", full.c_str());
                scriptFailed_[full] = true;
                continue;
            }
            if (!host_->Load(source)) {
                NEON_LOG_ERROR("runtime: script '%s' failed to compile: %s (skipped)",
                               full.c_str(), host_->LastError().message.c_str());
                scriptFailed_[full] = true;
                continue;
            }
            if (!host_->Run().Ok()) {
                NEON_LOG_ERROR("runtime: script '%s' failed to run: %s (skipped)",
                               full.c_str(), host_->LastError().message.c_str());
                scriptFailed_[full] = true;
                continue;
            }
            scriptSources_[full] = std::move(source);
        }

        scripts_.push_back({ent, s->path});
        if (host_->HasFunction("on_start")) {
            host_->Call("on_start", {EntityToValue(ent)});
        }
    }
}

void GameRuntime::AttachTrees() {
    auto view = world_.ViewAll<SceneBehaviorTree>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneBehaviorTree>(i);
        const SceneBehaviorTree* bt = world_.Get<SceneBehaviorTree>(ent);
        if (!bt) continue;
        auto tree = std::make_unique<bt::BehaviorTree>();
        std::string err;
        if (!tree->LoadText(bt->treeJson, &err)) {
            NEON_LOG_ERROR("runtime: entity behavior tree failed to load: %s (skipped)",
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
        draws_.push_back(std::move(item));
    }
}

void GameRuntime::ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer) {
    if (item.resolved || item.failed || !cfg_.assets) return;

    const std::string& key = item.meshKey;
    gfx::Mesh mesh;
    if (key.compare(0, 4, "obj:") == 0) {
        mesh = cfg_.assets->LoadMeshOBJ(key.substr(4));
    } else if (key.compare(0, 5, "gltf:") == 0) {
        assets::GltfAsset gltf = cfg_.assets->LoadGLTF(key.substr(5));
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
        NEON_LOG_WARN("runtime: meshKey '%s' has no known loader/procedural prefix (skipped)",
                      key.c_str());
        item.failed = true;
        return;
    }
    if (!mesh.Valid()) {
        NEON_LOG_WARN("runtime: mesh '%s' failed to load (skipped)", key.c_str());
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
    for (DrawItem& item : draws_) {
        if (!item.resolved) ResolveDrawItem(item, renderer);
        if (!item.resolved || item.failed) continue;
        const SceneTransform* t = world_.Get<SceneTransform>(item.ent);
        if (!t) continue;
        math::Mat4 model =
            math::Mat4::Translation(t->pos) * t->rot.ToMat4() * math::Mat4::Scale(t->scale);
        renderer.DrawMesh(item.mesh, item.mat, model);
    }
}

void GameRuntime::Tick(float dt) {
    if (!running_) return;

    if (host_) {
        host_->SetSimClock(simTime_);
        if (host_->HasFunction("on_update")) {
            for (const ScriptInst& inst : scripts_) {
                if (!world_.Alive(inst.ent)) continue;
                auto res = host_->Call("on_update", {EntityToValue(inst.ent), script::Value::Num(dt)});
                (void)res; // script errors are logged by the host, never fatal
            }
        }
    }

    for (BtInst& inst : trees_) {
        bt::Context ctx(scriptCtx_.gameVars, &inst.board);
        ctx.entity = EntityKey(inst.ent);
        ctx.dt = dt;
        ctx.timers.swap(inst.timers); // carry over accumulated wait/cooldown state
        inst.tree->Tick(ctx);
        ctx.timers.swap(inst.timers); // persist it for the next tick
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

std::string GameRuntime::FullScriptPath(const std::string& path) const {
    if (path.empty() || cfg_.scriptBaseDir.empty()) return path;
    return cfg_.scriptBaseDir + "/" + path;
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
