#include "neon/assets/asset_db.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>

#include "neon/core/json.hpp"

namespace neon::assets {

namespace {

// Directories never tracked as assets (derived data / VCS / build output).
constexpr const char* kSkipDirs[] = {".git/", "build/", "out/", ".neon/", "node_modules/"};

bool IsTrackedFile(const std::string& rel) {
    for (const char* skip : kSkipDirs)
        if (rel.rfind(skip, 0) == 0) return false;
    if (rel == ".asset_db.json") return false; // the database itself
    return true;
}

std::string ToSlash(const std::string& p) {
    std::string out = p;
    for (char& ch : out)
        if (ch == '\\') ch = '/';
    return out;
}

// FNV-1a 64 over raw bytes, returned as 16-hex. 64 bits keeps collisions
// negligible for any realistic project size while staying cheap to compute.
std::string HashFileBytes(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ull; // FNV offset basis
    for (const uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull; // FNV prime
    }
    char out[17];
    for (int i = 0; i < 8; ++i)
        std::snprintf(out + i * 2, 3, "%02x", static_cast<unsigned>((h >> (i * 8)) & 0xFF));
    return std::string(out, 16);
}

// Reads the file's bytes and fills size + hash. Returns false on IO failure
// (the entry then carries an empty hash and is never matched as a move).
bool HashFile(const std::string& abs, uint64_t& size, std::string& hash) {
    std::ifstream in(abs, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    const std::streamsize sz = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (sz > 0) in.read(reinterpret_cast<char*>(bytes.data()), sz);
    size = static_cast<uint64_t>(sz);
    hash = HashFileBytes(bytes);
    return true;
}

// Legacy sidecar support: a pre-centralization project may still carry
// <asset>.meta files whose first line is the GUID. Adopt it so identities
// survive the migration to the single-file store.
std::string ReadLegacyMetaGuid(const std::string& metaPath) {
    std::ifstream in(metaPath, std::ios::binary);
    if (!in.is_open()) return {};
    std::string guid;
    for (std::istreambuf_iterator<char> it(in); it != std::istreambuf_iterator<char>(); ++it) {
        const char ch = *it;
        if (ch == '\n' || ch == '\r') break;
        if (!std::isspace(static_cast<unsigned char>(ch))) guid.push_back(ch);
    }
    if (guid.empty() || guid.size() > 32) return {};
    return guid;
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out = text;
    return true;
}

bool WriteFileText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << text;
    return out.good();
}

} // namespace

std::string NewGuid() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    for (char& c : out) c = hex[rng() & 0xF];
    return out;
}

AssetDatabase AssetDatabase::Build(const std::string& rootDir, const AssetDatabase& prev,
                                   std::vector<std::string>* adoptedMetas) {
    AssetDatabase db;
    std::error_code ec;
    if (!std::filesystem::exists(rootDir, ec)) return db;

    // Index the previous scan: by path (fast unchanged check) and by content
    // hash (move recovery). Entries whose path is gone may donate their GUID to
    // a new file with identical bytes.
    std::map<std::string, const AssetDbEntry*> prevByPath;
    std::multimap<std::string, const AssetDbEntry*> prevByHash;
    std::set<const AssetDbEntry*> unclaimed;
    for (const AssetDbEntry& e : prev.entries_) {
        prevByPath[e.path] = &e;
        if (!e.hash.empty()) prevByHash.emplace(e.hash, &e);
        unclaimed.insert(&e);
    }

    // First pass: which previous paths still exist on disk? A prev path that
    // no longer exists is a candidate donator for a new file with its hash.
    std::set<std::string> livePrevPaths;
    for (const auto& [p, entry] : prevByPath) {
        if (std::filesystem::exists(rootDir + "/" + p, ec)) livePrevPaths.insert(p);
        ec.clear();
    }

    for (std::filesystem::recursive_directory_iterator it(rootDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = ToSlash(std::filesystem::relative(it->path(), rootDir, ec).string());
        if (!IsTrackedFile(rel)) continue;
        if (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".meta") == 0) continue;

        AssetDbEntry e;
        e.path = rel;

        // Fast path: same path as the previous scan and file stats untouched —
        // reuse the stored identity without reading any bytes.
        const auto pit = prevByPath.find(rel);
        if (pit != prevByPath.end()) {
            const AssetDbEntry& p = *pit->second;
            e.guid = p.guid;
            e.hash = p.hash;
            e.size = p.size;
            e.mtime = p.mtime;
            const bool statsSame =
                std::filesystem::last_write_time(it->path(), ec).time_since_epoch().count() ==
                    p.mtime &&
                static_cast<uint64_t>(it->file_size(ec)) == p.size;
            if (!statsSame || e.hash.empty()) {
                HashFile(rootDir + "/" + rel, e.size, e.hash);
                e.mtime =
                    std::filesystem::last_write_time(it->path(), ec).time_since_epoch().count();
            }
            unclaimed.erase(&p);
            db.entries_.push_back(std::move(e));
            continue;
        }

        // New path. A legacy sidecar meta wins over hashing-based recovery.
        const std::string legacyGuid = ReadLegacyMetaGuid(rootDir + "/" + rel + ".meta");
        if (!legacyGuid.empty()) {
            e.guid = legacyGuid;
            HashFile(rootDir + "/" + rel, e.size, e.hash);
            e.mtime = std::filesystem::last_write_time(it->path(), ec).time_since_epoch().count();
            if (adoptedMetas) adoptedMetas->push_back(rootDir + "/" + rel + ".meta");
            db.entries_.push_back(std::move(e));
            continue;
        }

        HashFile(rootDir + "/" + rel, e.size, e.hash);
        // Move recovery: identical bytes to a vanished previous entry.
        bool adopted = false;
        if (!e.hash.empty()) {
            const auto range = prevByHash.equal_range(e.hash);
            for (auto mit = range.first; mit != range.second; ++mit) {
                const AssetDbEntry* cand = mit->second;
                if (unclaimed.count(cand) && !livePrevPaths.count(cand->path)) {
                    e.guid = cand->guid;
                    unclaimed.erase(cand);
                    adopted = true;
                    break;
                }
            }
        }
        if (!adopted) e.guid = NewGuid();
        e.mtime = std::filesystem::last_write_time(it->path(), ec).time_since_epoch().count();
        db.entries_.push_back(std::move(e));
    }
    return db;
}

std::string AssetDatabase::GuidFor(const std::string& projectRelativePath) const {
    for (const AssetDbEntry& e : entries_)
        if (e.path == projectRelativePath) return e.guid;
    return {};
}

std::string AssetDatabase::PathFor(const std::string& guid) const {
    for (const AssetDbEntry& e : entries_)
        if (e.guid == guid) return e.path;
    return {};
}

std::string AssetDatabase::ToJson() const {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const AssetDbEntry& e : entries_) {
        auto str = [](const std::string& v) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = v;
            return j;
        };
        core::Json j;
        j.type_ = core::Json::Type::Object;
        j.object_["path"] = str(e.path);
        j.object_["guid"] = str(e.guid);
        j.object_["hash"] = str(e.hash);
        // Numbers go out as STRINGS: JsonWriter prints with %g (6 significant
        // digits), which would round 64-bit sizes and mtimes.
        j.object_["size"] = str(std::to_string(e.size));
        j.object_["mtime"] = str(std::to_string(e.mtime));
        arr.array_.push_back(std::move(j));
    }
    root.object_["assets"] = std::move(arr);
    return core::JsonWriter::Write(root);
}

AssetDatabase AssetDatabase::FromJson(const std::string& jsonText) {
    AssetDatabase db;
    std::string err;
    const core::Json root = core::Json::Parse(jsonText, &err);
    if (!err.empty() || !root.IsObject()) return db;
    const core::Json* arr = root.Get("assets");
    if (!arr || !arr->IsArray()) return db;
    for (const core::Json& j : arr->Items()) {
        if (!j.IsObject()) continue;
        AssetDbEntry e;
        e.path = j.Get("path") ? j.Get("path")->GetString() : "";
        e.guid = j.Get("guid") ? j.Get("guid")->GetString() : "";
        if (e.path.empty() || e.guid.empty()) continue;
        if (const core::Json* h = j.Get("hash")) e.hash = h->GetString();
        if (const core::Json* s = j.Get("size")) e.size = std::strtoull(s->GetString().c_str(),
                                                                        nullptr, 10);
        if (const core::Json* m = j.Get("mtime")) e.mtime = std::strtoll(m->GetString().c_str(),
                                                                         nullptr, 10);
        db.entries_.push_back(std::move(e));
    }
    return db;
}

std::vector<AssetMove> DetectAssetMoves(const AssetDatabase& before,
                                        const AssetDatabase& after) {
    std::vector<AssetMove> moves;
    std::map<std::string, std::string> guidToOld;
    for (const AssetDbEntry& e : before.Entries()) guidToOld[e.guid] = e.path;
    for (const AssetDbEntry& e : after.Entries()) {
        auto it = guidToOld.find(e.guid);
        if (it != guidToOld.end() && it->second != e.path) {
            moves.push_back({it->second, e.path});
        }
    }
    return moves;
}

std::string RewriteJsonReferences(const std::string& jsonText,
                                  const std::vector<AssetMove>& moves) {
    if (moves.empty() || jsonText.empty()) return jsonText;
    std::string out;
    out.reserve(jsonText.size());
    size_t i = 0;
    const size_t n = jsonText.size();
    while (i < n) {
        bool matched = false;
        for (const AssetMove& m : moves) {
            if (m.oldPath.empty()) continue;
            if (jsonText.compare(i, m.oldPath.size(), m.oldPath) == 0) {
                out += m.newPath;
                i += m.oldPath.size();
                matched = true;
                break;
            }
        }
        if (!matched) out += jsonText[i++];
    }
    return out;
}

} // namespace neon::assets
