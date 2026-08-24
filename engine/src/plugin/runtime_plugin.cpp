#include "neon/plugin/runtime_plugin.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "neon/core/log.hpp"
#include "neon/script/bindings.hpp"

namespace neon::plugin {
namespace {

std::string DefaultReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Active-plugin context: native Plugin.* bindings need to know WHICH plugin is
// executing (for scoped GetVar/SetVar and registration). The manager sets this
// before every captured call and clears it after.
constexpr const char* kActivePluginKey = "neon_active_plugin";

struct LoadedPlugin {
    PluginManifest manifest;
    script::IScriptHost* host = nullptr;
    uint64_t onStart = 0;  // captured on_start (0 = none)
    std::vector<std::pair<std::string, uint64_t>> handlers; // event -> fn
    std::vector<std::pair<std::string, uint64_t>> commands; // name -> fn
    std::vector<std::pair<std::string, uint64_t>> exports;  // api name -> fn
    bool started = false;
};

std::string ActivePlugin(script::IScriptHost& host) {
    const auto v = host.GetGlobal(kActivePluginKey);
    return v.Ok() && v.Value().type == script::Value::Type::String ? v.Value().str
                                                                   : std::string();
}

void SetActivePlugin(script::IScriptHost& host, const std::string& id) {
    host.SetGlobal(kActivePluginKey, script::Value::Str(id));
}

// --- Plugin API native functions (host-agnostic; registered on both backends)

script::Value NativeInfo(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    if (!mgr) return script::Value::Nil();
    const std::string id = ActivePlugin(host);
    for (const PluginManifest& m : mgr->Manifests()) {
        if (m.id == id) {
            script::Value t = script::Value::Tbl();
            t.table->fields.emplace_back("id", script::Value::Str(m.id));
            t.table->fields.emplace_back("name", script::Value::Str(m.name));
            t.table->fields.emplace_back("version", script::Value::Str(m.version));
            t.table->fields.emplace_back("type", script::Value::Str(PluginTypeName(m.type)));
            t.table->fields.emplace_back("backend", script::Value::Str(m.backend));
            return t;
        }
    }
    return script::Value::Nil();
}

script::Value NativeLog(script::IScriptHost& host, void* /*user*/) {
    const std::string msg = host.GetArg(1).type == script::Value::Type::String
                                ? host.GetArg(1).str
                                : "plugin log";
    const std::string level = host.GetArg(0).type == script::Value::Type::String
                                  ? host.GetArg(0).str
                                  : "info";
    core::LogLevel lv = core::LogLevel::Info;
    if (level == "warn" || level == "warning") lv = core::LogLevel::Warn;
    if (level == "error") lv = core::LogLevel::Error;
    if (level == "debug") lv = core::LogLevel::Debug;
    core::Log(lv, core::LogCategory::Script, nullptr, 0, "plugin[%s]: %s",
              ActivePlugin(host).c_str(), msg.c_str());
    return script::Value::Nil();
}

script::Value NativeOn(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string event = host.GetArg(0).type == script::Value::Type::String
                                  ? host.GetArg(0).str
                                  : "";
    if (event.empty()) {
        host.SetError("Plugin.On: event name must be a non-empty string");
        return script::Value::Nil();
    }
    const auto cap = host.CaptureStackFunction(1);
    if (!cap.Ok()) {
        host.SetError("Plugin.On: second argument must be a function");
        return script::Value::Nil();
    }
    if (mgr) mgr->OnPluginHandler(ActivePlugin(host), event, cap.Value());
    return script::Value::Nil();
}

script::Value NativeOnCommand(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string name = host.GetArg(0).type == script::Value::Type::String
                                 ? host.GetArg(0).str
                                 : "";
    if (name.empty()) {
        host.SetError("Plugin.OnCommand: command name must be a non-empty string");
        return script::Value::Nil();
    }
    const auto cap = host.CaptureStackFunction(1);
    if (!cap.Ok()) {
        host.SetError("Plugin.OnCommand: second argument must be a function");
        return script::Value::Nil();
    }
    if (mgr) mgr->OnPluginCommand(ActivePlugin(host), name, cap.Value());
    return script::Value::Nil();
}

script::Value NativeGetVar(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string key = host.GetArg(0).type == script::Value::Type::String
                                ? host.GetArg(0).str
                                : "";
    if (!mgr || key.empty()) return script::Value::Nil();
    return mgr->GetPluginVar(ActivePlugin(host), key);
}

script::Value NativeSetVar(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string key = host.GetArg(0).type == script::Value::Type::String
                                ? host.GetArg(0).str
                                : "";
    if (mgr && !key.empty()) mgr->SetPluginVar(ActivePlugin(host), key, host.GetArg(1));
    return script::Value::Nil();
}

script::Value NativeExport(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string name = host.GetArg(0).type == script::Value::Type::String
                                 ? host.GetArg(0).str
                                 : "";
    const auto cap = host.CaptureStackFunction(1);
    if (name.empty() || !cap.Ok()) {
        host.SetError("Plugin.Export: name (string) and fn required");
        return script::Value::Nil();
    }
    if (mgr) mgr->OnPluginExport(ActivePlugin(host), name, cap.Value());
    return script::Value::Nil();
}

script::Value NativeCall(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string pid = host.GetArg(0).type == script::Value::Type::String
                                ? host.GetArg(0).str
                                : "";
    const std::string name = host.GetArg(1).type == script::Value::Type::String
                                 ? host.GetArg(1).str
                                 : "";
    if (!mgr) return script::Value::Nil();
    std::vector<script::Value> args;
    for (int i = 2; i < host.ArgCount(); ++i) args.push_back(host.GetArg(i));
    return mgr->CallPluginApi(pid, name, args);
}

script::Value NativeRegisterComponent(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<RuntimePluginManager*>(user);
    const std::string name = host.GetArg(0).type == script::Value::Type::String
                                 ? host.GetArg(0).str
                                 : "";
    if (mgr && !name.empty()) mgr->OnRegisterComponent(name, host.GetArg(1));
    return script::Value::Nil();
}

} // namespace

struct RuntimePluginManager::Impl {
    Config cfg;
    RuntimePluginManager* owner = nullptr;
    bool started = false;
    double simTime = 0.0;
    std::unique_ptr<script::IScriptHost> luaHost;
    std::unique_ptr<script::IScriptHost> jsHost;
    std::vector<LoadedPlugin> plugins;
    std::vector<PluginManifest> manifests;
    std::map<std::string, script::Value> componentSchemas;

    script::IScriptHost* HostFor(const std::string& backend) {
        if (backend == "js") {
            if (!jsHost) {
                jsHost = script::CreateJsHost();
                if (jsHost && jsHost->Init()) RegisterPluginApi(*jsHost, owner);
            }
            return jsHost.get();
        }
        if (!luaHost) {
            luaHost = script::CreateLuaHost();
            if (luaHost && luaHost->Init()) RegisterPluginApi(*luaHost, owner);
        }
        return luaHost.get();
    }

    void RegisterPluginApi(script::IScriptHost& host, RuntimePluginManager* mgr) {
        // Engine gameplay bindings first (plugins drive the world like scene
        // scripts), then the Plugin API table.
        if (cfg.ctx) script::RegisterEngineBindings(host, *cfg.ctx);
        host.SetRngSeed(cfg.rngSeed ? cfg.rngSeed : 1u);
        host.SetSimClock(0.0);
        SetupBindings(host, mgr);
    }

    void SetupBindings(script::IScriptHost& host, RuntimePluginManager* mgr) {
        host.RegisterField("Plugin", "Info", &NativeInfo, mgr);
        host.RegisterField("Plugin", "Log", &NativeLog, mgr);
        host.RegisterField("Plugin", "On", &NativeOn, mgr);
        host.RegisterField("Plugin", "OnCommand", &NativeOnCommand, mgr);
        host.RegisterField("Plugin", "GetVar", &NativeGetVar, mgr);
        host.RegisterField("Plugin", "SetVar", &NativeSetVar, mgr);
        host.RegisterField("Plugin", "Export", &NativeExport, mgr);
        host.RegisterField("Plugin", "Call", &NativeCall, mgr);
        host.RegisterField("Plugin", "RegisterComponent", &NativeRegisterComponent, mgr);
    }
};

RuntimePluginManager::RuntimePluginManager() : impl_(std::make_unique<Impl>()) {}

RuntimePluginManager::~RuntimePluginManager() { Stop(); }

bool RuntimePluginManager::Running() const { return impl_->started; }

size_t RuntimePluginManager::Count() const { return impl_->manifests.size(); }

const std::vector<PluginManifest>& RuntimePluginManager::Manifests() const {
    return impl_->manifests;
}

const std::map<std::string, script::Value>& RuntimePluginManager::ComponentSchemas() const {
    return impl_->componentSchemas;
}

size_t RuntimePluginManager::Load(const Config& cfg) {
    impl_->cfg = cfg;
    impl_->owner = this;
    if (!cfg.readFile) impl_->cfg.readFile = &DefaultReadFile;
    impl_->plugins.clear();
    impl_->manifests.clear();
    impl_->componentSchemas.clear();

    std::vector<PluginManifest> all = DiscoverPlugins(cfg.baseDir);
    // Load in dependency order: iterate until every loadable plugin is done.
    std::vector<bool> done(all.size(), false);
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < all.size(); ++i) {
            if (done[i]) continue;
            if (all[i].type != PluginType::Runtime) {
                done[i] = true;
                progress = true;
                continue;
            }
            bool depsOk = true;
            for (const std::string& req : all[i].requires) {
                bool found = false;
                for (size_t j = 0; j < all.size(); ++j)
                    if (all[j].id == req) found = true;
                if (!found) {
                    depsOk = false;
                    break;
                }
            }
            if (!depsOk) continue;
            // Engine version gate.
            if (!all[i].minEngineVersion.empty()) {
                Version minV, engV;
                if (ParseVersion(all[i].minEngineVersion, &minV) &&
                    ParseVersion(kEngineVersion, &engV)) {
                    const bool atLeast =
                        engV.major > minV.major ||
                        (engV.major == minV.major &&
                         (engV.minor > minV.minor ||
                          (engV.minor == minV.minor && engV.patch >= minV.patch)));
                    if (!atLeast) {
                        NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                                     "plugin '%s' needs engine >= %s (have %s); skipped",
                                     all[i].id.c_str(), all[i].minEngineVersion.c_str(),
                                     kEngineVersion);
                        done[i] = true;
                        progress = true;
                        continue;
                    }
                }
            }
            script::IScriptHost* host = impl_->HostFor(all[i].backend);
            if (!host) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                             "plugin '%s': script host for '%s' unavailable; skipped",
                             all[i].id.c_str(), all[i].backend.c_str());
                done[i] = true;
                progress = true;
                continue;
            }
            const std::string full = all[i].dir + "/" + all[i].entry;
            const std::string source = impl_->cfg.readFile(full);
            if (source.empty()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                             "plugin '%s': cannot read '%s'; skipped", all[i].id.c_str(),
                             full.c_str());
                done[i] = true;
                progress = true;
                continue;
            }
            if (!host->Load(source) || !host->Run().Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                             "plugin '%s': load failed: %s; skipped", all[i].id.c_str(),
                             host->LastError().message.c_str());
                done[i] = true;
                progress = true;
                continue;
            }
            LoadedPlugin lp;
            lp.manifest = all[i];
            lp.host = host;
            // Push BEFORE on_load so Plugin.On/OnCommand/Export registered
            // during the entry script land on this plugin's record.
            impl_->plugins.push_back(std::move(lp));
            LoadedPlugin& loaded = impl_->plugins.back();
            if (const auto h = host->CaptureFunction("on_load"); h.Ok()) {
                SetActivePlugin(*host, all[i].id);
                const auto res = host->CallCaptured(h.Value(), {});
                if (!res.Ok()) {
                    NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                                 "plugin '%s': on_load failed: %s", all[i].id.c_str(),
                                 host->LastError().message.c_str());
                    impl_->plugins.pop_back();
                    done[i] = true;
                    progress = true;
                    continue;
                }
            }
            if (const auto h = host->CaptureFunction("on_start"); h.Ok())
                loaded.onStart = h.Value();
            impl_->manifests.push_back(all[i]);
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Info,
                         "plugin '%s' v%s (%s/%s) loaded", all[i].id.c_str(),
                         all[i].version.c_str(), all[i].backend.c_str(),
                         PluginTypeName(all[i].type));
            done[i] = true;
            progress = true;
        }
    }
    return impl_->plugins.size();
}

void RuntimePluginManager::Start() {
    if (impl_->started) return;
    impl_->started = true;
    for (LoadedPlugin& p : impl_->plugins) {
        if (p.onStart == 0) continue;
        SetActivePlugin(*p.host, p.manifest.id);
        const auto res = p.host->CallCaptured(p.onStart, {});
        if (!res.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                         "plugin '%s': on_start failed: %s", p.manifest.id.c_str(),
                         p.host->LastError().message.c_str());
        }
        p.started = true;
    }
}

void RuntimePluginManager::Tick(float dt) {
    if (!impl_->started) return;
    impl_->simTime += dt;
    if (impl_->luaHost) impl_->luaHost->SetSimClock(impl_->simTime);
    if (impl_->jsHost) impl_->jsHost->SetSimClock(impl_->simTime);
    for (LoadedPlugin& p : impl_->plugins) {
        for (const auto& [event, handle] : p.handlers) {
            if (event != "tick") continue;
            SetActivePlugin(*p.host, p.manifest.id);
            const auto res = p.host->CallCaptured(handle, {script::Value::Num(dt)});
            if (!res.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                             "plugin '%s' tick handler failed: %s", p.manifest.id.c_str(),
                             p.host->LastError().message.c_str());
            }
        }
    }
}

void RuntimePluginManager::SetSimTime(double seconds) { impl_->simTime = seconds; }

void RuntimePluginManager::Stop() {
    if (impl_->started) {
        impl_->started = false;
        DispatchEvent("stop", {});
    }
    if (impl_->luaHost) {
        impl_->luaHost->Shutdown();
        impl_->luaHost.reset();
    }
    if (impl_->jsHost) {
        impl_->jsHost->Shutdown();
        impl_->jsHost.reset();
    }
    impl_->plugins.clear();
    impl_->manifests.clear();
}

void RuntimePluginManager::DispatchEvent(const std::string& name,
                                         const std::vector<script::Value>& args) {
    for (LoadedPlugin& p : impl_->plugins) {
        for (const auto& [event, handle] : p.handlers) {
            if (event != name) continue;
            SetActivePlugin(*p.host, p.manifest.id);
            const auto res = p.host->CallCaptured(handle, args);
            if (!res.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Warn,
                             "plugin '%s' event '%s' handler failed: %s",
                             p.manifest.id.c_str(), name.c_str(),
                             p.host->LastError().message.c_str());
            }
        }
    }
}

bool RuntimePluginManager::RunCommand(const std::string& name,
                                      const std::vector<script::Value>& args,
                                      std::string* error) {
    for (LoadedPlugin& p : impl_->plugins) {
        for (const auto& [cmd, handle] : p.commands) {
            if (cmd != name) continue;
            SetActivePlugin(*p.host, p.manifest.id);
            const auto res = p.host->CallCaptured(handle, args);
            if (!res.Ok()) {
                if (error) *error = p.host->LastError().message;
                return false;
            }
            return true;
        }
    }
    if (error) *error = "unknown command '" + name + "'";
    return false;
}

void RuntimePluginManager::OnPluginHandler(const std::string& pluginId,
                                           const std::string& event, uint64_t handle) {
    for (LoadedPlugin& p : impl_->plugins) {
        if (p.manifest.id == pluginId) {
            p.handlers.emplace_back(event, handle);
            return;
        }
    }
}

void RuntimePluginManager::OnPluginCommand(const std::string& pluginId,
                                           const std::string& name, uint64_t handle) {
    for (LoadedPlugin& p : impl_->plugins) {
        if (p.manifest.id == pluginId) {
            p.commands.emplace_back(name, handle);
            return;
        }
    }
}

void RuntimePluginManager::OnPluginExport(const std::string& pluginId,
                                          const std::string& name, uint64_t handle) {
    for (LoadedPlugin& p : impl_->plugins) {
        if (p.manifest.id == pluginId) {
            p.exports.emplace_back(name, handle);
            return;
        }
    }
}

script::Value RuntimePluginManager::CallPluginApi(const std::string& pluginId,
                                                  const std::string& name,
                                                  const std::vector<script::Value>& args) {
    for (LoadedPlugin& p : impl_->plugins) {
        if (p.manifest.id != pluginId) continue;
        for (const auto& [apiName, handle] : p.exports) {
            if (apiName != name) continue;
            SetActivePlugin(*p.host, pluginId);
            const auto res = p.host->CallCaptured(handle, args);
            return res.Ok() ? res.Value() : script::Value::Nil();
        }
    }
    return script::Value::Nil();
}

void RuntimePluginManager::OnRegisterComponent(const std::string& name,
                                               const script::Value& schema) {
    impl_->componentSchemas[name] = schema;
}

script::Value RuntimePluginManager::GetPluginVar(const std::string& pluginId,
                                                 const std::string& key) {
    if (!impl_->cfg.gameVars) return script::Value::Nil();
    return impl_->cfg.gameVars->Get("plugin:" + pluginId + ":" + key);
}

void RuntimePluginManager::SetPluginVar(const std::string& pluginId, const std::string& key,
                                        const script::Value& v) {
    if (!impl_->cfg.gameVars) return;
    impl_->cfg.gameVars->Set("plugin:" + pluginId + ":" + key, v);
}

} // namespace neon::plugin
