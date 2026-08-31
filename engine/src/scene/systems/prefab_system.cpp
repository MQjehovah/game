// PrefabSystem implementation. Migrated from GameRuntime::LoadPrefabs /
// SpawnPrefab (C1): owns the prefab component-template library and turns a
// prefab name into a single-entity SceneFile (prefab reference + transform
// override). The world-level expansion (component factories + script attach)
// stays in GameRuntime, injected as the instantiate callback so this class
// never reaches back into runtime internals. Pure code movement, no semantic
// change.
#include "neon/scene/systems/prefab_system.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/log.hpp"

namespace neon::scene {
namespace {

// Case-insensitive suffix match ("main.JSON" counts as a .json prefab) --
// inline copy of the runtime's detail::HasSuffix (kept local per TU pattern).
bool HasSuffix(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
            s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

// "a/b/c.json" -> "c" (the prefab registration name).
std::string FileStem(const std::string& p) {
    const size_t slash = p.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

} // namespace

void PrefabSystem::Load(const std::string& scriptBaseDir, const ListFilesFn& listFiles,
                        const ReadFileFn& readFile) {
    prefs_ = PrefabLibrary{};
    if (!listFiles) return;
    const std::vector<std::string> files = listFiles(scriptBaseDir + "/assets/prefabs");
    size_t loaded = 0;
    for (const std::string& path : files) {
        if (!HasSuffix(path, ".json")) continue;
        const std::string name = FileStem(path);
        if (name.empty()) continue;
        const std::string text = readFile ? readFile(path) : std::string{};
        if (text.empty()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: prefab '%s' cannot be read (skipped)", path.c_str());
            continue;
        }
        core::Status st = prefs_.Add(name, text);
        if (!st.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: prefab '%s' failed to load: %s (skipped)", path.c_str(),
                         st.Error().c_str());
            continue;
        }
        ++loaded;
    }
    if (!files.empty()) {
        NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Debug,
                     "runtime: loaded %zu prefabs", loaded);
    }
}

ecs::Entity PrefabSystem::Spawn(const std::string& name, const math::Vec3& pos) {
    if (name.empty() || !prefs_.Get(name).Ok()) {
        NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                     "runtime: SpawnPrefab: unknown prefab '%s'", name.c_str());
        return {};
    }
    if (!instantiate_) return {};
    // Build a one-entity scene that references the prefab and overrides the
    // transform; the injected callback runs the exact Instantiate pipeline
    // (prefab expansion, component factories, custom-component SceneData
    // storage) against the runtime world + component registry.
    static uint64_t spawnCounter = 1;
    const std::string uniqueName = name + "_" + std::to_string(spawnCounter++);
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    core::Json ent;
    ent.type_ = core::Json::Type::Object;
    core::Json nameJ;
    nameJ.type_ = core::Json::Type::String;
    nameJ.string_ = uniqueName;
    ent.object_["name"] = std::move(nameJ);
    core::Json prefabJ;
    prefabJ.type_ = core::Json::Type::String;
    prefabJ.string_ = name;
    ent.object_["prefab"] = std::move(prefabJ);
    core::Json comps;
    comps.type_ = core::Json::Type::Object;
    core::Json tr;
    tr.type_ = core::Json::Type::Object;
    core::Json posArr;
    posArr.type_ = core::Json::Type::Array;
    for (float v : {pos.x, pos.y, pos.z}) {
        core::Json num;
        num.type_ = core::Json::Type::Number;
        num.number_ = v;
        posArr.array_.push_back(std::move(num));
    }
    tr.object_["pos"] = std::move(posArr);
    comps.object_["transform"] = std::move(tr);
    ent.object_["components"] = std::move(comps);
    arr.array_.push_back(std::move(ent));
    root.object_["entities"] = std::move(arr);

    auto parsed = SceneFile::Parse(core::JsonWriter::Write(root));
    if (!parsed.Ok()) return {};
    return instantiate_(parsed.Value());
}

} // namespace neon::scene
