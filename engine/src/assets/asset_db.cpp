#include "neon/assets/asset_db.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

#include "neon/core/json.hpp"

namespace neon::assets {

namespace {

bool IsTrackedFile(const std::string& rel) {
    for (const char* skip : {".git/", "build/", "out/", "import_cache/", "node_modules/"})
        if (rel.rfind(skip, 0) == 0) return false;
    return true;
}

std::string ToSlash(const std::string& p) {
    std::string out = p;
    for (char& ch : out)
        if (ch == '\\') ch = '/';
    return out;
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

AssetDatabase AssetDatabase::Build(const std::string& rootDir) {
    AssetDatabase db;
    std::error_code ec;
    if (!std::filesystem::exists(rootDir, ec)) return db;
    for (std::filesystem::recursive_directory_iterator it(rootDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = ToSlash(std::filesystem::relative(it->path(), rootDir, ec).string());
        if (!IsTrackedFile(rel)) continue;
        if (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".meta") == 0) continue;
        const std::string metaPath = rootDir + "/" + rel + ".meta";
        std::string guid;
        std::string metaText;
        if (ReadFileText(metaPath, metaText)) {
            // First non-empty line is the GUID.
            for (const char ch : metaText) {
                if (ch == '\n' || ch == '\r') break;
                if (!std::isspace(static_cast<unsigned char>(ch))) guid.push_back(ch);
            }
        }
        if (guid.empty() || guid.size() > 32) {
            guid = NewGuid();
            WriteFileText(metaPath, guid + "\n");
        }
        AssetDbEntry e;
        e.path = rel;
        e.guid = guid;
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
        core::Json j;
        j.type_ = core::Json::Type::Object;
        core::Json p;
        p.type_ = core::Json::Type::String;
        p.string_ = e.path;
        core::Json g;
        g.type_ = core::Json::Type::String;
        g.string_ = e.guid;
        j.object_["path"] = std::move(p);
        j.object_["guid"] = std::move(g);
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
