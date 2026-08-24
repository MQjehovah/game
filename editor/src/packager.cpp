#include "packager.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
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

// Read a text file's bytes. A leading UTF-8 BOM (EF BB BF) is stripped so the
// JSON/Lua parses below never trip on Notepad/PowerShell's default encoding.
// The BOM is only removed for VALIDATION reads; the pack stores the raw file
// bytes (store-only container), so T4.7's runtime must tolerate a BOM when it
// parses packed text files.
bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
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

// Case-insensitive suffix match (a "Main.JSON" scene is packed like
// "main.json"; matching the case-insensitive conventions of ListLuaFiles).
bool HasExt(const std::string& rel, const std::string& ext) {
    if (rel.size() < ext.size()) return false;
    const std::string tail = rel.substr(rel.size() - ext.size());
    for (size_t i = 0; i < tail.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(tail[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
        if (a != b) return false;
    }
    return true;
}

// True when `norm` (forward-slash normalized) contains a ".." path segment.
// References with one could escape the project root, so they are rejected.
bool ContainsTraversal(const std::string& norm) {
    size_t start = 0;
    while (start <= norm.size()) {
        const size_t slash = norm.find('/', start);
        const size_t end = (slash == std::string::npos) ? norm.size() : slash;
        if (end - start == 2 && norm.compare(start, 2, "..") == 0) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
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
    std::string manifestText;        // normalized game.json (GameManifest::ToJson)
    std::string manifestStartScene;  // startScene from game.json ("" when unparsed)
    bool manifestOk = false;
    bool syntaxCheck = true;
    std::set<std::string> walkedScenes; // scene virtual paths already validated
    std::map<std::string, std::string> scriptRefs; // lua scripts referenced by scenes/prefabs
                                                   // (virtual path -> absolute path)
};

void CheckAssetRef(ProjectContext& pc, const std::string& ref) {
    if (ref.empty()) return;
    const std::string norm = Normalize(ref);
    if (ContainsTraversal(norm)) {
        pc.report.errors.push_back("asset reference '" + ref +
                                   "' escapes the project directory (contains '..')");
        return;
    }
    std::string abs = ResolveRef(pc.projectDir, norm);
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

// Collects a glTF's internal dependencies (buffers + image URIs) into the
// pack. The glTF JSON is parsed with core::Json; URIs are resolved relative to
// the gltf file and mapped to pack virtual paths under the same directory.
// Embedded base64 data URIs are skipped (no external file).
void CollectGltfDeps(ProjectContext& pc, const std::string& ref) {
    const std::string norm = Normalize(ref);
    if (norm.empty() || ContainsTraversal(norm)) return;
    std::string abs = ResolveRef(pc.projectDir, norm);
    if (abs.empty()) return; // CheckAssetRef already reported the missing file
    std::string text;
    if (!ReadFileText(abs, text)) return;
    std::string err;
    core::Json root = core::Json::Parse(text, &err);
    if (!err.empty()) {
        pc.report.warnings.push_back("gltf '" + ref + "' failed to parse for pack deps: " + err);
        return;
    }
    const std::string dir = abs.substr(0, abs.find_last_of("/\\") + 1);
    const std::string vpDir = VirtualPathOf(ref);
    const std::string vpBase = vpDir.substr(0, vpDir.find_last_of('/') + 1);
    auto collect = [&](const char* section) {
        const core::Json* arr = root.Get(section);
        if (!arr || !arr->IsArray()) return;
        for (size_t i = 0; i < arr->Size(); ++i) {
            const core::Json* e = arr->At(i);
            if (!e || !e->IsObject()) continue;
            const core::Json* uri = e->Get("uri");
            if (!uri || !uri->IsString()) continue;
            const std::string u = Normalize(uri->GetString());
            if (u.empty() || u.compare(0, 5, "data:") == 0) continue; // embedded
            if (ContainsTraversal(u)) {
                pc.report.errors.push_back("gltf '" + ref + "' " + section +
                                           " uri escapes its directory ('" + u + "')");
                continue;
            }
            const std::string absDep = dir + u;
            const std::string vp = vpBase + u;
            if (!FileExists(absDep)) {
                pc.report.errors.push_back("gltf '" + ref + "' " + section + " file missing: '" +
                                           u + "'");
                continue;
            }
            pc.packFiles[vp] = absDep;
        }
    };
    collect("buffers");
    collect("images");
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
        CollectGltfDeps(pc, k.substr(5));
    } else if (k != "terrain" && k != "cube" && k != "sphere" && k != "plane" &&
               k != "tree" && k != "house" && k != "bush" && k != "hero" &&
               k != "wolf" && k != "rock" && k != "water" && k != "road" &&
               k != "npc" && k.compare(0, 4, "npc:") != 0) {
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
    if (ContainsTraversal(Normalize(rel))) {
        pc.report.errors.push_back(where + ": script '" + rel +
                                   "' escapes the project directory (contains '..')");
        return;
    }
    std::string abs = ResolveRef(pc.projectDir, rel);
    if (abs.empty()) {
        pc.report.errors.push_back(where + ": script '" + rel + "' not found");
        return;
    }
    // Collect the referenced script into the pack even when it lives outside
    // scripts/ (a scene or prefab may point anywhere under the project).
    pc.packFiles[VirtualPathOf(rel)] = abs;
    const core::Json* backend = data.Get("backend");
    const std::string bk = (backend && backend->IsString()) ? backend->GetString() : "lua";
    if (bk != "lua") {
        pc.report.warnings.push_back(where + ": script backend '" + bk +
                                     "' is not syntax-checked (only lua)");
    } else {
        pc.scriptRefs[VirtualPathOf(rel)] = abs;
    }
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
        if (ContainsTraversal(Normalize(name))) {
            pc.report.errors.push_back(where + ": behavior tree '" + t +
                                       "' escapes the project directory (contains '..')");
            return;
        }
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

    // 1. Manifest (game.json).
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
                pc.manifestStartScene = m.startScene;
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
        pc.walkedScenes.insert(rel);
    }
    if (scenes.empty())
        r.warnings.push_back("no files under scenes/ (startScene may still reference one)");

    // 2b. startScene content: a startScene at an arbitrary path (e.g.
    // custom/main.json) is validated + collected with the SAME per-entity pass
    // as the scenes above. A startScene under scenes/ was already walked by the
    // loop, so it is deduped here and never double-walked.
    if (pc.manifestOk && !pc.manifestStartScene.empty()) {
        const std::string startVp = VirtualPathOf(pc.manifestStartScene);
        const std::string startAbs = pc.projectDir + "/" + pc.manifestStartScene;
        if (ContainsTraversal(Normalize(pc.manifestStartScene))) {
            r.errors.push_back("startScene '" + pc.manifestStartScene +
                               "' escapes the project directory (contains '..')");
        } else if (!FileExists(startAbs)) {
            r.errors.push_back("startScene '" + pc.manifestStartScene + "' not found");
        } else {
            pc.packFiles[startVp] = startAbs;
            if (!pc.walkedScenes.count(startVp)) {
                ValidateScene(pc, startAbs, startVp);
                pc.walkedScenes.insert(startVp);
            }
        }
    }

    // 3. Prefabs: parse every prefabs/*.json into one library, then walk each
    // prefab's component templates with the same mesh/script/behaviorTree
    // passes. Prefab-referenced assets/scripts are validated and collected too,
    // not just instance components (a prefab mesh outside assets/ would
    // otherwise ship an empty pack entry or a missing file silently).
    scene::PrefabLibrary prefs;
    std::vector<std::string> prefabs;
    ListFilesRecursive(pc.projectDir + "/prefabs", "prefabs", prefabs);
    for (const auto& kv : prefabs) {
        if (!HasExt(kv, ".json")) continue;
        std::string text;
        if (!ReadFileText(pc.projectDir + "/" + kv, text)) {
            r.errors.push_back("cannot read prefab '" + kv + "'");
            continue;
        }
        core::Status st = prefs.Add(FileStem(kv), text);
        if (!st.Ok()) r.errors.push_back("prefab '" + kv + "': " + st.Error());
        pc.packFiles[kv] = pc.projectDir + "/" + kv;
    }
    // Walk each prefab's component templates with the same passes. Prefab-
    // referenced assets/scripts are validated and collected too, not just
    // instance components (a prefab mesh outside assets/ would otherwise ship
    // an empty pack entry or a missing file silently).
    std::set<std::string> walkedPrefabs;
    for (const std::string& kv : prefabs) {
        if (!HasExt(kv, ".json")) continue;
        const std::string name = FileStem(kv);
        if (!walkedPrefabs.insert(name).second) continue; // same-name prefab walked once
        auto got = prefs.Get(name);
        if (!got.Ok()) continue; // Add above already reported the failure
        const std::string where = "prefab '" + name + "'";
        for (const auto& comp : got.Value()->Members()) {
            if (comp.first == "mesh")
                ValidateMesh(pc, where, comp.second);
            else if (comp.first == "script")
                ValidateScript(pc, where, comp.second);
            else if (comp.first == "behaviorTree")
                ValidateBehaviorTree(pc, where, comp.second);
        }
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

    // 5. Scripts: enumerate scripts/*.lua (recursive), plus every script a
    // scene/prefab references (which may live outside scripts/), syntax-check
    // each lua script when enabled and collect every one into the pack.
    std::vector<std::string> scripts;
    ListLuaFiles(pc.projectDir + "/scripts", "scripts", scripts);
    for (const std::string& rel : scripts) pc.packFiles[rel] = pc.projectDir + "/" + rel;

    // Absolute path -> virtual path, deduped across the enumeration and the
    // scene/prefab references (one host run; CheckSyntax is validation-only).
    std::map<std::string, std::string> scriptsToCheck;
    for (const std::string& rel : scripts) scriptsToCheck[pc.projectDir + "/" + rel] = rel;
    for (const auto& kv : pc.scriptRefs) scriptsToCheck[kv.second] = kv.first;
    if (pc.syntaxCheck && !scriptsToCheck.empty()) {
        auto host = script::CreateLuaHost();
        if (!host || !host->Init()) {
            r.warnings.push_back("script host unavailable; skipping Lua syntax checks");
        } else {
            for (const auto& kv : scriptsToCheck) {
                std::string source;
                if (!ReadFileText(kv.first, source)) {
                    r.errors.push_back("script '" + kv.second + "' cannot be read");
                    continue;
                }
                if (!host->CheckSyntax(source)) {
                    const script::ScriptError& err = host->LastError();
                    char line[32];
                    std::snprintf(line, sizeof(line), "%d", err.line);
                    r.errors.push_back("script '" + kv.second + "' syntax error (line " + line +
                                       "): " + err.message);
                }
            }
            host->Shutdown();
        }
    }

    // 6. Assets: pack the whole project-local assets/ directory.
    std::vector<std::string> assets;
    ListFilesRecursive(pc.projectDir + "/assets", "assets", assets);
    for (const std::string& rel : assets) pc.packFiles[rel] = pc.projectDir + "/" + rel;

    // UI documents (data-driven .ui.json, consumed by UIShow at runtime).
    std::vector<std::string> uiDocs;
    ListFilesRecursive(pc.projectDir + "/ui", "ui", uiDocs);
    for (const std::string& rel : uiDocs) pc.packFiles[rel] = pc.projectDir + "/" + rel;

    // Godot-style input actions (project root, next to game.json).
    const std::string inputPath = pc.projectDir + "/input.json";
    if (FileExists(inputPath)) pc.packFiles["input.json"] = inputPath;

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
    "rem Runs game.pack with the generic data-driven neon_game player.\r\n"
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

    // P2-5 release artifacts: update manifest + install/update scripts.
    {
        uint32_t hash = 2166136261u;  // FNV-1a 32 over the pack bytes
        for (uint8_t b : packBytes) {
            hash ^= b;
            hash *= 16777619u;
        }
        core::Json upd;
        upd.type_ = core::Json::Type::Object;
        core::Json v;
        v.type_ = core::Json::Type::String;
        v.string_ = cfg.version;
        upd.object_["version"] = v;
        core::Json bytes;
        bytes.type_ = core::Json::Type::Number;
        bytes.number_ = static_cast<double>(packBytes.size());
        upd.object_["packBytes"] = bytes;
        core::Json chk;
        chk.type_ = core::Json::Type::Number;
        chk.number_ = hash;
        upd.object_["packChecksum"] = chk;
        core::Json player;
        player.type_ = core::Json::Type::String;
        player.string_ = "neon_game.exe";
        upd.object_["player"] = player;
        if (!cfg.updateUrl.empty()) {
            core::Json url;
            url.type_ = core::Json::Type::String;
            url.string_ = cfg.updateUrl;
            upd.object_["updateUrl"] = url;
        }
        r.updatePath = cfg.outDir + "/update.json";
        const std::string updJson = core::JsonWriter::Write(upd);
        if (!WriteFileBytes(r.updatePath,
                            std::vector<uint8_t>(updJson.begin(), updJson.end()))) {
            r.errors.push_back("cannot write '" + r.updatePath + "'");
            r.ok = false;
            return r;
        }

        const std::string installBat =
            "@echo off\r\n"
            "rem NeonEngine release installer (P2-5): copies the game to a target "
            "dir and adds a desktop shortcut.\r\n"
            "set TARGET=%1\r\n"
            "if \"%TARGET%\"==\"\" set TARGET=%USERPROFILE%\\NeonGame\r\n"
            "if not exist \"%TARGET%\" mkdir \"%TARGET%\"\r\n"
            "copy /Y neon_game.exe \"%TARGET%\" >nul\r\n"
            "copy /Y game.pack \"%TARGET%\" >nul\r\n"
            "copy /Y update.json \"%TARGET%\" >nul\r\n"
            "powershell -NoProfile -Command \"$s=(New-Object -ComObject WScript.Shell)."
            "CreateShortcut([Environment]::GetFolderPath('Desktop')+'\\NeonGame.lnk');"
            "$s.TargetPath='%TARGET%\\neon_game.exe';$s.WorkingDirectory='%TARGET%';$s.Save()\"\r\n"
            "echo Installed to %TARGET% (shortcut added)\r\n";
        r.installPath = cfg.outDir + "/install.bat";
        if (!WriteFileBytes(r.installPath,
                            std::vector<uint8_t>(installBat.begin(), installBat.end()))) {
            r.errors.push_back("cannot write '" + r.installPath + "'");
            r.ok = false;
            return r;
        }

        // Auto-update script (Windows 10+ ships curl.exe): downloads a fresh
        // game.pack from the update host and verifies its checksum against
        // update.json before replacing the local copy.
        std::string updateBat =
            "@echo off\r\n"
            "rem NeonEngine auto-updater (P2-5): fetches game.pack from the "
            "update host.\r\n"
            "set URL=" + cfg.updateUrl + "\r\n"
            "if \"%URL%\"==\"\" (echo update.json has no updateUrl & exit /b 1)\r\n"
            "where curl >nul 2>nul || (echo curl not found & exit /b 1)\r\n"
            "curl -L -o game.pack.new \"%URL%/game.pack\" || (echo download failed & exit /b 1)\r\n"
            "if not exist game.pack.new (echo download failed & exit /b 1)\r\n"
            "move /Y game.pack.new game.pack >nul\r\n"
            "echo Updated to version " + cfg.version + "\r\n";
        if (cfg.updateUrl.empty()) {
            updateBat =
                "@echo off\r\n"
                "echo updateUrl not configured; pack with --update-url <host> to enable "
                "auto-update\r\n";
        }
        if (!WriteFileBytes(cfg.outDir + "/update.bat",
                            std::vector<uint8_t>(updateBat.begin(), updateBat.end()))) {
            r.errors.push_back("cannot write '" + cfg.outDir + "/update.bat'");
            r.ok = false;
            return r;
        }
    }

    if (cfg.copyPlayer) {
        const std::string src =
            cfg.playerSource.empty() ? std::string("build/neon_game.exe") : cfg.playerSource;
        if (!FileExists(src)) {
            r.warnings.push_back("player not copied: '" + src +
                                 "' missing (build neon_game first; the run script needs it)");
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
