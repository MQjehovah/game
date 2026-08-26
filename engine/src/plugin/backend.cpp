#include "neon/plugin/backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "neon/core/log.hpp"

namespace neon::plugin {
namespace {

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Library filename stem of a plugin path, lower-cased ("neon_plugin_physics.dll" -> "neon_plugin_physics").
std::string PathStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t begin = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    size_t len = (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
    return ToLower(path.substr(begin, len));
}

// Shared kind-scoped provider discovery (G5-1): scans the native plugins under
// <baseDir>/plugins, keeps the first one that exports `getterSymbol`, resolves
// its C API table and matches the requested backend name. `ApiT` is a POD C
// function-table struct (PhysicsWorldApi / AudioApi).
template <typename ApiT>
struct FoundProvider {
    std::unique_ptr<NativePlugin> plugin;
    ApiT api{};
    bool ok = false;
};

template <typename ApiT>
FoundProvider<ApiT> FindProvider(const std::string& backendName, const std::string& baseDir,
                                 const char* getterSymbol) {
    const std::string want = ToLower(backendName);
    const bool any = want.empty() || want == "*";

    for (std::unique_ptr<NativePlugin>& p : LoadNativePlugins(baseDir)) {
        void* sym = p->Symbol(getterSymbol);
        if (!sym) continue; // not a provider of this kind
        using GetterFn = const ApiT* (*)();
        auto getter = reinterpret_cast<GetterFn>(sym);
        if (!getter) continue;
        const ApiT* api = nullptr;
        try {
            api = getter();
        } catch (...) {
            NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                         "backend: %s getter raised in '%s'", getterSymbol, p->Path().c_str());
            continue;
        }
        if (!api) {
            NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                         "backend: plugin '%s' returned a null %s table", p->Path().c_str(),
                         getterSymbol);
            continue;
        }
        // Required callbacks vary per kind; the kind-specific loader checks the
        // ones it needs, so we only gate on the table existing here.
        const std::string reported =
            api->name ? ToLower(api->name()) : std::string{};
        const std::string libStem = PathStem(p->Path());
        const bool matches =
            any || (!reported.empty() && reported.find(want) != std::string::npos) ||
            libStem.find(want) != std::string::npos;
        if (!matches) continue;

        FoundProvider<ApiT> out;
        out.plugin = std::move(p);
        out.api = *api;
        out.ok = true;
        return out;
    }
    return {};
}

} // namespace

std::unique_ptr<physics::World, std::function<void(physics::World*)>>
PhysicsBackend::CreateWorld() const {
    auto del = [this](physics::World* w) {
        if (w && api.destroy_world) api.destroy_world(w);
    };
    if (!api.create_world) return {nullptr, del};
    void* raw = nullptr;
    try {
        raw = api.create_world();
    } catch (...) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Error,
                     "backend: physics create_world raised; disabled");
        return {nullptr, del};
    }
    if (!raw) return {nullptr, del};
    return {static_cast<physics::World*>(raw), std::move(del)};
}

std::unique_ptr<PhysicsBackend> LoadNativePhysicsBackend(const std::string& backendName,
                                                         const std::string& baseDir) {
    FoundProvider<PhysicsWorldApi> found =
        FindProvider<PhysicsWorldApi>(backendName, baseDir, "NeonPhysics_GetWorldApi");
    if (!found.ok) return nullptr;
    if (!found.api.create_world || !found.api.destroy_world) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                     "backend: plugin '%s' exports an incomplete physics factory",
                     found.plugin->Path().c_str());
        return nullptr;
    }
    auto backend = std::make_unique<PhysicsBackend>();
    backend->plugin = std::move(found.plugin);
    backend->api = found.api;
    NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                 "backend: physics provider '%s' loaded from '%s'",
                 backend->Name().c_str(), PathStem(backend->plugin->Path()).c_str());
    return backend;
}

std::unique_ptr<neon::audio::IAudioBackend,
                std::function<void(neon::audio::IAudioBackend*)>>
AudioBackend::CreateBackend() const {
    auto del = [this](neon::audio::IAudioBackend* b) {
        if (b && api.destroy_backend) api.destroy_backend(b);
    };
    if (!api.create_backend) return {nullptr, del};
    void* raw = nullptr;
    try {
        raw = api.create_backend();
    } catch (...) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Error,
                     "backend: audio create_backend raised; disabled");
        return {nullptr, del};
    }
    if (!raw) return {nullptr, del};
    return {static_cast<neon::audio::IAudioBackend*>(raw), std::move(del)};
}

std::unique_ptr<AudioBackend> LoadNativeAudioBackend(const std::string& backendName,
                                                     const std::string& baseDir) {
    FoundProvider<AudioApi> found = FindProvider<AudioApi>(backendName, baseDir, "NeonAudio_GetApi");
    if (!found.ok) return nullptr;
    if (!found.api.create_backend || !found.api.destroy_backend) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                     "backend: plugin '%s' exports an incomplete audio factory",
                     found.plugin->Path().c_str());
        return nullptr;
    }
    auto backend = std::make_unique<AudioBackend>();
    backend->plugin = std::move(found.plugin);
    backend->api = found.api;
    NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                 "backend: audio provider '%s' loaded from '%s'",
                 backend->Name().c_str(), PathStem(backend->plugin->Path()).c_str());
    return backend;
}

} // namespace neon::plugin
