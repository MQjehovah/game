#pragma once

#include <string>

namespace neon::assets {

// Normalizes an asset path to the canonical project-relative form "assets/...".
// Accepted schemes:
//   "@assets/x"   -> "assets/x"   (readable virtual-directory form)
//   "assets:/x"   -> "assets/x"   (legacy scheme name)
//   "asset:/x"    -> "assets/x"
// It also collapses a duplicated "assets/" segment so historical bad data like
// "@assets/assets/x" or "assets/assets/x" normalizes to a single "assets/x".
// Everything else (plain relative, absolute) passes through unchanged; the
// asset reference is left for the caller to resolve against its base dir.
inline std::string NormalizeAssetPath(const std::string& path) {
    std::string p = path;
    for (int pass = 0; pass < 4; ++pass) {
        if (p.rfind("@assets/", 0) == 0) p = "assets/" + p.substr(8);
        else if (p.rfind("@asset/", 0) == 0) p = "assets/" + p.substr(7);
        else if (p.rfind("assets:/", 0) == 0) p = "assets/" + p.substr(8);
        else if (p.rfind("asset:/", 0) == 0) p = "assets/" + p.substr(7);
        else if (p.rfind("assets/assets/", 0) == 0) p = p.substr(7); // dedupe
        else break;
    }
    return p;
}

// True when `path` carries an explicit asset scheme ("@assets/", "assets:/",
// "asset:/"). Used by callers that must route a reference through the asset
// VFS (project-root) rather than treat it as a raw filesystem path.
inline bool HasAssetScheme(const std::string& path) {
    return path.rfind("@assets/", 0) == 0 || path.rfind("@asset/", 0) == 0 ||
           path.rfind("assets:/", 0) == 0 || path.rfind("asset:/", 0) == 0;
}

} // namespace neon::assets
