#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"

namespace neon::assets {

class AssetManager;

// Unified result of loading a file-backed mesh format. Every registered loader
// (OBJ / glTF / FBX / ...) returns this so callers (the runtime's
// ResolveMeshKey, the editor's ResolveMesh) never need a per-format branch.
// `mesh` is the renderable GPU mesh; `material` carries the format's baked PBR
// when present (glTF/FBX), else a default lit material.
struct MeshLoadResult {
    gfx::Mesh mesh;
    gfx::Material material = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
};

// Registry of file-backed mesh formats, keyed by the meshKey prefix (excluding
// the trailing ':'): {"obj", "gltf", "fbx"}. Adding a new format requires only
// registering a loader here -- the runtime/editor/scene-file call sites stay
// unchanged. This replaces the hard-coded `meshKey.compare(0, n, "prefix:")`
// chains scattered across the engine and editor.
class MeshFormatRegistry {
public:
    // Loads `path` into a MeshLoadResult. The path is the part AFTER the
    // "prefix:" in the meshKey. Implementations need the AssetManager (for the
    // VFS file read + the renderer that uploads the GPU mesh).
    using Loader = std::function<MeshLoadResult(AssetManager&, const std::string& path)>;

    static MeshFormatRegistry& Instance();

    // Registers a loader for `prefix` (e.g. "obj" or "gltf"). The prefix is the
    // meshKey token before ':'; "fbx:" registers under "fbx". `extensions` are
    // the on-disk file suffixes the format is authored in ({"fbx"}, {".fbx"} is
    // normalized); `displayName` is the label shown in the asset panel / menus.
    void Register(const std::string& prefix, std::vector<std::string> extensions,
                  const std::string& displayName, Loader loader);

    // True when `meshKey` starts with "<prefix>:" for a registered format.
    bool HasPrefix(const std::string& meshKey) const;

    // Returns the registered prefix if `meshKey` starts with "<prefix>:", else
    // "". With `outPath` non-null, receives the meshKey's path suffix.
    std::string MatchPrefix(const std::string& meshKey, std::string* outPath = nullptr) const;

    // The registered format that owns `filePath` (by extension), or "" when the
    // path is not a recognized mesh model. With `outPrefix` the meshKey prefix
    // is returned too (e.g. a ".fbx" path -> "fbx").
    std::string FormatFromExt(const std::string& filePath) const;

    // Display label for a prefix ("gltf" -> "glTF 模型"). "" when unknown.
    std::string DisplayName(const std::string& prefix) const;

    // Loads a meshKey ("fbx:assets/models/x.fbx") via its registered loader.
    // Returns an empty result when the prefix is unknown or the load fails.
    MeshLoadResult Load(AssetManager& assets, const std::string& meshKey) const;

    // The set of registered prefixes (for UI labels / tooling).
    std::vector<std::string> Prefixes() const;

    // All registered file extensions with leading '.', deduplicated (for file
    // dialog filters / extension lists).
    std::vector<std::string> Extensions() const;

private:
    MeshFormatRegistry();
    std::map<std::string, Loader> loaders_;
    std::map<std::string, std::string> prefixToExt_;  // prefix -> one canonical ext
    std::map<std::string, std::string> extToPrefix_;  // lowercase ext -> prefix
    std::map<std::string, std::string> prefixToLabel_;
};

} // namespace neon::assets
