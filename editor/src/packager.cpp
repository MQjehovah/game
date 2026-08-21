#include "packager.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/json.hpp"
#include "neon/core/pack.hpp"
#include "neon/scene/game_manifest.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/script/script.hpp"
#include "script_panel_model.hpp"

namespace neon::editor::pack {
namespace {

// ---------------------------------------------------------------------------
// Path / filesystem helpers (Windows uses the Wide Win32 APIs so UTF-8 paths
// round-trip; POSIX uses dirent/stat).
// ---------------------------------------------------------------------------

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr,
                            nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}
#endif

// Forward slashes everywhere (the pack's virtual paths are normalized keys).
std::string Normalize(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

bool FileExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}

bool DirExists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return CreateDirectoryW(Utf8ToWide(path).c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

bool MakeDirs(const std::string& path) {
    if (path.empty() || path == ".") return true;
    std::string acc;
    size_t i = 0;
    while (i < path.size()) {
        size_t next = path.find_first_of("/\\", i);
        if (next == std::string::npos) next = path.size();
        std::string comp = path.substr(i, next - i);
        i = next + 1;
        if (comp.empty()) continue;
        acc = acc.empty() ? comp : acc + "/" + comp;
        // A Windows drive root like "C:" already exists; skip creation.
        if (acc.size() == 2 && acc[1] == ':') continue;
        if (!MakeDir(acc)) return false;
    }
    return DirExists(path);
}

// Recursively enumerate every file under `absDir`, appending project-relative
// virtual paths ("prefix/name", "prefix/sub/name", ...) to `out`. Missing dirs
// yield an empty list (not an error). Results are sorted for deterministic
// packs and reports.
void ListFilesRecursive(const std::string& absDir, const std::string& prefix,
                        std::vector<std::string>& out) {
#if defined(_WIN32)
    std::wstring pattern = Utf8ToWide(absDir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::wstring, bool>> entries; // name, isDir
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        entries.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        std::string name = WideToUtf8(e.first);
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (e.second)
            ListFilesRecursive(absDir + "/" + name, rel, out);
        else
            out.push_back(rel);
    }
#else
    DIR* d = ::opendir(absDir.c_str());
    if (!d) return;
    std::vector<std::pair<std::string, bool>> entries;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = absDir + "/" + name;
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

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    return !in.bad();
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool HasExt(const std::string& rel, const std::string& ext) {
    if (rel.size() < ext.size()) return false;
    return rel.compare(rel.size() - ext.size(), ext.size(), ext) == 0;
}

std::string BaseName(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string FileStem(const std::string& p) {
    std::string name = BaseName(p);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Turn a raw reference into a pack virtual path: forward slashes, no leading
// "./" or "/", and a drive-prefixed absolute path collapses to its basename.
std::string VirtualPathOf(std::string ref) {
    ref = Normalize(ref);
    if (ref.size() >= 2 && ref[1] == ':') {
        size_t slash = ref.find('/');
        ref = slash == std::string::npos ? ref.substr(2) : ref.substr(slash + 1);
    }
    while (ref.size() >= 2 && ref.compare(0, 2, "./") == 0) ref = ref.substr(2);
    while (!ref.empty() && ref[0] == '/') ref = ref.substr(1);
    return ref;
}

// Resolve a scene asset reference to a physical file: <projectDir>/<ref> first
// (the project-local convention), then <ref> relative to the current directory
// (the engine's repo-relative "assets/..." convention). "" when neither exists.
std::string ResolveRef(const std::string& projectDir, const std::string& ref) {
    if (ref.empty()) return "";
    const std::string norm = Normalize(ref);
    const std::string joined = projectDir + "/" + norm;
    if (FileExists(joined)) return joined;
    if (FileExists(norm)) return norm;
    return "";
}

// ---------------------------------------------------------------------------
// Validation context + passes
// ---------------------------------------------------------------------------

// Mutable per-run state shared by the validation passes and the pack step.
// packFiles maps a virtual path to its absolute source path ("game.json" is
// special: its bytes come from manifestText, not from disk).
struct ProjectContext {
    std::string projectDir;
    PackageReport report;
    std::map<std::string, std::string> packFiles;
    std::string manifestText; // normalized game.json (GameManifest::ToJson)
    bool manifestOk = false;
    bool syntaxCheck = true;
};

void CheckAssetRef(ProjectContext& pc, const std::string& ref) {
    if (ref.empty()) return;
    std::string abs = ResolveRef(pc.projectDir, ref);
    if (abs.empty()) {
        pc.report.errors.push_back("missing asset '" + ref + "'");
        return;
    }
    std::string vp = VirtualPathOf(ref);
    if (vp.empty()) {
        pc.report.warnings.push_back("asset reference '" + ref + "' has no usable path (skipped)");
        return;
    }
    pc.packFiles[vp] = abs;
}

void ValidateMesh(ProjectContext& pc, const std::string& where, const core::Json& data) {
    if (!data.IsObject()) {
        pc.report.errors.push_back(where + ": mesh component must be a JSON object");
        return;
    }
    const core::Json* key = data.Get("meshKey");
    if (!key || !key->IsString() || key->GetString().empty()) {
        pc.report.errors.push_back(where + ": mesh requires a non-empty 'meshKey'");
        return;
    }
    const std::string& k = key->GetString();
    if (k.compare(0, 4, "obj:") == 0) {
        CheckAssetRef(pc, k.substr(4));
    } else if (k.compare(0, 5, "gltf:") == 0) {
        CheckAssetRef(pc, k.substr(5));
    } else if (k != "terrain" && k != "cube" && k != "sphere" && k != "plane") {
        // An unknown prefix: the runtime logs and skips it; the packager warns.
        pc.report.warnings.push_back(where + ": meshKey '" + k +
                                     "' has no file / known loader prefix (skipped)");
    }

    const char* slots[] = {"albedoTex", "mrTex", "aoTex", "emissiveTex"};
    const core::Json* mat = data.Get("material");
    if (mat) {
        if (!mat->IsObject()) {
            pc.report.errors.push_back(where + ": mesh material must be a JSON object");
            return;
        }
        for (const char* slot : slots) {
            if (const core::Json* t = mat->Get(slot))
                if (t->IsString() && !t->GetString().empty()) CheckAssetRef(pc, t->GetString());
        }
    }
    // The mesh component also accepts the texture slots at the top level.
    for (const char* slot : slots) {
        if (const core::Json* t = data.Get(slot))
            if (t->IsString() && !t->GetString().empty()) CheckAssetRef(pc, t->GetString());
    }
}

void ValidateScript(ProjectContext& pc, const std::string& where, const core::Json& data) {
    if (!data.IsObject()) {
        pc.report.errors.push_back(where + ": script component must be a JSON object");
        return;
    }
    const core::Json* path = data.Get("path");
    if (!path || !path->IsString() || path->GetString().empty()) return; // inert component
    const std::string& rel = path->GetString();
    if (!FileExists(pc.projectDir + "/" + rel))
        pc.report.errors.push_back(where + ": script '" + rel + "' not found");
    const core::Json* backend = data.Get("backend");
    const std::string bk = (backend && backend->IsString()) ? backend->GetString() : "lua";
    if (bk != "lua")
        pc.report.warnings.push_back(where + ": script backend '" + bk +
                                     "' is not syntax-checked (only lua)");
}

void ValidateBehaviorTree(ProjectContext& pc, const std::string& where,
                          const core::Json& data) {
    if (!data.IsObject()) {
        pc.report.errors.push_back(where + ": behaviorTree component must be a JSON object");
        return;
    }
    const core::Json* tree = data.Get("tree");
    if (!tree || !tree->IsString() || tree->GetString().empty()) {
        pc.report.errors.push_back(where + ": behaviorTree requires a non-empty 'tree' string");
        return;
    }
    const std::string& t = tree->GetString();
    if (t.compare(0, 3, "bt:") == 0) {
        const std::string name = t.substr(3);
        const std::string file = pc.projectDir + "/behaviors/" + name + ".bt.json";
        if (!FileExists(file)) {
            pc.report.errors.push_back(where + ": behavior tree '" + t +
                                       "' not found (behaviors/" + name + ".bt.json missing)");
            return;
        }
        std::string text;
        if (!ReadFileText(file, text)) {
            pc.report.errors.push_back(where + ": cannot read behavior tree '" + file + "'");
            return;
        }
        bt::BehaviorTree bt;
        std::string err;
        if (!bt.LoadText(text, &err))
            pc.report.errors.push_back(where + ": behavior tree '" + t +
                                       "' failed to parse: " + err);
    } else {
        bt::BehaviorTree bt;
        std::string err;
        if (!bt.LoadText(t, &err))
            pc.report.errors.push_back(where + ": behavior tree failed to parse: " + err);
    }
}

void ValidateScene(ProjectContext& pc, const std::string& absPath, const std::string& virtualPath) {
    std::string text;
    if (!ReadFileText(absPath, text)) {
        pc.report.errors.push_back("cannot read scene '" + virtualPath + "'");
        return;
    }
    auto res = scene::SceneFile::Parse(text);
    if (!res.Ok()) {
        pc.report.errors.push_back("scene '" + virtualPath + "': " + res.Error());
        return;
    }
    const scene::SceneFile& sf = res.Value();
    for (const scene::EntityDef& e : sf.entities) {
        const std::string where = "scene '" + virtualPath + "' entity '" + e.name + "'";
        if (!e.prefab.empty()) {
            const std::string prefabFile = pc.projectDir + "/prefabs/" + e.prefab + ".json";
            if (!FileExists(prefabFile))
                pc.report.errors.push_back(where + ": prefab '" + e.prefab +
                                           "' not found (prefabs/" + e.prefab + ".json missing)");
        }
        for (const scene::ComponentDef& c : e.components) {
            if (c.name == "mesh")
                ValidateMesh(pc, where, c.data);
            else if (c.name == "script")
                ValidateScript(pc, where, c.data);
            else if (c.name == "behaviorTree")
                ValidateBehaviorTree(pc, where, c.data);
        }
    }
}

void ValidateInto(const PackConfig& cfg, ProjectContext& pc) {
    pc.projectDir = cfg.projectDir.empty() ? "." : cfg.projectDir;
    pc.syntaxCheck = cfg.checkScriptSyntax;
    PackageReport& r = pc.report;

    // 1. Manifest (game.json) + startScene reference.
    const std::string manifestPath = pc.projectDir + "/game.json";
    if (!FileExists(manifestPath)) {
        r.errors.push_back("game.json not found in '" + pc.projectDir + "'");
    } else {
        std::string text;
        if (!ReadFileText(manifestPath, text)) {
            r.errors.push_back("cannot read '" + manifestPath + "'");
        } else {
            auto mres = scene::GameManifest::Load(text);
            if (!mres.Ok()) {
                r.errors.push_back("game.json: " + mres.Error());
            } else {
                const scene::GameManifest& m = mres.Value();
                pc.manifestText = core::JsonWriter::Write(m.ToJson());
                pc.manifestOk = true;
                const std::string startAbs = pc.projectDir + "/" + m.startScene;
                if (!FileExists(startAbs)) {
                    r.errors.push_back("startScene '" + m.startScene + "' not found");
                } else {
                    pc.packFiles[VirtualPathOf(m.startScene)] = startAbs;
                }
            }
        }
    }

    // 2. Scenes: parse every scenes/*.json and pull out its asset references.
    std::vector<std::string> scenes;
    ListFilesRecursive(pc.projectDir + "/scenes", "scenes", scenes);
    for (const std::string& rel : scenes) {
        if (!HasExt(rel, ".json")) continue;
        pc.packFiles[rel] = pc.projectDir + "/" + rel;
        ValidateScene(pc, pc.projectDir + "/" + rel, rel);
    }
    if (scenes.empty())
        r.warnings.push_back("no files under scenes/ (startScene may still reference one)");

    // 3. Prefabs: parse every prefabs/*.json.
    std::vector<std::string> prefabs;
    ListFilesRecursive(pc.projectDir + "/prefabs", "prefabs", prefabs);
    for (const std::string& rel : prefabs) {
        if (!HasExt(rel, ".json")) continue;
        std::string text;
        if (!ReadFileText(pc.projectDir + "/" + rel, text)) {
            r.errors.push_back("cannot read prefab '" + rel + "'");
            continue;
        }
        scene::PrefabLibrary lib;
        core::Status st = lib.Add(FileStem(rel), text);
        if (!st.Ok()) r.errors.push_back("prefab '" + rel + "': " + st.Error());
        pc.packFiles[rel] = pc.projectDir + "/" + rel;
    }

    // 4. Behavior trees: parse every behaviors/*.bt.json.
    std::vector<std::string> behaviors;
    ListFilesRecursive(pc.projectDir + "/behaviors", "behaviors", behaviors);
    for (const std::string& rel : behaviors) {
        if (!HasExt(rel, ".bt.json")) continue;
        std::string text;
        if (!ReadFileText(pc.projectDir + "/" + rel, text)) {
            r.errors.push_back("cannot read behavior tree '" + rel + "'");
            continue;
        }
        bt::BehaviorTree bt;
        std::string err;
        if (!bt.LoadText(text, &err))
            r.errors.push_back("behavior tree '" + rel + "' failed to parse: " + err);
        pc.packFiles[rel] = pc.projectDir + "/" + rel;
    }

    // 5. Scripts: enumerate scripts/*.lua (recursive, project-relative) and
    // syntax-check every one when enabled. A scene-referenced script is always
    // inside scripts/, so checking the whole directory covers all references.
    std::vector<std::string> scripts;
    ListLuaFiles(pc.projectDir + "/scripts", "scripts", scripts);
    for (const std::string& rel : scripts) pc.packFiles[rel] = pc.projectDir + "/" + rel;
    if (pc.syntaxCheck && !scripts.empty()) {
        auto host = script::CreateLuaHost();
        if (!host || !host->Init()) {
            r.warnings.push_back("script host unavailable; skipping Lua syntax checks");
        } else {
            for (const std::string& rel : scripts) {
                ScriptCheckResult res = CheckScriptFile(*host, pc.projectDir, rel);
                if (!res.ok) {
                    char line[32];
                    std::snprintf(line, sizeof(line), "%d", res.line);
                    r.errors.push_back("script '" + rel + "' syntax error (line " + line +
                                       "): " + res.message);
                }
            }
            host->Shutdown();
        }
    }

    // 6. Assets: pack the whole project-local assets/ directory.
    std::vector<std::string> assets;
    ListFilesRecursive(pc.projectDir + "/assets", "assets", assets);
    for (const std::string& rel : assets) pc.packFiles[rel] = pc.projectDir + "/" + rel;

    if (pc.manifestOk) pc.packFiles["game.json"] = manifestPath;

    r.ok = r.errors.empty();
}

std::vector<std::string> SortedKeys(const std::map<std::string, std::string>& m) {
    std::vector<std::string> out;
    out.reserve(m.size());
    for (const auto& kv : m) out.push_back(kv.first);
    return out;
}

const char kRunBat[] =
    "@echo off\r\n"
    "rem NeonEngine packaged game launcher (Windows).\r\n"
    "rem NOTE: neon_game.exe is currently the neon_rush demo stand-in and does\r\n"
    "rem NOT read game.pack yet; T4.7 ships the real data-driven player.\r\n"
    "cd /d \"%~dp0\"\r\n"
    "neon_game.exe --pack game.pack\r\n";

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PackageReport ValidateProject(const PackConfig& cfg) {
    ProjectContext pc;
    ValidateInto(cfg, pc);
    PackageReport r = std::move(pc.report);
    r.files = SortedKeys(pc.packFiles);
    r.fileCount = r.files.size();
    r.ok = r.errors.empty();
    return r;
}

PackageReport PackProject(const PackConfig& cfg) {
    ProjectContext pc;
    ValidateInto(cfg, pc);
    PackageReport r = std::move(pc.report);
    r.files = SortedKeys(pc.packFiles);
    r.fileCount = r.files.size();
    if (!r.errors.empty()) {
        r.ok = false;
        return r;
    }
    if (cfg.outDir.empty()) {
        r.errors.push_back("output directory is empty");
        r.ok = false;
        return r;
    }
    if (!MakeDirs(cfg.outDir)) {
        r.errors.push_back("cannot create output directory '" + cfg.outDir + "'");
        r.ok = false;
        return r;
    }

    // Build the pack: game.json (normalized manifest) + every collected file.
    core::PackWriter writer;
    for (const auto& kv : pc.packFiles) {
        if (kv.first == "game.json" && pc.manifestOk) {
            const std::string& text = pc.manifestText;
            core::Status st = writer.AddFile(kv.first,
                                             std::vector<uint8_t>(text.begin(), text.end()));
            if (!st.Ok()) {
                r.errors.push_back("pack: " + st.Error());
                r.ok = false;
                return r;
            }
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(kv.second, bytes)) {
            r.errors.push_back("cannot read file to pack: '" + kv.first + "'");
            r.ok = false;
            return r;
        }
        core::Status st = writer.AddFile(kv.first, bytes);
        if (!st.Ok()) {
            r.errors.push_back("pack: " + st.Error());
            r.ok = false;
            return r;
        }
    }
    std::vector<uint8_t> packBytes = writer.Build();
    r.bytesWritten = packBytes.size();

    r.packPath = cfg.outDir + "/game.pack";
    if (!WriteFileBytes(r.packPath, packBytes)) {
        r.errors.push_back("cannot write '" + r.packPath + "'");
        r.ok = false;
        return r;
    }

    r.runScriptPath = cfg.outDir + "/run.bat";
    if (!WriteFileBytes(r.runScriptPath,
                        std::vector<uint8_t>(kRunBat, kRunBat + sizeof(kRunBat) - 1))) {
        r.errors.push_back("cannot write '" + r.runScriptPath + "'");
        r.ok = false;
        return r;
    }

    if (cfg.copyPlayer) {
        const std::string src =
            cfg.playerSource.empty() ? std::string("build/neon_rush.exe") : cfg.playerSource;
        if (!FileExists(src)) {
            r.warnings.push_back("player not copied: '" + src +
                                 "' missing (T4.7 builds the real neon_game.exe; until then the "
                                 "run script needs it)");
        } else {
            std::vector<uint8_t> exeBytes;
            if (!ReadFileBytes(src, exeBytes)) {
                r.warnings.push_back("player not copied: cannot read '" + src + "'");
            } else if (!WriteFileBytes(cfg.outDir + "/neon_game.exe", exeBytes)) {
                r.warnings.push_back("player not copied: cannot write '" + cfg.outDir +
                                     "/neon_game.exe'");
            } else {
                r.playerPath = cfg.outDir + "/neon_game.exe";
            }
        }
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace neon::editor::pack
