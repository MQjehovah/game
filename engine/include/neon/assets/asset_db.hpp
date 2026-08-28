#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neon::assets {

// Asset GUID database (G5-4-4项3, central-store revision): EVERY fact the
// editor knows about project assets lives in ONE file, <project>/.asset_db.json
// (path -> guid + content hash + size + mtime). There are no per-file sidecar
// files. The database is the single source of truth for asset identity:
//
//   * A file that keeps its path keeps its GUID (path is the primary key).
//   * A file that shows up at a NEW path whose content hash matches a
//     previously-known path that has VANISHED is a MOVE: the old entry's GUID
//     is adopted, DetectAssetMoves reports oldPath -> newPath, and the editor
//     rewrites path references inside scene/prefab/UI JSON so nothing breaks.
//   * Anything else new gets a fresh GUID; anything gone is dropped.
//
// Size + mtime ride along so a rescan can reuse a stored hash without reading
// file bytes when neither changed (the common case: nothing happened).

struct AssetDbEntry {
    std::string path;  // project-relative, forward slashes ("assets/sprites/x.png")
    std::string guid;  // stable 16-hex GUID
    std::string hash;  // FNV-1a 64 over the file bytes, 16-hex ("" = unknown)
    uint64_t size = 0;
    int64_t mtime = 0; // last_write_time tick count (same-machine comparisons only)
};

class AssetDatabase {
public:
    // Scans `rootDir` and returns the fresh database. `prev` is the previous
    // scan (loaded from .asset_db.json by the caller): unchanged files reuse
    // the stored hash without re-reading bytes, same-path files keep their
    // GUID, and vanished-path + matching-hash pairs are recognized as moves
    // (the moved file inherits the old GUID). Files with a legacy sibling
    // <asset>.meta have that meta's GUID adopted instead of a fresh one (the
    // meta path is reported through `adoptedMetas` so the caller can delete
    // the now-redundant file).
    static AssetDatabase Build(const std::string& rootDir, const AssetDatabase& prev = {},
                               std::vector<std::string>* adoptedMetas = nullptr);

    std::string GuidFor(const std::string& projectRelativePath) const;
    std::string PathFor(const std::string& guid) const;
    const std::vector<AssetDbEntry>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

    // Snapshot serialization (the single .asset_db.json file).
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
