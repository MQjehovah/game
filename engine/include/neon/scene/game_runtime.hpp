#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/physics/physics.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"

namespace neon::assets {
class AssetManager;
}
namespace neon::gfx {
class Renderer;
}

namespace neon::scene {

// Runtime configuration for a headless playtest / game session. Everything is
// optional: a null AssetManager makes Draw a no-op (pure simulation, which is
// what headless servers and unit tests use). Scripts are read from disk under
// scriptBaseDir unless readScript overrides the source (pack readers, tests).
struct GameRuntimeConfig {
    assets::AssetManager* assets = nullptr; // mesh loading; null = sim-only
    std::string scriptBaseDir;              // base dir for script paths ("" = cwd)
    std::function<std::string(const std::string& path)> readScript; // optional override
    uint64_t rngSeed = 20260821u;           // fixed: playtest RNG is reproducible
};

// A self-contained, reusable game runtime. The editor embeds one instance for
// in-editor playtest (F5); the packaged neon_game player reuses the same class
// (T4.7). Lifecycle: Start(sceneJson, cfg) -> Tick/Draw... -> Stop(); Start
// again after Stop starts a fresh world.
//
// One Lua host serves the whole runtime: every SceneScript with backend "lua"
// is loaded once per unique path, then on_start(ent) runs at Start and
// on_update(ent, dt) runs every Tick for each entity carrying the script.
// GameVars is shared by scripts and behavior trees (single global store).
class GameRuntime {
public:
    core::Status Start(const std::string& sceneJson, GameRuntimeConfig cfg);
    void Tick(float dt);
    void Draw(gfx::Renderer& renderer, const gfx::Camera& camera);
    void Stop();

    bool Running() const { return running_; }
    ecs::World& World() { return world_; }
    script::GameVars& GameVars() { return scriptCtx_.gameVars; }

    // Stats for the editor profiler / debug panels.
    size_t EntityCount() const { return world_.EntityCount(); }
    size_t ScriptCount() const { return scripts_.size(); }
    size_t BehaviorTreeCount() const { return trees_.size(); }
    size_t DrawCount() const { return draws_.size(); }
    float SimTime() const { return simTime_; }

    // Observability for tests/debug: the per-entity blackboard value the
    // behavior tree of `ent` wrote under `key` (Nil when the entity has no
    // tree or the key is unset).
    script::Value EntityBlackboardValue(const ecs::Entity& ent, const std::string& key) const;

private:
    struct ScriptInst {
        ecs::Entity ent;
        std::string path;
    };
    struct BtInst {
        ecs::Entity ent;
        std::unique_ptr<bt::BehaviorTree> tree;
        script::Blackboard board;
        // Persistent per-entity timer state. bt::Context owns its timers map
        // but is rebuilt per tick, so the map is parked here between ticks and
        // swapped in/out of the fresh Context (a Context cannot be stored: it
        // binds a GameVars reference and the board address may move).
        std::map<uint64_t, std::map<std::string, float>> timers;
    };
    struct DrawItem {
        ecs::Entity ent;
        std::string meshKey;
        gfx::Mesh mesh;
        gfx::Material mat;
        bool resolved = false;
        bool failed = false;
    };

    void AttachScripts();
    void AttachTrees();
    void BuildDrawList();
    void ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer);
    std::string ReadScript(const std::string& path) const;
    std::string FullScriptPath(const std::string& path) const;

    ecs::World world_;
    physics::World physics_;
    script::ScriptContext scriptCtx_; // owns the GameVars scripts + BT share
    std::unique_ptr<script::IScriptHost> host_; // one Lua host, deterministic RNG
    std::vector<ScriptInst> scripts_;
    std::vector<BtInst> trees_;
    std::vector<DrawItem> draws_;
    std::map<std::string, std::string> scriptSources_; // resolved path -> source (loaded once)
    std::map<std::string, bool> scriptFailed_;         // resolved path -> failed (skip later)
    GameRuntimeConfig cfg_;
    bool running_ = false;
    float simTime_ = 0.f;
};

} // namespace neon::scene
