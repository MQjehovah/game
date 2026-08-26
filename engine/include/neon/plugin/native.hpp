#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neon/plugin/plugin.hpp"

namespace neon::plugin {

// ---------------------------------------------------------------------------
// Native binary plugins (G4-1): DLL/SO modules with a stable C ABI, loaded at
// runtime. This extends the existing script-plugin system: a native plugin is
// just a PluginType::Native manifest whose entry is a shared library instead of
// a Lua/JS file. The ABI below is deliberately POD + extern "C" functions so no
// C++ runtime objects (vtable layout, std::string, exceptions) cross the
// boundary — the host and the module only exchange an opaque instance handle.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kNativeApiVersion = 1;

// Versioned descriptor a module fills via the exported
// `bool NeonPlugin_GetInfo(NativePluginInfo*)` symbol.
struct NativePluginInfo {
    uint32_t apiVersion = 0;   // must equal kNativeApiVersion
    uint32_t size = 0;         // must equal sizeof(NativePluginInfo)
    const char* name = nullptr;   // display name
    const char* version = nullptr;
    // Lifecycle: create() returns an opaque instance, destroy() frees it. The
    // host only ever calls these two on the opaque handle; everything else is
    // module-specific (a module exports its own C functions taking the handle).
    void* (*create)(uint32_t apiVersion) = nullptr;
    void (*destroy)(void* instance) = nullptr;
};

// A loaded native module. Owns the OS shared-library handle and the plugin
// instance; destroying it calls the plugin's destroy then frees the library.
// Hot-reload = Reload() (destroy + reload the same path).
class NativePlugin {
public:
    // Loads `libPath`, resolves NeonPlugin_GetInfo and validates the ABI
    // (apiVersion + size + required callbacks). Returns null (with *err set)
    // on any failure.
    static std::unique_ptr<NativePlugin> Load(const std::string& libPath,
                                              std::string* err);
    ~NativePlugin();

    NativePlugin(NativePlugin&&) noexcept;
    NativePlugin& operator=(NativePlugin&&) noexcept;
    NativePlugin(const NativePlugin&) = delete;
    NativePlugin& operator=(const NativePlugin&) = delete;

    const NativePluginInfo& Info() const { return info_; }
    const std::string& Path() const { return path_; }
    // The opaque plugin instance returned by Info().create().
    void* Instance() const { return instance_; }

    // Resolves a module-specific C symbol (e.g. a module's API getter). Returns
    // null when the library does not export `name`.
    void* Symbol(const char* name) const;

    // Destroys + frees the library and loads `libPath` again. Returns false on
    // reload failure (the plugin is left unloaded).
    bool Reload(const std::string& libPath, std::string* err);

private:
    NativePlugin() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NativePluginInfo info_{};
    void* instance_ = nullptr;
    std::string path_;
};

// Discovers native plugins under <baseDir>/plugins (PluginType::Native
// manifests, via DiscoverPlugins) and loads each one's shared library. The
// library file is <plugin.dir>/<manifest.entry>. Individual failures are
// logged, never fatal — the vector holds only successfully loaded plugins.
// Returns the loaded plugins (ordered by manifest id).
std::vector<std::unique_ptr<NativePlugin>> LoadNativePlugins(const std::string& baseDir);

// Resolves a native plugin's shared library path from its manifest: the entry
// name is used verbatim on Windows; POSIX appends ".so" when the entry has no
// extension. Returns "" when entry is empty.
std::string NativeLibraryPath(const PluginManifest& m);

} // namespace neon::plugin
