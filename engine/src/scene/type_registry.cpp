#include "neon/scene/type_registry.hpp"

#include <utility>

#include "neon/scene/scene_file.hpp"

namespace neon::scene {

namespace {
inline void EnsureNoDuplicate(std::vector<std::pair<std::string, TypeInfo>>& v,
                              const std::string& name) {
    for (auto& [n, info] : v)
        if (n == name) {
            info = TypeInfo{};
            return;
        }
}
} // namespace

std::vector<std::pair<std::string, TypeInfo>>& TypeRegistry::Mutate() {
    static std::vector<std::pair<std::string, TypeInfo>> g_registry;
    return g_registry;
}

const TypeInfo* TypeRegistry::Find(const std::string& name) {
    for (const auto& [n, info] : Mutate())
        if (n == name) return &info;
    return nullptr;
}

std::vector<TypeInfo> TypeRegistry::All() {
    std::vector<TypeInfo> out;
    out.reserve(Mutate().size());
    for (auto& [n, info] : Mutate()) out.push_back(info);
    return out;
}

bool TypeRegistry::Has(const std::string& name) { return Find(name) != nullptr; }

void TypeRegistry::Clear() { Mutate().clear(); }

void RegisterBuiltinReflectedTypes() {
    // Symmetric re-registration is a no-op (last wins); the call is idempotent
    // so tests can Clear() and re-register.
    TypeRegistry::Register<SceneHealth>("health", "生命");
    TypeRegistry::Register<SceneAudioSource>("audio", "音频源");
}

} // namespace neon::scene
