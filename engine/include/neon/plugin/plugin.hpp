#pragma once

#include <string>
#include <vector>

#include "neon/core/json.hpp"

namespace neon::plugin {

// The engine version plugins are validated against (matches CMake project
// VERSION 0.1.0). minEngineVersion in a plugin.json is compared with this.
inline constexpr const char* kEngineVersion = "0.1.0";

// A plugin's extension surface. Runtime = gameplay/system modules (server +
// client, Lua/JS); Editor = editor tooling (panels/generators/asset sources);
// Native = future C++ DLL plugins (reserved; not loadable yet).
enum class PluginType { Runtime, Editor, Native };
const char* PluginTypeName(PluginType t);
PluginType PluginTypeFromName(const std::string& s);

// "1.2.3" -> {1,2,3}; returns false on malformed input. Trailing zero groups
// are allowed ("1.2" == "1.2.0").
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};
bool ParseVersion(const std::string& s, Version* out);

// A parsed plugin.json. `dir` is the plugin's directory (set by discovery),
// `enabled` is management state persisted by the host (not part of the file).
struct PluginManifest {
    std::string id;                  // unique id, e.g. "tree_gen"
    std::string name;                // display name, e.g. "树木生成器"
    std::string version;             // "1.0.0"
    PluginType type = PluginType::Runtime;
    std::string backend;             // "lua" | "js"
    std::string entry;               // "init.lua"
    std::string minEngineVersion;    // "0.1.0" ("" = any)
    std::vector<std::string> requires;    // plugin ids this depends on
    std::vector<std::string> permissions; // "world" | "commands" | "net" | ...
    std::string dir;                 // absolute plugin directory
    bool enabled = true;             // management state (not from plugin.json)

    // Parses a plugin.json object. `err` receives a human-readable message on
    // failure. id/entry/backend are required; type defaults to runtime.
    bool Load(const core::Json& j, std::string* err);
    bool LoadJson(const std::string& text, std::string* err);
};

// Scans <baseDir>/plugins/*/plugin.json. Plugins with unreadable/invalid
// manifests are skipped (the caller can surface a warning via the return of
// a valid neighbor; malformed ones are dropped silently per plugin dir).
std::vector<PluginManifest> DiscoverPlugins(const std::string& baseDir);

} // namespace neon::plugin
