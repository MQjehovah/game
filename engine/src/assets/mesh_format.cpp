#include "neon/assets/mesh_format.hpp"

#include <cctype>
#include <set>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"

namespace neon::assets {

namespace {
// Registers the three built-in file-backed formats on first use. Each loader
// adapts the AssetManager's existing importer (VFS read + GPU upload inside)
// into the unified MeshLoadResult. New third-party formats register here or via
// MeshFormatRegistry::Register.
struct BuiltinRegistrar {
    BuiltinRegistrar() {
        auto& reg = MeshFormatRegistry::Instance();
        reg.Register("obj", {".obj"}, "OBJ 模型",
                     [](AssetManager& a, const std::string& p) -> MeshLoadResult {
            MeshLoadResult r;
            r.mesh = a.LoadMeshOBJ(p);
            r.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
            return r;
        });
        reg.Register("gltf", {".gltf", ".glb"}, "glTF 模型",
                     [](AssetManager& a, const std::string& p) -> MeshLoadResult {
            MeshLoadResult r;
            GltfAsset g = a.LoadGLTF(p);
            if (!g.nodes.empty()) {
                r.mesh = g.nodes[0].mesh;
                r.material = g.nodes[0].material;
            }
            return r;
        });
        reg.Register("fbx", {".fbx"}, "FBX 模型",
                     [](AssetManager& a, const std::string& p) -> MeshLoadResult {
            MeshLoadResult r;
            FbxAsset f = a.LoadFBX(p);
            if (!f.nodes.empty()) {
                r.mesh = f.nodes[0].mesh;
                r.material = f.nodes[0].material;
            }
            return r;
        });
    }
};
BuiltinRegistrar g_builtinFormats;

} // namespace

MeshFormatRegistry& MeshFormatRegistry::Instance() {
    static MeshFormatRegistry reg;
    return reg;
}

MeshFormatRegistry::MeshFormatRegistry() = default;

void MeshFormatRegistry::Register(const std::string& prefix,
                                  std::vector<std::string> extensions,
                                  const std::string& displayName, Loader loader) {
    loaders_[prefix] = std::move(loader);
    prefixToLabel_[prefix] = displayName;
    // Normalize + record first extension as canonical; map every ext -> prefix.
    for (std::string ext : extensions) {
        if (!ext.empty() && ext[0] != '.') ext = "." + ext;
        std::string lower;
        for (char c : ext) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (prefixToExt_.count(prefix) == 0) prefixToExt_[prefix] = lower;
        extToPrefix_[lower] = prefix;
    }
}

bool MeshFormatRegistry::HasPrefix(const std::string& meshKey) const {
    if (meshKey.empty()) return false;
    // A format prefix is "label:" immediately followed by a non-empty path.
    const size_t colon = meshKey.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= meshKey.size()) return false;
    return loaders_.count(meshKey.substr(0, colon)) != 0;
}

std::string MeshFormatRegistry::MatchPrefix(const std::string& meshKey,
                                            std::string* outPath) const {
    if (meshKey.empty()) return {};
    const size_t colon = meshKey.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= meshKey.size()) return {};
    const std::string prefix = meshKey.substr(0, colon);
    if (loaders_.count(prefix) == 0) return {};
    if (outPath) *outPath = meshKey.substr(colon + 1);
    return prefix;
}

std::string MeshFormatRegistry::FormatFromExt(const std::string& filePath) const {
    if (filePath.empty()) return {};
    const size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext;
    for (size_t i = dot; i < filePath.size(); ++i)
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(filePath[i]))));
    auto it = extToPrefix_.find(ext);
    return it == extToPrefix_.end() ? std::string{} : it->second;
}

std::string MeshFormatRegistry::DisplayName(const std::string& prefix) const {
    auto it = prefixToLabel_.find(prefix);
    return it == prefixToLabel_.end() ? std::string{} : it->second;
}

MeshLoadResult MeshFormatRegistry::Load(AssetManager& assets, const std::string& meshKey) const {
    std::string path;
    const std::string prefix = MatchPrefix(meshKey, &path);
    if (prefix.empty()) return {};
    auto it = loaders_.find(prefix);
    if (it == loaders_.end()) return {};
    return it->second(assets, path);
}

std::vector<std::string> MeshFormatRegistry::Prefixes() const {
    std::vector<std::string> out;
    out.reserve(loaders_.size());
    for (const auto& [prefix, _] : loaders_) out.push_back(prefix);
    return out;
}

std::vector<std::string> MeshFormatRegistry::Extensions() const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& [ext, _] : extToPrefix_) {
        if (seen.insert(ext).second) out.push_back(ext);
    }
    return out;
}

} // namespace neon::assets
