// PluginSystem implementation. Thin lifecycle wrapper over
// plugin::RuntimePluginManager, migrated from GameRuntime's plugins_ member +
// the plugin setup inside Start / the tick dispatch inside Tick / the teardown
// inside Stop and the DispatchPluginEvent / RunPluginCommand forwarders
// (Task 8). Pure code movement: the manager still owns the isolated plugin
// hosts, discovery/ordering and per-plugin on_load/on_start/on_tick/on_stop;
// this class only parameterizes the base dir + file reader (same pattern as
// PrefabSystem) and drives the lifecycle.
#include "neon/scene/systems/plugin_system.hpp"

namespace neon::scene {

void PluginSystem::Load(const std::string& scriptBaseDir, const ReadScriptFn& readScript,
                        script::ScriptContext* ctx, uint64_t rngSeed) {
    manager_ = std::make_unique<plugin::RuntimePluginManager>();
    plugin::RuntimePluginManager::Config pc;
    pc.baseDir = scriptBaseDir.empty() ? "." : scriptBaseDir;
    pc.readFile = readScript;
    pc.ctx = ctx;
    pc.gameVars = ctx ? &ctx->gameVars : nullptr;
    pc.rngSeed = rngSeed ? rngSeed : 1u; // 0 aliases seed 1
    manager_->Load(pc);
    manager_->Start();
}

void PluginSystem::Tick(float dt, double simTime) {
    if (!manager_) return;
    manager_->SetSimTime(simTime);
    manager_->Tick(dt);
}

void PluginSystem::Shutdown() {
    if (!manager_) return;
    manager_->Stop();
    manager_.reset();
}

bool PluginSystem::DispatchEvent(const std::string& name,
                                 const std::vector<script::Value>& args) {
    if (!manager_) return false;
    manager_->DispatchEvent(name, args);
    return true;
}

bool PluginSystem::RunCommand(const std::string& name,
                              const std::vector<script::Value>& args, std::string* error) {
    return manager_ && manager_->RunCommand(name, args, error);
}

} // namespace neon::scene
