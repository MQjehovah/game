// ScriptRuntime implementation. Migrated from GameRuntime's AttachScripts /
// AttachOneScript / CallEntityFunctionHandle / HasScriptFunction /
// CallScriptFunction plus the per-frame on_update dispatch in Tick (Task 13).
// Pure code movement, no semantic change: scriptCtx_ (ScriptContext) and the
// Lua/JS hosts stay on GameRuntime as shared state (scripts + behavior trees +
// bindings all share them); the script instances, the load-dedup state and the
// Godot-style input action map moved here.
#include "neon/scene/systems/script_runtime.hpp"

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/log.hpp"
#include "neon/core/pack.hpp"

namespace neon::scene {
namespace {

// Entity handle as a Lua table {id, gen} (matches the T2.3 bindings' shape and
// the EntityFromValue parser in bindings.cpp).
script::Value EntityToValue(const ecs::Entity& e) {
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", script::Value::Num(static_cast<double>(e.generation)));
    return t;
}

} // namespace

void ScriptRuntime::Configure(Content content) {
    scriptBaseDir_ = std::move(content.scriptBaseDir);
    readScript_ = std::move(content.readScript);
}

void ScriptRuntime::AttachAll(const std::vector<std::pair<ecs::Entity, SceneScript>>& scripts,
                              script::ScriptContext& ctx, Hosts hosts) {
    for (const auto& pair : scripts) AttachOne(pair.first, pair.second, ctx, hosts);
}

bool ScriptRuntime::AttachOne(ecs::Entity ent, const SceneScript& s,
                              script::ScriptContext& ctx, Hosts hosts) {
    if (!hosts.lua) return false;
    // The component's `backend` field picks the language ("lua" / "js");
    // empty means the schema default (lua). An unknown backend falls back to
    // the Lua host (ScriptHosts::Get semantics), matching the original.
    const std::string backend = s.backend.empty() ? "lua" : s.backend;
    script::IScriptHost* host = backend == "js" ? hosts.js : hosts.lua;
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
        std::string source = readScript_(full);
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
        CallEntity(ctx, inst, inst.onStart, "on_start", {EntityToValue(ent)});
    }
    return true;
}

void ScriptRuntime::CallEntity(script::ScriptContext& ctx, ScriptInst& inst, uint64_t handle,
                               const char* fn, const std::vector<script::Value>& args) {
    if (!inst.host || handle == 0) return;
    // The input bindings resolve per-entity input through the entity being
    // updated (multi-player: each player's script reads its OWN client input).
    ctx.currentEntity = inst.ent;
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
    ctx.currentEntity = {};
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

bool ScriptRuntime::HasFunction(Hosts hosts, const std::string& name) const {
    return (hosts.lua && hosts.lua->HasFunction(name)) ||
           (hosts.js && hosts.js->HasFunction(name));
}

bool ScriptRuntime::CallFunction(script::ScriptContext& ctx, Hosts hosts,
                                 const std::string& name,
                                 const std::vector<script::Value>& args) const {
    // Deterministic lookup order: Lua first, then JS (an on_player_join etc.
    // defined by both backends resolves to the Lua one).
    script::IScriptHost* host = nullptr;
    if (hosts.lua && hosts.lua->HasFunction(name))
        host = hosts.lua;
    else if (hosts.js && hosts.js->HasFunction(name))
        host = hosts.js;
    if (!host) return false;
    ctx.currentEntity = {};
    const core::Result<script::Value> res = host->Call(name, args);
    if (!res.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                     "runtime: script function %s() failed: %s", name.c_str(),
                     host->LastError().message.c_str());
    }
    return res.Ok();
}

void ScriptRuntime::Tick(float dt, ecs::World& world, script::ScriptContext& ctx) {
    // Index-based: SpawnPrefab (called from a script) can push new
    // ScriptInst entries into scripts_ mid-loop; an iterator would be
    // invalidated. New instances are processed in the same tick, which is
    // the expected "spawned this frame acts this frame" semantics.
    for (size_t si = 0; si < scripts_.size(); ++si) {
        ScriptInst& inst = scripts_[si];
        if (!world.Alive(inst.ent)) continue;
        if (inst.onUpdate == 0) continue; // this chunk defines no on_update
        CallEntity(ctx, inst, inst.onUpdate, "on_update",
                   {EntityToValue(inst.ent), script::Value::Num(dt)});
    }
}

void ScriptRuntime::Clear() {
    scripts_.clear();
    loadedScripts_.clear();
    scriptFailed_.clear();
    chunkHandlers_.clear(); // handles die with the hosts (GameRuntime owns them)
}

std::string ScriptRuntime::FullScriptPath(const std::string& path) const {
    if (path.empty() || scriptBaseDir_.empty()) return path;
    return scriptBaseDir_ + "/" + path;
}

} // namespace neon::scene
