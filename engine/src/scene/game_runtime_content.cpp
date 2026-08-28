// C1: GameRuntime content-loading subsystem (prefabs / locales / script &
// asset path resolution). Split out of the former single game_runtime.cpp TU:
// these functions only touch cfg_/prefs_/loc_ and the shared detail helpers.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <fstream>
#include <iterator>

#include "neon/assets/asset_path.hpp"
#include "neon/assets/asset_variants.hpp"
#include "neon/core/log.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {
using namespace detail; // ListFilesRecursive/HasSuffix/FileStem (inline copies)

void GameRuntime::LoadPrefabs() {
    prefs_ = PrefabLibrary{};
    if (cfg_.scriptBaseDir.empty()) return; // disk-less hosts have no prefab tree
    std::vector<std::string> files;
    ListFilesRecursive(cfg_.scriptBaseDir + "/prefabs", "", files);
    size_t loaded = 0;
    for (const std::string& rel : files) {
        if (!HasSuffix(rel, ".json")) continue;
        const std::string name = FileStem(rel);
        if (name.empty()) continue;
        std::string text = ReadScript(FullScriptPath("prefabs/" + rel));
        if (text.empty()) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: prefab '%s' cannot be read (skipped)", rel.c_str());
            continue;
        }
        core::Status st = prefs_.Add(name, text);
        if (!st.Ok()) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: prefab '%s' failed to load: %s (skipped)", rel.c_str(),
                         st.Error().c_str());
            continue;
        }
        ++loaded;
    }
    if (!files.empty()) {
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Debug,
                     "runtime: loaded %zu prefabs", loaded);
    }
}

void GameRuntime::LoadLocales() {
    loc_ = core::Localization();
    if (cfg_.localesDir.empty()) return;
    std::vector<std::string> files;
    ListFilesRecursive(cfg_.localesDir, "", files);
    size_t loaded = 0;
    for (const std::string& rel : files) {
        if (!HasSuffix(rel, ".json")) continue;
        std::string text = ReadScript(FullScriptPath(rel));
        if (text.empty()) continue;
        std::string err;
        if (!loc_.LoadTable(text, &err)) {
            NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                         "runtime: locale '%s' failed to load: %s", rel.c_str(), err.c_str());
            continue;
        }
        ++loaded;
    }
    if (loaded > 0) {
        std::string langs;
        for (const std::string& l : loc_.Languages()) {
            if (!langs.empty()) langs += ",";
            langs += l;
        }
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                     "runtime: loaded %zu locale file(s), languages: %s", loaded,
                     langs.c_str());
    }
}

std::string GameRuntime::FullScriptPath(const std::string& path) const {
    if (path.empty() || cfg_.scriptBaseDir.empty()) return path;
    return cfg_.scriptBaseDir + "/" + path;
}

std::string GameRuntime::FullAssetPath(const std::string& path) const {
    // G7-1: "assets:/..." scheme normalization ("assets:/x.obj" == "x.obj").
    const std::string p = assets::NormalizeAssetPath(path);
    // G6-1: resolve the logical asset path through the variant table first
    // (unlisted paths fall back to themselves).
    const std::string resolved = cfg_.variantTable ? cfg_.variantTable->Resolve(p) : p;
    if (resolved.empty() || cfg_.assetBaseDir.empty()) return resolved;
    // Absolute paths (drive letter or leading separator) pass through unchanged.
    if (resolved.size() >= 2 && resolved[1] == ':') return resolved;
    if (resolved[0] == '/') return resolved;
    return cfg_.assetBaseDir + "/" + resolved;
}

std::string GameRuntime::ReadScript(const std::string& path) const {
    // G7-1: when a virtual file system is installed (pack + Mod mount stack),
    // script reads go through it with virtual paths -- the scriptBaseDir prefix
    // is stripped and the mount stack resolves pack-then-Mod. On a VFS miss we
    // fall through to the pack-reader override / disk as before.
    if (cfg_.fileSystem) {
        std::string rel = path;
        if (!cfg_.scriptBaseDir.empty()) {
            const std::string prefix = cfg_.scriptBaseDir + "/";
            if (path.rfind(prefix, 0) == 0) rel = path.substr(prefix.size());
        }
        const core::Result<std::vector<uint8_t>> bytes = cfg_.fileSystem->ReadFile(rel);
        if (bytes.Ok())
            return std::string(bytes.Value().begin(), bytes.Value().end());
    }
    if (cfg_.readScript) return cfg_.readScript(path);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::string out;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return out;
}

} // namespace neon::scene
