#pragma once

// G6-1 platform/LOD asset variants: a logical asset path resolves to a concrete
// file per variant ("mobile" / "pc" / "lod0"...). Unlisted paths fall back to
// themselves, so the table is a sparse override layer over the base pack.
//
// Project layout: <project>/variants.json
//   {
//     "mobile": { "models/wolf.obj": "models/wolf_low.obj",
//                 "textures/rock.png": "textures/rock_1k.png" },
//     "pc":     { "models/wolf.obj": "models/wolf_hi.obj" }
//   }
// The host (neon_game --variant <name>) loads the table and GameRuntime resolves
// every asset path through it before loading, so platform/LOD variants are pure
// data. Variant files are ordinary assets under the project (and therefore pack)
// — the packer needs no changes.

#include <map>
#include <string>

#include "neon/core/json.hpp"

namespace neon::assets {

class AssetVariantTable {
public:
    // Maps a logical (relative) asset path to its concrete file for this
    // variant. Returns `logical` when unlisted (fallback to the base asset).
    std::string Resolve(const std::string& logical) const {
        const auto it = map_.find(logical);
        return it == map_.end() ? logical : it->second;
    }

    bool Set(const std::string& logical, const std::string& concrete) {
        if (logical.empty() || concrete.empty()) return false;
        map_[logical] = concrete;
        return true;
    }

    bool Empty() const { return map_.empty(); }
    size_t Size() const { return map_.size(); }

    // Parses a JSON object (path -> concrete path). Returns false with *err on
    // malformed input; non-string values are rejected.
    bool LoadJson(const std::string& text, std::string* err) {
        std::string parseErr;
        core::Json root = core::Json::Parse(text, &parseErr);
        if (!parseErr.empty()) {
            if (err) *err = "variant table: " + parseErr;
            return false;
        }
        return Load(root, err);
    }

    bool Load(const core::Json& root, std::string* err) {
        if (!root.IsObject()) {
            if (err) *err = "variant table: must be a JSON object";
            return false;
        }
        for (const auto& [key, value] : root.Members()) {
            if (!value.IsString()) {
                if (err) *err = "variant table: '" + key + "' must map to a file path string";
                return false;
            }
            map_[key] = value.GetString();
        }
        return true;
    }

    std::string ToJson() const {
        core::Json root;
        root.type_ = core::Json::Type::Object;
        for (const auto& [logical, concrete] : map_) {
            core::Json v;
            v.type_ = core::Json::Type::String;
            v.string_ = concrete;
            root.object_[logical] = std::move(v);
        }
        return core::JsonWriter::Write(root);
    }

    // Reads a named variant table from a variants.json document
    // ({"mobile": {...}, "pc": {...}}) and loads it into `out`. Returns false
    // (with *err) when the variant is absent or malformed.
    static bool LoadVariant(const std::string& text, const std::string& variant,
                            AssetVariantTable& out, std::string* err) {
        std::string parseErr;
        core::Json root = core::Json::Parse(text, &parseErr);
        if (!parseErr.empty()) {
            if (err) *err = "variants.json: " + parseErr;
            return false;
        }
        const core::Json* table = root.Get(variant);
        if (!table) {
            if (err) *err = "variants.json: no '" + variant + "' variant";
            return false;
        }
        return out.Load(*table, err);
    }

private:
    std::map<std::string, std::string> map_;
};

} // namespace neon::assets
