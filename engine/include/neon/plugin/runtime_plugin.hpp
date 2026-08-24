#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "neon/plugin/plugin.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"

namespace neon::plugin {

// Loads and runs runtime plugins (gameplay/system modules written in Lua or
// JS) against the engine's dual script backends.
//
// Each plugin gets an isolated script chunk (its own host, shared with other
// plugins of the same backend) with the full engine binding surface PLUS a
// `Plugin` API:
//   Plugin.Info()                 -> {id,name,version,type,backend}
//   Plugin.Log(level, msg)
//   Plugin.On("tick"|"stop"|any, fn)      event subscription
//   Plugin.OnCommand(name, fn, help)      command registry
//   Plugin.GetVar(key) / Plugin.SetVar(key, value)  scoped plugin:<id>:<key>
//   Plugin.Export(name, fn)               publish a module API
//   Plugin.Call(pluginId, name, ...args)  call another plugin's exported API
//   Plugin.RegisterComponent(name, schema) editor schema registration
//
// Plugin state is scoped per plugin id (GetVar/SetVar prefix), so parallel
// gameplay modules cannot collide. Determinism matches scene scripts: the
// hosts share the engine's seeded RNG and the injected simulated clock.
class RuntimePluginManager {
public:
    using ReadFile = std::function<std::string(const std::string&)>;

    struct Config {
        std::string baseDir; // project root; plugins live in <baseDir>/plugins
        ReadFile readFile;   // file reader (disk default; packs via override)
        script::ScriptContext* ctx = nullptr; // engine bindings context
        script::GameVars* gameVars = nullptr; // scoped plugin state store
        uint64_t rngSeed = 1;
    };

    RuntimePluginManager();
    ~RuntimePluginManager();
    RuntimePluginManager(const RuntimePluginManager&) = delete;
    RuntimePluginManager& operator=(const RuntimePluginManager&) = delete;

    // Discovers + loads every runtime plugin under <baseDir>/plugins. Returns
    // the number loaded; individual failures are logged, never fatal.
    size_t Load(const Config& cfg);

    void Start();            // on_load already ran; calls on_start per plugin
    void Tick(float dt);     // calls on_tick handlers; updates the sim clock
    void Stop();             // calls on_stop handlers and tears the hosts down
    bool Running() const;

    void SetSimTime(double seconds); // injected clock (NMath.Time / Math-based)

    // Dispatches a named event to every subscribed handler (args passed
    // through). Unknown events are a no-op.
    void DispatchEvent(const std::string& name,
                       const std::vector<script::Value>& args = {});

    // Runs a registered command. Returns false when the command is unknown or
    // its handler raised (message in *error when provided).
    bool RunCommand(const std::string& name, const std::vector<script::Value>& args,
                    std::string* error = nullptr);

    size_t Count() const;
    const std::vector<PluginManifest>& Manifests() const;

    // Schemas registered via Plugin.RegisterComponent (name -> Value table).
    const std::map<std::string, script::Value>& ComponentSchemas() const;

    // Called by the Plugin.* native bindings (public so the host-agnostic
    // registration closures can reach the manager).
    void OnPluginHandler(const std::string& pluginId, const std::string& event,
                         uint64_t handle);
    void OnPluginCommand(const std::string& pluginId, const std::string& name,
                         uint64_t handle);
    void OnPluginExport(const std::string& pluginId, const std::string& name,
                        uint64_t handle);
    void OnRegisterComponent(const std::string& name, const script::Value& schema);
    script::Value GetPluginVar(const std::string& pluginId, const std::string& key);
    void SetPluginVar(const std::string& pluginId, const std::string& key,
                      const script::Value& v);
    script::Value CallPluginApi(const std::string& pluginId, const std::string& name,
                                const std::vector<script::Value>& args);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::plugin
