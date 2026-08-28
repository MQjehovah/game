#pragma once

#include <map>
#include <string>
#include <vector>

namespace neon::assets {

// G5-4-4(项3) lightweight asset database (Unity ".meta" model): every asset in
// a project has a sibling <asset>.meta file whose first line is a stable 16-hex
// GUID. The GUID physically travels with the file, so a moved/renamed asset
// keeps its identity — the editor compares two scans and rewrites path
// references for anything whose GUID changed path. Scene/prefab/UI files keep
// PATH-based references (the runtime resolves paths), so the rewrite is what
// makes renames safe without changing the storage format.

struct AssetDbEntry {
    std::string path; // project-relative, forward slashes ("assets/sprites/x.png")
    std::string guid; // stable 16-hex GUID (from <asset>.meta)
};

class AssetDatabase {
public:
    // Scans `rootDir` (project dir): ensures a .meta for every tracked asset
    // (reading an existing GUID, generating + writing a fresh one otherwise)
    // and returns the path -> GUID map. Skips .meta files themselves and known
    // non-asset dirs (.neon — the engine's derived-data cache — .git, build,
    // out).
    static AssetDatabase Build(const std::string& rootDir);

    std::string GuidFor(const std::string& projectRelativePath) const;
    std::string PathFor(const std::string& guid) const;
    const std::vector<AssetDbEntry>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

    // Snapshot serialization (the editor stores the previous scan so the next
    // one can DetectAssetMoves against it).
    std::string ToJson() const;
    static AssetDatabase FromJson(const std::string& jsonText);

private:
    std::vector<AssetDbEntry> entries_;
};

// A GUID-preserving path change (the file was moved/renamed between two scans).
struct AssetMove {
    std::string oldPath; // project-relative
    std::string newPath;
};

// Compares `before` and `after`: every entry whose GUID survived but whose path
// changed is a move (oldPath -> newPath).
std::vector<AssetMove> DetectAssetMoves(const AssetDatabase& before,
                                        const AssetDatabase& after);

// Rewrites path references inside a scene/prefab/UI JSON document for the given
// moves. A reference is any JSON string value containing an old path as a
// contiguous substring (exact, forward-slash). Returns the rewritten text.
std::string RewriteJsonReferences(const std::string& jsonText,
                                  const std::vector<AssetMove>& moves);

// Generates a fresh 16-hex GUID.
std::string NewGuid();

} // namespace neon::assets
