#pragma once

#include <string>

namespace neon::assets {

// Normalizes an asset path: strips a leading "assets:/" (or "asset:/") scheme
// so "assets:/models/x.obj" is the same path as "models/x.obj" everywhere.
// Non-scheme paths pass through unchanged (absolute paths, other prefixes).
// The scheme exists so asset references stay unambiguous ("this is a project
// asset, not a filesystem path") across scenes, prefabs, scripts and data.
inline std::string NormalizeAssetPath(const std::string& path) {
    if (path.rfind("assets:/", 0) == 0) return path.substr(8);
    if (path.rfind("asset:/", 0) == 0) return path.substr(7);
    return path;
}

} // namespace neon::assets
