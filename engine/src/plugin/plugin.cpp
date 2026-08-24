#include "neon/plugin/plugin.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace neon::plugin {
namespace {

// Case-insensitive suffix match.
bool HasSuffixLower(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(s[s.size() - n + i])));
        const char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

std::string ReadFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Directories directly under `dir` (sorted, deterministic). Missing dirs
// yield an empty list.
std::vector<std::string> ListDirectories(const std::string& dir) {
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (::stat((dir + "/" + name).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) out.push_back(name);
    }
    ::closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

const char* PluginTypeName(PluginType t) {
    switch (t) {
        case PluginType::Runtime: return "runtime";
        case PluginType::Editor: return "editor";
        case PluginType::Native: return "native";
    }
    return "runtime";
}

PluginType PluginTypeFromName(const std::string& s) {
    if (s == "editor") return PluginType::Editor;
    if (s == "native") return PluginType::Native;
    return PluginType::Runtime; // "runtime" (and unknown values) default safe
}

bool ParseVersion(const std::string& s, Version* out) {
    if (!out) return false;
    *out = Version{};
    if (s.empty()) return true; // empty = any version
    int parts[3] = {0, 0, 0};
    int idx = 0;
    size_t i = 0;
    while (i <= s.size() && idx < 3) {
        if (i == s.size() || s[i] == '.') {
            ++idx;
            ++i;
            continue;
        }
        if (s[i] < '0' || s[i] > '9') return false;
        int v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            ++i;
        }
        parts[idx] = v;
    }
    if (idx < 1) return false; // at least "0"
    if (i < s.size()) return false; // trailing garbage after 3 groups
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];
    return true;
}

bool PluginManifest::Load(const core::Json& j, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (!j.IsObject()) return fail("plugin.json root must be an object");
    const core::Json* idJ = j.Get("id");
    const core::Json* entryJ = j.Get("entry");
    const core::Json* backendJ = j.Get("backend");
    if (!idJ || !idJ->IsString() || idJ->GetString().empty())
        return fail("plugin.json requires a non-empty string 'id'");
    if (!entryJ || !entryJ->IsString() || entryJ->GetString().empty())
        return fail("plugin.json requires a non-empty string 'entry'");
    if (!backendJ || !backendJ->IsString() || backendJ->GetString().empty())
        return fail("plugin.json requires a non-empty string 'backend'");

    id = idJ->GetString();
    entry = entryJ->GetString();
    backend = backendJ->GetString();
    if (backend != "lua" && backend != "js")
        return fail("plugin.json 'backend' must be 'lua' or 'js' (got '" + backend + "')");
    name = j.Get("name") ? j.Get("name")->GetString(id) : id;
    version = j.Get("version") ? j.Get("version")->GetString("0.0.0") : "0.0.0";
    type = PluginTypeFromName(j.Get("type") ? j.Get("type")->GetString("runtime") : "runtime");
    minEngineVersion =
        j.Get("minEngineVersion") ? j.Get("minEngineVersion")->GetString() : "";
    requires.clear();
    if (const core::Json* r = j.Get("requires")) {
        if (r->IsArray()) {
            for (const core::Json& it : r->Items())
                if (it.IsString() && !it.GetString().empty()) requires.push_back(it.GetString());
        } else if (r->IsString() && !r->GetString().empty()) {
            requires.push_back(r->GetString());
        }
    }
    permissions.clear();
    if (const core::Json* p = j.Get("permissions")) {
        if (p->IsArray()) {
            for (const core::Json& it : p->Items())
                if (it.IsString()) permissions.push_back(it.GetString());
        } else if (p->IsString() && !p->GetString().empty()) {
            permissions.push_back(p->GetString());
        }
    }
    return true;
}

bool PluginManifest::LoadJson(const std::string& text, std::string* err) {
    std::string perr;
    core::Json j = core::Json::Parse(text, &perr);
    if (j.IsNull() && !perr.empty()) {
        if (err) *err = "plugin.json parse error: " + perr;
        return false;
    }
    return Load(j, err);
}

std::vector<PluginManifest> DiscoverPlugins(const std::string& baseDir) {
    std::vector<PluginManifest> out;
    const std::string pluginsDir = baseDir + "/plugins";
    for (const std::string& sub : ListDirectories(pluginsDir)) {
        PluginManifest m;
        std::string err;
        const std::string text = ReadFileText(pluginsDir + "/" + sub + "/plugin.json");
        if (text.empty() || !m.LoadJson(text, &err)) {
            // Invalid plugins are skipped, never fatal to the host.
            continue;
        }
        m.dir = pluginsDir + "/" + sub;
        out.push_back(std::move(m));
    }
    // Deterministic load order: dependencies first is the caller's job; here
    // we only guarantee a stable order by id.
    std::sort(out.begin(), out.end(),
              [](const PluginManifest& a, const PluginManifest& b) { return a.id < b.id; });
    return out;
}

} // namespace neon::plugin
