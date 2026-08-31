#pragma once
// C1: internal helpers shared across the GameRuntime translation-unit split.
// game_runtime.cpp keeps its own internal copies (anonymous namespace); new
// TUs (content/combat/...) include this header so the moved member functions
// can call the same helpers. All are `inline` to avoid ODR conflicts when a
// TU also carries a local copy.
//
// NOT installed -- lives in engine/src/scene next to the runtime sources.

#include <string>
#include <vector>

#include "neon/gfx/color.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/math/math.hpp"
#include "neon/script/script.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace neon::scene {
namespace detail {

// Entity handle as a Lua table {id, gen} (matches the T2.3 bindings' shape).
inline script::Value EntityToValue(const ecs::Entity& e) {
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", script::Value::Num(static_cast<double>(e.generation)));
    return t;
}

// Stable 64-bit key for per-entity BT/blackboard scoping: id occupies the high
// half so an id reused across generations still keys uniquely.
inline uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

// "#RRGGBB" -> Color; empty/invalid -> white.
inline gfx::Color ParseColorHex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return gfx::Color::White;
    auto nibble = [](char c) -> unsigned int {
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(c - 'A' + 10);
        return 255u;
    };
    auto byte = [&](char hi, char lo) {
        return static_cast<unsigned int>((nibble(hi) << 4) | nibble(lo));
    };
    return {byte(hex[1], hex[2]) / 255.0f, byte(hex[3], hex[4]) / 255.0f,
            byte(hex[5], hex[6]) / 255.0f, 1.0f};
}

// Case-insensitive suffix match ("main.JSON" counts as a .json prefab).
inline bool HasSuffix(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(
            s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

// "a/b/c.json" -> "c" (the prefab registration name).
inline std::string FileStem(const std::string& p) {
    const size_t slash = p.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Recursively lists every file under `absDir` (sorted, forward-slash relative
// to `absDir`). Missing directories yield an empty list (not an error).
inline void ListFilesRecursive(const std::string& absDir, const std::string& prefix,
                               std::vector<std::string>& out) {
#if defined(_WIN32)
    std::string pattern = absDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::string, bool>> entries; // name, isDir
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        entries.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second)
            ListFilesRecursive(absDir + "/" + e.first, rel, out);
        else
            out.push_back(rel);
    }
#else
    DIR* d = ::opendir(absDir.c_str());
    if (!d) return;
    std::vector<std::pair<std::string, bool>> entries;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = absDir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        entries.emplace_back(name, S_ISDIR(st.st_mode));
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second)
            ListFilesRecursive(absDir + "/" + e.first, rel, out);
        else
            out.push_back(rel);
    }
#endif
}

// Two materials render identically (same shader/textures/scalars/flags), so
// their entities can share one instanced draw. Exact float equality is fine:
// materials are copied from the same resolved source, and materials that
// merely have numerically identical values are safe to batch.
inline bool SameMaterial(const gfx::Material& a, const gfx::Material& b) {
    return a.shader.id == b.shader.id && a.albedo.id == b.albedo.id &&
           a.metallicRoughness.id == b.metallicRoughness.id &&
           a.occlusion.id == b.occlusion.id && a.emissive.id == b.emissive.id &&
           a.tint.r == b.tint.r && a.tint.g == b.tint.g && a.tint.b == b.tint.b &&
           a.tint.a == b.tint.a && a.shininess == b.shininess && a.metallic == b.metallic &&
           a.roughness == b.roughness && a.aoStrength == b.aoStrength &&
           a.emissiveIntensity == b.emissiveIntensity && a.lit == b.lit &&
           a.transparent == b.transparent && a.doubleSided == b.doubleSided &&
           a.alphaTest == b.alphaTest && a.alphaCutoff == b.alphaCutoff;
}

// Returns the mesh to draw for one entity given a camera position: the single
// resolved mesh when the item has no LOD chain, else the chain level selected
// by PickLod. Falls back to the base mesh for a malformed/missing selection.
inline const gfx::Mesh& SelectLodMesh(const gfx::Mesh& base, const gfx::LodChain& chain,
                                      const math::Vec3& pos, const math::Vec3& camPos) {
    if (chain.levels.empty()) return base;
    const int level = PickLod(chain, math::Distance(pos, camPos));
    if (level < 0 || static_cast<size_t>(level) >= chain.levels.size()) return base;
    return chain.levels[static_cast<size_t>(level)];
}

} // namespace detail
} // namespace neon::scene
