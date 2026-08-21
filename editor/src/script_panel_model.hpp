#pragma once

// Script panel (T4.5) pure model: the ImGui-free logic shared by the panel UI
// (panels.cpp), the scene export/load paths and the unit tests. Like
// bt_editor.hpp, everything here is inline + dependency-light (core::Json and
// the script host interface only), so the test binary can exercise it headlessly:
//   * script file enumeration under <projectDir>/scripts/ (recursive, relative)
//   * per-file syntax check through IScriptHost::CheckSyntax (message + line)
//   * the entity's script component fields (SceneScriptFields) with an equality
//     helper used by the editor's undo command

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
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

namespace neon::editor {

// The script component carried by a SceneEntity (mirrors scene::SceneScript:
// backend/path/vars). An empty `path` means no script is attached.
struct SceneScriptFields {
    std::string backend; // "lua" (empty means unattached / default)
    std::string path;    // project-relative, e.g. "scripts/wolf.lua"
    core::Json vars;     // object, or null when absent
};

inline bool ScriptFieldsEqual(const SceneScriptFields& a, const SceneScriptFields& b) {
    return a.backend == b.backend && a.path == b.path &&
           core::JsonWriter::Write(a.vars) == core::JsonWriter::Write(b.vars);
}

// Result of a syntax check on one project script.
struct ScriptCheckResult {
    std::string path;    // project-relative, e.g. "scripts/foo.lua"
    std::string message; // empty when the script compiles
    int line = 0;        // source line of the error (0 when unknown)
    bool ok = true;
};

// The project's scripts directory: <projectDir>/scripts ("" base -> "./scripts").
inline std::string ScriptsDir(const std::string& projectDir) {
    std::string base = projectDir.empty() ? "." : projectDir;
    return base + "/scripts";
}

// Recursively enumerate every *.lua under `dir`, appending project-relative
// paths ("scripts/foo.lua", "scripts/sub/x.lua") to `out`. `prefix` is the
// relative path of `dir` ("scripts", "scripts/sub", ...). Missing dirs yield
// an empty list (not an error). Results are sorted for deterministic UI/tests.
inline void ListLuaFiles(const std::string& dir, const std::string& prefix,
                         std::vector<std::string>& out) {
#if defined(_WIN32)
    auto utf8ToWide = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    };
    auto wideToUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr,
                            nullptr);
        return s;
    };
    std::wstring pattern = utf8ToWide(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    std::vector<std::wstring> files;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            subdirs.push_back(name);
        else
            files.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::vector<std::string> luaNames;
    for (const std::wstring& f : files) {
        std::string name = wideToUtf8(f);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0)
            luaNames.push_back(name);
    }
    std::sort(luaNames.begin(), luaNames.end());
    for (const std::string& f : luaNames) out.push_back(prefix + "/" + f);

    std::vector<std::string> subNames;
    for (const std::wstring& s : subdirs) subNames.push_back(wideToUtf8(s));
    std::sort(subNames.begin(), subNames.end());
    for (const std::string& s : subNames)
        ListLuaFiles(dir + "/" + s, prefix + "/" + s, out);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    std::vector<std::string> files;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            subdirs.push_back(name);
        else if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".lua") == 0)
            files.push_back(name);
    }
    ::closedir(d);
    std::sort(files.begin(), files.end());
    std::sort(subdirs.begin(), subdirs.end());
    for (const std::string& f : files) out.push_back(prefix + "/" + f);
    for (const std::string& s : subdirs) ListLuaFiles(dir + "/" + s, prefix + "/" + s, out);
#endif
}

// Read a text file's bytes. Returns false when the file cannot be opened; `out`
// is left untouched. An empty file is a successful read (empty Lua source is
// valid), so callers can distinguish "empty" from "unreadable".
inline bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Run a syntax check on one project script. `base` is the project directory and
// `relPath` a project-relative path ("scripts/foo.lua"); the file is read from
// <base>/<relPath> and compiled with the host's CheckSyntax (the editor reuses
// one throwaway host for all checks, so no script ever runs here).
inline ScriptCheckResult CheckScriptFile(script::IScriptHost& host, const std::string& base,
                                         const std::string& relPath) {
    ScriptCheckResult r;
    r.path = relPath;
    std::string source;
    if (!ReadTextFile(base + "/" + relPath, source)) {
        r.ok = false;
        r.message = "无法读取文件";
        return r;
    }
    // An empty source is valid Lua (an empty chunk compiles fine).
    if (host.CheckSyntax(source)) {
        r.ok = true;
        r.message.clear();
        r.line = 0;
    } else {
        r.ok = false;
        const script::ScriptError& err = host.LastError();
        r.message = err.message;
        r.line = err.line;
    }
    return r;
}

} // namespace neon::editor
