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
    const std::string want = ToLower(backendName);
    const bool any = want.empty() || want == "*";

    for (std::unique_ptr<NativePlugin>& p : LoadNativePlugins(baseDir)) {
        void* sym = p->Symbol("NeonPhysics_GetWorldApi");
        if (!sym) continue; // not a physics provider
        auto getter = reinterpret_cast<const PhysicsWorldApi* (*)()>(sym);
        if (!getter) continue;
        const PhysicsWorldApi* api = nullptr;
        try {
            api = getter();
        } catch (...) {
            NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                         "backend: physics api getter raised in '%s'", p->Path().c_str());
            continue;
        }
        if (!api || !api->create_world || !api->destroy_world) {
            NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn,
                         "backend: plugin '%s' exports an incomplete physics factory",
                         p->Path().c_str());
            continue;
        }
        const std::string reported =
            api->name ? ToLower(api->name()) : std::string{};
        const std::string libStem = PathStem(p->Path());
        const bool matches =
            any || (!reported.empty() && reported.find(want) != std::string::npos) ||
            libStem.find(want) != std::string::npos;
        if (!matches) continue;

        auto backend = std::make_unique<PhysicsBackend>();
        backend->plugin = std::move(p);
        backend->api = *api;
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                     "backend: physics provider '%s' loaded from '%s'",
                     backend->Name().c_str(), libStem.c_str());
        return backend;
    }
    return nullptr;
}

} // namespace neon::plugin
