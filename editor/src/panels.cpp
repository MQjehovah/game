#include "editor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <functional>
#include <cstdio>
#include <fstream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h must precede shobjidl.h: MinGW's IFileDialog needs the COM base
// types defined first, otherwise it stays an incomplete type.
#include <windows.h>
#include <direct.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#include "editor_history.hpp"
#include "imgui_internal.h"
#include "neon/gfx/imgui_neon.hpp"

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neon::editor {
std::string GetCurrentDir() {
    char buf[4096];
#if defined(_WIN32)
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    if (::getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

// Convert an asset path (absolute or project-root-relative) into a
// project-relative path ("scripts/foo.lua"); returns the input unchanged when it
// doesn't live under the project dir.
std::string ToProjectRelPath(const std::string& path, const std::string& projectDir) {
    std::string base = projectDir.empty() ? "." : projectDir;
    if (base == ".") {
        // Keep paths already relative ("scripts/..."); strip a leading "./".
        if (path.compare(0, 2, "./") == 0) return path.substr(2);
        return path;
    }
    // The project dir may itself be relative ("projects/pvz") while the asset
    // panel hands us absolute paths; resolve the project dir against the cwd
    // so absolute paths under it still convert to project-relative form.
    std::string absBase = base;
    const bool baseAbsolute = absBase.size() >= 2 && absBase[1] == ':' ||
                              (!absBase.empty() && (absBase[0] == '/' || absBase[0] == '\\'));
    if (!baseAbsolute) absBase = GetCurrentDir() + "/" + absBase;
    std::string normBase = base;
    if (normBase.back() != '/' && normBase.back() != '\\') normBase += '/';
    std::string normPath = path;
    if (normPath.rfind(normBase, 0) == 0) return normPath.substr(normBase.size());
    std::string normAbsBase = absBase;
    if (normAbsBase.back() != '/' && normAbsBase.back() != '\\') normAbsBase += '/';
    if (normPath.rfind(normAbsBase, 0) == 0) return normPath.substr(normAbsBase.size());
    // Also match with a leading "./".
    std::string dotBase = "./" + base;
    if (normPath.rfind(dotBase, 0) == 0)
        return normPath.substr(dotBase.size() + 1); // skip "./" + base + "/"
    return path;
}


namespace {

std::string TypeLabel(const std::string& key) {
    if (key.empty()) return "实体";
    if (key == "terrain") return "地形";
    if (key == "helmet") return "头盔 (glTF PBR)";
    if (key == "cube") return "方块";
    if (key == "sphere") return "球体";
    if (key == "plane") return "平面";
    if (key == "hero") return "英雄";
    if (key == "wolf") return "狼";
    if (key == "npc" || key.compare(0, 4, "npc:") == 0) return "村民";
    if (key == "house") return "房屋";
    if (key == "bush") return "灌木";
    if (key == "rock") return "岩石";
    if (key == "water") return "水面";
    if (key == "road") return "道路";
    if (key == "tree") return "松树 (OBJ)";
    if (key.rfind("obj:", 0) == 0) return "OBJ 模型";
    if (key.rfind("gltf:", 0) == 0) return "glTF 模型";
    return key;
}

// Unity-like entity type: what the selected object IS, derived from its
// components / mesh kind (plant/zombie from the 2D canvas, sprite, prefab,
// or the mesh type).
std::string EntityTypeLabel(const SceneEntity& e) {
    if (!e.nodeType.empty()) return e.nodeType;
    if (e.extraComponents.count("plant")) return "植物";
    if (e.extraComponents.count("zombie")) return "僵尸";
    if (!e.spriteTex.empty()) return "精灵";
    if (!e.prefab.empty()) return "预制体: " + e.prefab;
    return TypeLabel(e.meshKey);
}

// Node type table (P1-1): the combo list for the inspector.
const char* kNodeTypes[] = {"Node", "MeshInstance3D", "Camera3D",
                            "CharacterBody", "Sprite", "Light3D"};

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

gfx::Color ColorFromHex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return gfx::Color::White;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        return 255u;
    };
    auto byte = [&](char hi, char lo) {
        return static_cast<float>(((nibble(hi) << 4) | nibble(lo)) / 255.0);
    };
    return {byte(hex[1], hex[2]), byte(hex[3], hex[4]), byte(hex[5], hex[6]), 1.0f};
}

bool MakeDirSingle(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

std::string UiFileBaseName(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool CopyFileBinary(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) return false;
    out << in.rdbuf();
    return true;
}

bool IsDirPath(const std::string& p) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0) return false;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

// Picks a collision-free file name in `dir` ("base.ext" -> "base_1.ext"...).
std::string UniqueNameIn(const std::string& dir, const std::string& base) {
    std::string name = base;
    const size_t dot = base.find_last_of('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : base.substr(dot);
    int counter = 1;
    while (std::ifstream(dir + "/" + name, std::ios::binary).is_open())
        name = stem + "_" + std::to_string(counter++) + ext;
    return name;
}

// Recursively copies a source directory tree into `dst` (created on demand).
// Files keep their relative layout; name collisions get _N suffixes.
bool CopyDirRecursive(const std::string& src, const std::string& dst) {
    MakeDirSingle(dst);
    std::vector<AssetEntry> entries;
    if (!ListDirectory(src, entries)) return false;
    for (const AssetEntry& e : entries) {
        if (e.isDir) {
            if (!CopyDirRecursive(e.path, dst + "/" + e.name)) return false;
        } else {
            const std::string name = UniqueNameIn(dst, e.name);
            if (!CopyFileBinary(e.path, dst + "/" + name)) return false;
        }
    }
    return true;
}

// Native open-file dialog for the asset panel's 导入 action. Returns an empty
// string when cancelled. Non-Windows hosts fall back to the path input row.
std::string PickImportFile() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter =
        "所有文件 (*.*)\0*.*\0"
        "图片 (*.png;*.jpg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
        "模型 (*.obj;*.gltf)\0*.obj;*.gltf\0"
        "脚本 (*.lua;*.js)\0*.lua;*.js\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn)) return buf;
#else
    (void)0;
#endif
    return {};
}

std::string ParentPath(const std::string& p);
std::string FileName(const std::string& p);
std::string FileStem(const std::string& p);
std::string FileExt(const std::string& p);

bool IsImageExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

bool IsModelExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".obj" || ext == ".gltf";
}

bool IsScriptExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".lua" || ext == ".js";
}

// Material ball asset: materials/*.mat.json.
bool IsMaterialExt(const std::string& name) {
    const std::string lower = ToLower(name);
    // ".mat.json" is NINE characters; comparing 8 made every material ball
    // fail the asset filter (no grid tile, no thumbnail preview).
    return lower.size() > 9 && lower.compare(lower.size() - 9, 9, ".mat.json") == 0;
}

// Asset listing filter: 0 all, 1 models, 2 textures, 3 scripts.
bool AssetMatchesFilter(const AssetEntry& e, int filter) {
    if (e.isDir || filter == 0) return true;
    if (filter == 1) return IsModelExt(e.name);
    if (filter == 2) return IsImageExt(e.name);
    if (filter == 3) return IsScriptExt(e.name);
    if (filter == 4) return IsMaterialExt(e.name);
    return true;
}


std::string ParentPath(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return p;
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

std::string FileName(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string FileStem(const std::string& p) {
    std::string name = FileName(p);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string FileExt(const std::string& p) {
    std::string name = FileName(p);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : name.substr(dot);
}

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr,
                        nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}
#endif

} // namespace

// Deletes a file or a whole directory tree. Windows moves it to the recycle
// bin (recoverable); POSIX removes it recursively (caller confirms first).
bool DeletePathRecursive(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    std::wstring w = Utf8ToWide(path);
    // SHFileOperationW silently fails on relative paths (e.g. the asset panel
    // points at "projects/xxx/assets" after a project switch), so resolve to
    // an absolute path first.
    wchar_t absBuf[MAX_PATH];
    const DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH, absBuf, nullptr);
    if (n > 0 && n < MAX_PATH) w = absBuf;
    std::vector<wchar_t> buf(w.begin(), w.end());
    buf.push_back(0);
    buf.push_back(0); // SHFileOperation requires a double-null-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
#else
    if (IsDirPath(path)) {
        std::vector<AssetEntry> entries;
        if (ListDirectory(path, entries)) {
            for (const AssetEntry& e : entries) {
                if (!DeletePathRecursive(e.path)) return false;
            }
        }
        return ::rmdir(path.c_str()) == 0;
    }
    return ::remove(path.c_str()) == 0;
#endif
}

// Native folder picker for importing a whole resource directory (model +
// textures + subfolders). Non-Windows hosts fall back to the path input row.
std::string PickImportDir() {
#if defined(_WIN32)
    std::string out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // MinGW-w64's shobjidl.h only forward-declares IFileDialog, so use the
    // classic SHBrowseForFolderW folder picker instead (same UX, compiles).
    BROWSEINFOW bi = {};
    bi.hwndOwner = nullptr;
    bi.lpszTitle = L"选择要导入的资源目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            out = WideToUtf8(path);
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return out;
#else
    return {};
#endif
}

bool ListDirectory(const std::string& dir, std::vector<AssetEntry>& out) {
#if defined(_WIN32)
    std::wstring pattern = Utf8ToWide(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        AssetEntry e;
        e.name = WideToUtf8(name);
        e.path = dir + "/" + e.name;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.isDir) {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            e.size = sz.QuadPart;
        }
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        AssetEntry e;
        e.name = name;
        e.path = path;
        e.isDir = S_ISDIR(st.st_mode);
        if (!e.isDir) e.size = static_cast<uint64_t>(st.st_size);
        out.push_back(std::move(e));
    }
    closedir(d);
    return true;
#endif
}

void EditorApp::InitToolPanels() {
    assetDir_ = GetCurrentDir();
    RefreshAssetDir();
}

void EditorApp::RefreshAssetDir() {
    assetEntries_.clear();
    selectedAsset_ = -1;
    ListDirectory(assetDir_, assetEntries_);
    assetDirSignature_.clear();
    for (const AssetEntry& a : assetEntries_) {
        assetDirSignature_ += a.name;
        assetDirSignature_ += a.isDir ? "/" : "|";
    }
    std::sort(assetEntries_.begin(), assetEntries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir;
                  return ToLower(a.name) < ToLower(b.name);
              });
    NEON_LOG_DEBUG("Editor: asset dir '%s' (%zu entries)", assetDir_.c_str(),
                   assetEntries_.size());
}

// Copies a file into the current asset dir (skipping a duplicate name by
// appending _1/_2/...), then refreshes the listing.
void EditorApp::ImportAssetFile(const std::string& srcPath) {
    if (srcPath.empty()) return;
    if (IsDirPath(srcPath)) {
        // A whole resource directory (model + textures + subfolders) is
        // copied recursively into the project assets/.
        const std::string name = FileName(srcPath);
        if (CopyDirRecursive(srcPath, assetDir_ + "/" + name)) {
            NEON_LOG_INFO("Asset: imported directory '%s' -> '%s/%s'", srcPath.c_str(),
                          assetDir_.c_str(), name.c_str());
            RefreshAssetDir();
        } else {
            NEON_LOG_ERROR("Asset: failed to import directory '%s'", srcPath.c_str());
        }
        return;
    }
    const std::string base = FileName(srcPath);
    if (base.empty() || base == "." || base == "..") {
        NEON_LOG_ERROR("Asset: cannot import '%s'", srcPath.c_str());
        return;
    }
    const std::string name = UniqueNameIn(assetDir_, base);
    if (CopyFileBinary(srcPath, assetDir_ + "/" + name)) {
        NEON_LOG_INFO("Asset: imported '%s' -> '%s/%s'", srcPath.c_str(), assetDir_.c_str(),
                      name.c_str());
        RefreshAssetDir();
    } else {
        NEON_LOG_ERROR("Asset: failed to import '%s' (target '%s/%s')", srcPath.c_str(),
                       assetDir_.c_str(), name.c_str());
    }
}

// Creates a new asset in the current asset dir: kind 0 = directory, 1 = Lua
// script, 2 = JSON, 3 = empty text file, 4 = material ball, 5 = JS script.
void EditorApp::CreateAssetFile(const std::string& name, int kind) {
    if (name.empty() || name == "." || name == "..") {
        NEON_LOG_ERROR("Asset: invalid asset name '%s'", name.c_str());
        return;
    }
    const std::string path = assetDir_ + "/" + name;
    if (kind == 0) {
        if (MakeDirSingle(path)) {
            NEON_LOG_INFO("Asset: created directory '%s'", path.c_str());
            RefreshAssetDir();
        } else {
            NEON_LOG_ERROR("Asset: cannot create directory '%s'", path.c_str());
        }
        return;
    }
    if (std::ifstream(path, std::ios::binary).is_open()) {
        NEON_LOG_ERROR("Asset: '%s' already exists", path.c_str());
        return;
    }
    std::string content;
    if (kind == 1) {
        content = "-- New script (data-driven)\nfunction on_start(ent)\nend\n"
                  "function on_update(ent, dt)\nend\n";
    } else if (kind == 5) {
        content = "// New script (data-driven)\n"
                  "function on_start(ent) {\n}\n"
                  "function on_update(ent, dt) {\n}\n";
    } else if (kind == 2) {
        content = "{\n}\n";
    } else if (kind == 4) {
        // Material ball asset (Unity .mat / Godot Material style).
        content = "{\n  \"colorHex\": \"#FFFFFF\",\n  \"metallic\": 0.0,\n"
                  "  \"roughness\": 0.8,\n  \"ao\": 1.0,\n"
                  "  \"emissiveIntensity\": 1.0,\n  \"albedoTex\": \"\",\n"
                  "  \"mrTex\": \"\",\n  \"aoTex\": \"\",\n  \"emissiveTex\": \"\"\n}\n";
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Asset: cannot create '%s'", path.c_str());
        return;
    }
    out << content;
    NEON_LOG_INFO("Asset: created '%s'", path.c_str());
    RefreshAssetDir();
}

void EditorApp::ImportAssetPath(const std::string& path) {
    std::string lower = ToLower(path);
    if (lower.rfind(".obj") != std::string::npos) {
        AddEntity("obj:" + path);
        NEON_LOG_INFO("Editor: imported OBJ '%s'", path.c_str());
    } else if (lower.rfind(".gltf") != std::string::npos) {
        AddEntity("gltf:" + path);
        NEON_LOG_INFO("Editor: imported glTF '%s'", path.c_str());
    } else if (IsImageExt(path)) {
        gfx::Texture tex = assetMgr_.LoadTexture(path);
        if (tex.Valid()) {
            previewTexture_ = tex;
            if (previewTexId_ != ImTextureID_Invalid) {
                gfx::ImGuiNeon_UnregisterTexture(previewTexture_.Handle());
            }
            previewTexId_ = gfx::ImGuiNeon_RegisterTexture(previewTexture_.Handle());
            NEON_LOG_INFO("Editor: preview texture '%s' (%dx%d)", path.c_str(), tex.Width(),
                          tex.Height());
        }
    } else if (IsScriptExt(path)) {
        // Double-clicking a .lua opens the built-in script editor.
        OpenScriptEditor(path);
    } else {
        NEON_LOG_INFO("Editor: '%s' (%llu bytes) - no import action",
                      path.c_str(), static_cast<unsigned long long>(0));
    }
}

// Deletes the selected asset immediately (recycle bin on Windows, so it is
// recoverable). A modal confirm popup proved unreliable in the docked panel,
// so deletion is direct and logged.
void EditorApp::DeleteSelectedAsset() {
    if (selectedAsset_ < 0 ||
        selectedAsset_ >= static_cast<int>(assetEntries_.size())) {
        NEON_LOG_WARN("Asset: delete ignored (selected=%d entries=%zu)",
                      selectedAsset_, assetEntries_.size());
        return;
    }
    const AssetEntry& e = assetEntries_[static_cast<size_t>(selectedAsset_)];
    NEON_LOG_INFO("Asset: deleting '%s'", e.path.c_str());
    if (DeletePathRecursive(e.path)) {
        NEON_LOG_INFO("Asset: deleted '%s'", e.path.c_str());
    } else {
        NEON_LOG_ERROR("Asset: failed to delete '%s'", e.path.c_str());
    }
    RefreshAssetDir();
}

void EditorApp::ImportSelectedAsset() {
    if (selectedAsset_ >= 0 && selectedAsset_ < static_cast<int>(assetEntries_.size()) &&
        !assetEntries_[static_cast<size_t>(selectedAsset_)].isDir) {
        ImportAssetPath(assetEntries_[static_cast<size_t>(selectedAsset_)].path);
    }
}

void EditorApp::BuildScenePanel() {
    if (!showHierarchy_) return;
    if (ImGui::Begin("场景", &showHierarchy_)) {
        static int addType = 0;
        // Unity-style: only primitive geometry is created from the toolbar.
        // Helmet / tree / house etc. are resource objects and are dragged in
        // from the 资源 panel (or double-clicked there).
        const char* types[] = {"地形", "方块", "球体", "平面", "相机", "方向光", "点光源"};
        ImGui::SetNextItemWidth(86.0f);
        ImGui::Combo("##addtype", &addType, types, 7);
        ImGui::SameLine();
        if (ImGui::Button("添加")) {
            const char* keys[] = {"terrain", "cube", "sphere", "plane",
                                  "camera", "light:directional", "light:point"};
            AddEntity(keys[addType]);
        }
        ImGui::SameLine();
        ImGui::Button("?");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("模型资源(头盔/松树/房屋等)从 资源 面板双击导入或拖入");
        ImGui::SameLine();
        {
            static int prefabSel = 0;
            if (prefabLib_.Size() > 0) {
                std::vector<const char*> prefabNames;
                for (const auto& entry : projectPrefabs_) prefabNames.push_back(entry.c_str());
                if (prefabSel >= static_cast<int>(prefabNames.size())) prefabSel = 0;
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::Combo("##prefab_pick", &prefabSel, prefabNames.data(),
                                 static_cast<int>(prefabNames.size())))
                    ;
                ImGui::SameLine();
                if (ImGui::Button("插入预置体"))
                    AddEntity("prefab:" + projectPrefabs_[static_cast<size_t>(prefabSel)]);
            } else {
                ImGui::TextDisabled("无预置体");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("复制") && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                &entities_, SelectedIndices()));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("删除") && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ClampSelection();
        }
        ImGui::SameLine();
        // ↑/↓ move the selected entity within its OWN sibling group (the tree
        // groups children by parentId in global-array order, so a global ±1
        // move could silently reorder another parent's children instead).
        auto moveSibling = [this](int dir) {
            if (selection_.size() != 1 || selected_ < 0 ||
                selected_ >= static_cast<int>(entities_.size()))
                return;
            const size_t sel = static_cast<size_t>(selected_);
            const int parentId = entities_[sel].parentId;
            std::vector<size_t> sibs;
            for (size_t i = 0; i < entities_.size(); ++i)
                if (entities_[i].parentId == parentId) sibs.push_back(i);
            const auto it = std::find(sibs.begin(), sibs.end(), sel);
            if (it == sibs.end()) return;
            const size_t pos = static_cast<size_t>(it - sibs.begin());
            const size_t none = static_cast<size_t>(-1);
            const size_t to = dir < 0 ? (pos == 0 ? none : sibs[pos - 1])
                                      : (pos + 1 >= sibs.size() ? none : sibs[pos + 1]);
            if (to == none) return;
            history_.Push(std::make_unique<ReorderEntityCommand>(&entities_, sel, to));
            SetSelection(static_cast<int>(to));
        };
        ImGui::SameLine();
        if (ImGui::Button("↑")) moveSibling(-1);
        ImGui::SameLine();
        if (ImGui::Button("↓")) moveSibling(1);
        ImGui::SameLine();
        if (ImGui::Button("按名称排序")) SortSceneTreeByName();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("对场景树按名称排序（每个父级下递归、可撤销）");
        if (selection_.size() > 1)
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "已选中 %zu 个实体 (Ctrl 加选 / Shift 连选)",
                               selection_.size());
        ImGui::Separator();
        ImGui::BeginChild("##scene_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        // P2-editor UX: entity-name filter — flat filtered list replaces the
        // tree while typing (large scenes stay navigable).
        static char filterBuf[128] = {};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##scene_filter", "过滤实体名...", filterBuf,
                                 sizeof(filterBuf));
        ImGui::Separator();
        const std::string filter = ToLower(filterBuf);
        if (!filter.empty()) {
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& fe = entities_[i];
                if (ToLower(fe.name).find(filter) == std::string::npos) continue;
                char flabel[256];
                std::snprintf(flabel, sizeof(flabel), "%s##filter_%zu", fe.name.c_str(), i);
                if (ImGui::Selectable(flabel, IsSelected(static_cast<int>(i)))) {
                    if (ImGui::GetIO().KeyCtrl)
                        ToggleSelection(static_cast<int>(i));
                    else if (ImGui::GetIO().KeyShift)
                        SelectRangeTo(static_cast<int>(i));
                    else
                        SetSelection(static_cast<int>(i));
                }
            }
            ImGui::EndChild();
            // The asset drop targets below still apply to the filtered view.
        } else {
        // Godot-style scene tree: entities group under their parentId
        // (0 = root). Drag one row onto another to reparent; drag onto the
        // empty area to detach back to root. Cycle / self-parent are rejected.
            // Live ids must be unique and non-zero for the id-based tree and
            // drag guards: mid-session entities (duplicate command copies the
            // source id; asset drops start at 0) get normalized here, every
            // frame, before the tree is built.
            NormalizeEntityIds();
            std::map<int, std::vector<int>> childrenByParent;
            for (size_t i = 0; i < entities_.size(); ++i)
                childrenByParent[entities_[i].parentId].push_back(static_cast<int>(i));
            auto parentIdOf = [this](int id) {
                for (const SceneEntity& e : entities_)
                    if (e.id == id) return e.parentId;
                return 0;
            };
            auto reparent = [this, &parentIdOf](const std::vector<int>& from, int toParentId) {
                if (from.empty()) return;
                // Cycle guard: cannot parent an entity under itself or one of
                // its descendants (walk the target's ancestor chain by id).
                std::set<int> draggedIds;
                for (int i : from)
                    if (i >= 0 && i < static_cast<int>(entities_.size()))
                        draggedIds.insert(entities_[static_cast<size_t>(i)].id);
                if (draggedIds.count(toParentId) != 0) return; // self-parent
                int cur = toParentId;
                int guard = 0;
                while (cur != 0 && guard++ <= static_cast<int>(entities_.size())) {
                    if (draggedIds.count(cur) != 0) return; // descendant -> cycle
                    cur = parentIdOf(cur);
                }
                std::vector<int> valid;
                for (int i : from) {
                    if (i < 0 || i >= static_cast<int>(entities_.size())) continue;
                    if (entities_[static_cast<size_t>(i)].parentId == toParentId) continue;
                    valid.push_back(i);
                }
                if (valid.empty()) return;
                history_.Push(std::make_unique<MultiSetParentCommand>(
                    &entities_, valid, toParentId));
            };
            std::function<void(int)> drawNode = [&](int parentId) {
                const auto it = childrenByParent.find(parentId);
                if (it == childrenByParent.end()) return;
                for (int idx : it->second) {
                    const SceneEntity& e = entities_[static_cast<size_t>(idx)];
                    char label[256];
                    std::snprintf(label, sizeof(label), "%s%s##scene_%d", e.name.c_str(),
                                  e.prefab.empty() ? "" : " (预置体)", idx);
                    const bool hasChildren = childrenByParent.count(e.id) != 0;
                    const bool selected = IsSelected(idx);
                    const bool ctrl = ImGui::GetIO().KeyCtrl;
                    const bool shift = ImGui::GetIO().KeyShift;
                    // P2-editor UX: right-click context menu on any row.
                    auto contextMenu = [&]() {
                        if (ImGui::BeginPopupContextItem("scene_ctx")) {
                            if (ImGui::MenuItem("复制")) {
                                std::vector<int> sel = SelectedIndices();
                                if (sel.empty()) sel.push_back(idx);
                                history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                                    &entities_, sel));
                                SetSelection(static_cast<int>(entities_.size()) - 1);
                            }
                            if (ImGui::MenuItem("删除")) {
                                std::vector<int> sel = SelectedIndices();
                                if (sel.empty()) sel.push_back(idx);
                                history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                                    &entities_, sel));
                                ClampSelection();
                            }
                            ImGui::EndPopup();
                        }
                    };
                    if (hasChildren) {
                        const bool open = ImGui::TreeNodeEx(
                            label, ImGuiTreeNodeFlags_OpenOnArrow |
                                       (selected ? ImGuiTreeNodeFlags_Selected : 0));
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                            if (ctrl)
                                ToggleSelection(idx);
                            else if (shift)
                                SelectRangeTo(idx);
                            else
                                SetSelection(idx);
                        }
                        contextMenu();
                        // P2-editor UX: drag the whole selection to reparent.
                        // The source + target attach to THIS row BEFORE the
                        // children recurse: ImGui binds drag-drop to the LAST
                        // item, and after TreePop that is the deepest child
                        // row, so an expanded parent's own row had no source/
                        // target (multi-level reparent silently failed).
                        if (ImGui::BeginDragDropSource()) {
                            std::vector<int> drag = SelectedIndices();
                            if (drag.empty()) drag.push_back(idx);
                            dragPayload_ = drag;
                            ImGui::SetDragDropPayload("SCENE_ENTITIES",
                                                      dragPayload_.data(),
                                                      static_cast<size_t>(
                                                          dragPayload_.size()) *
                                                          sizeof(int));
                            ImGui::Text("移动 %zu 个实体", dragPayload_.size());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* p =
                                    ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                                const int* data = static_cast<const int*>(p->Data);
                                const size_t n = p->DataSize / sizeof(int);
                                reparent(std::vector<int>(data, data + n), e.id);
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (open) {
                            drawNode(e.id);
                            ImGui::TreePop();
                        }
                    } else {
                        if (ImGui::Selectable(label, selected)) {
                            if (ctrl)
                                ToggleSelection(idx);
                            else if (shift)
                                SelectRangeTo(idx);
                            else
                                SetSelection(idx);
                        }
                        contextMenu();
                        if (ImGui::BeginDragDropSource()) {
                            std::vector<int> drag = SelectedIndices();
                            if (drag.empty()) drag.push_back(idx);
                            dragPayload_ = drag;
                            ImGui::SetDragDropPayload("SCENE_ENTITIES",
                                                      dragPayload_.data(),
                                                      static_cast<size_t>(
                                                          dragPayload_.size()) *
                                                          sizeof(int));
                            ImGui::Text("移动 %zu 个实体", dragPayload_.size());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* p =
                                    ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                                const int* data = static_cast<const int*>(p->Data);
                                const size_t n = p->DataSize / sizeof(int);
                                reparent(std::vector<int>(data, data + n), e.id);
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                }
            };
            drawNode(0);
            // Detach target: drag an entity here to clear its parent.
            ImGui::TextDisabled("(拖到此处取消父子关系)");
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("SCENE_ENTITIES")) {
                    const int* data = static_cast<const int*>(p->Data);
                    const size_t n = p->DataSize / sizeof(int);
                    reparent(std::vector<int>(data, data + n), 0);
                }
                ImGui::EndDragDropTarget();
        }
        }
        ImGui::EndChild();

        // Drop targets: a model asset adds an entity, a script asset attaches to
        // the selected entity.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path) {
                    std::string lower = ToLower(std::string(path));
                    if (lower.rfind(".obj") != std::string::npos)
                        AddEntity("obj:" + std::string(path));
                    else if (lower.rfind(".gltf") != std::string::npos)
                        AddEntity("gltf:" + std::string(path));
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && *path) AddSpriteEntity(path);
            }
            if (const ImGuiPayload* p =
                    ImGui::AcceptDragDropPayload("ASSET_BUILTIN_MODEL")) {
                const char* key = static_cast<const char*>(p->Data);
                if (key && *key) AddEntity(std::string(key));
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && selected_ >= 0 &&
                    selected_ < static_cast<int>(entities_.size())) {
                    std::vector<SceneScriptFields> newList =
                        entities_[static_cast<size_t>(selected_)].scripts;
                    newList.push_back({"lua", ToProjectRelPath(path, projectDir_), {}});
                    history_.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities_, selected_, ApplyScriptList,
                        entities_[static_cast<size_t>(selected_)].scripts, newList,
                        /*mergeable=*/false));
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

void EditorApp::BuildAssetPanel() {
    if (!showAssets_) return;
    if (deleteAssetRequested_) {
        deleteAssetRequested_ = false;
        DeleteSelectedAsset();
    }
    if (ImGui::Begin("资产", &showAssets_)) {
        if (ImGui::SmallButton("刷新")) RefreshAssetDir();
        ImGui::SameLine();
        if (ImGui::SmallButton("浏览导入")) {
            const std::string picked = PickImportFile();
            if (!picked.empty()) ImportAssetFile(picked);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("浏览目录")) {
            const std::string picked = PickImportDir();
            if (!picked.empty()) ImportAssetFile(picked);
        }
        ImGui::SameLine();
        static bool importOpen = false;
        if (ImGui::SmallButton(importOpen ? "取消路径" : "输入路径")) importOpen = !importOpen;
        ImGui::SameLine();
        static int newKind = -1;
        if (ImGui::SmallButton(newKind >= 0 ? "取消新建" : "新建"))
            newKind = (newKind >= 0) ? -1 : 0;
        ImGui::SameLine();
        ImGui::TextUnformatted(assetDir_.c_str());
        // Import row: paste a source path and copy it into the current dir.
        if (importOpen) {
            static char importSrc[1024] = {};
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::InputText("##import_src", importSrc, sizeof(importSrc));
            ImGui::SameLine();
            if (ImGui::SmallButton("导入文件")) {
                ImportAssetFile(importSrc);
                importSrc[0] = '\0';
            }
        }
        // New-asset row: type combo + name + create.
        if (newKind >= 0) {
            static const char* kinds[] = {"目录", "Lua 脚本", "JSON 文件", "文本文件",
                                          "材质球", "JS 脚本"};
            static char newName[128] = {};
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##new_kind", &newKind, kinds, 6)) {
                // hint defaults per kind
                if (newKind == 1) std::strncpy(newName, "new_script.lua", sizeof(newName) - 1);
                else if (newKind == 5) std::strncpy(newName, "new_script.js", sizeof(newName) - 1);
                else if (newKind == 2) std::strncpy(newName, "new_data.json", sizeof(newName) - 1);
                else if (newKind == 4) std::strncpy(newName, "new_material.mat.json", sizeof(newName) - 1);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputText("##new_name", newName, sizeof(newName));
            ImGui::SameLine();
            if (ImGui::SmallButton("创建")) {
                if (newName[0] != '\0') {
                    CreateAssetFile(newName, newKind);
                    newName[0] = '\0';
                    newKind = -1;
                }
            }
        }
        ImGui::Separator();

        // Unity-style Project filter tabs: 全部 / 模型 / 贴图 / 脚本.
        const char* filters[] = {"全部", "模型", "贴图", "脚本", "材质"};
        for (int f = 0; f < 5; ++f) {
            if (f) ImGui::SameLine();
            if (ImGui::SmallButton(filters[f])) assetFilter_ = f;
            if (assetFilter_ == f) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "✓");
            }
        }
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (ImGui::SmallButton(assetGridView_ ? "网格视图" : "列表视图"))
            assetGridView_ = !assetGridView_;
        ImGui::Separator();

        // Plugin asset sources (素材市场): plugins contribute read-only
        // catalogs; 导入 copies an entry into the current project dir.
        if (pluginMgr_ && !pluginMgr_->AssetSources().empty()) {
            for (editor::PluginAssetSource& s : pluginMgr_->AssetSources()) {
                const std::string header = s.name + " (插件)";
                if (!ImGui::CollapsingHeader(header.c_str())) continue;
                if (!s.host || s.listFn == 0) {
                    ImGui::TextDisabled("源不可用");
                    continue;
                }
                const auto res = s.host->CallCaptured(s.listFn, {});
                if (!res.Ok() || res.Value().type != script::Value::Type::Table) {
                    ImGui::TextDisabled("列表加载失败");
                    continue;
                }
                const script::Value& entries = res.Value();
                for (size_t i = 0; i < entries.table->array.size(); ++i) {
                    const script::Value& it = entries.table->array[i];
                    if (it.type != script::Value::Type::Table) continue;
                    std::string name, type, path;
                    for (const auto& kv : it.table->fields) {
                        if (kv.second.type != script::Value::Type::String) continue;
                        if (kv.first == "name") name = kv.second.str;
                        if (kv.first == "type") type = kv.second.str;
                        if (kv.first == "path") path = kv.second.str;
                    }
                    if (name.empty() || path.empty()) continue;
                    ImGui::BulletText("%s  (%s)", name.c_str(), type.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("导入##" + s.id + std::to_string(i)).c_str())) {
                        if (s.importFn != 0) {
                            const auto imp =
                                s.host->CallCaptured(s.importFn, {script::Value::Str(path)});
                            if (!imp.Ok()) {
                                NEON_LOG_ERROR("插件资产源 '%s' 导入失败: %s", s.id.c_str(),
                                               s.host->LastError().message.c_str());
                            } else if (imp.Value().type == script::Value::Type::String &&
                                       !imp.Value().str.empty()) {
                                NEON_LOG_INFO("插件资产源 '%s' 已导入: %s", s.id.c_str(),
                                              imp.Value().str.c_str());
                            }
                        }
                    }
                }
            }
            ImGui::Separator();
        }

        // Split-pane layout: file browsing on the LEFT takes all the height,
        // the selected asset's details/preview sit in a fixed-width RIGHT
        // column (no more squeezing the list with a bottom reserve).
        const float detailW = 250.0f;
        const ImVec2 bodyAvail = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##asset_list", ImVec2(bodyAvail.x - detailW - 8.0f, 0),
                          ImGuiChildFlags_Borders);
        if (assetEntries_.empty()) {
            ImGui::TextWrapped("此目录为空。使用上方 浏览导入 / 浏览目录 添加外部资源，"
                               "或 新建 创建 Lua 脚本 / JSON / 材质球 / 目录。");
        }
        if (assetGridView_) {
            // Thumbnail grid (Unity Project icon mode): a fixed cell per
            // asset with a real preview for textures/models and a colored
            // type tile for directories/scripts/materials/JSON.
            const float cellW = 84.0f;
            const float cellH = 94.0f;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const int cols = std::max(1, static_cast<int>(avail.x / cellW));
            // Parent-directory cell: first slot goes up a level.
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            if (ImGui::Button("⬆ 上级", ImVec2(cellW - 6.0f, cellH - 8.0f))) {
                const std::string parent = ParentPath(assetDir_);
                if (parent != assetDir_) {
                    assetDir_ = parent;
                    RefreshAssetDir();
                }
            }
            int visible = 0;
            for (size_t i = 0; i < assetEntries_.size(); ++i) {
                const AssetEntry& e = assetEntries_[i];
                if (!AssetMatchesFilter(e, assetFilter_)) continue;
                const int col = (visible + 1) % cols;
                const int row = (visible + 1) / cols;
                ++visible;
                ImGui::SetCursorPos(ImVec2(col * cellW, row * cellH));
                const std::string id = "##acell_" + std::to_string(i);
                if (ImGui::InvisibleButton(id.c_str(), ImVec2(cellW - 6.0f, cellH - 8.0f))) {
                    selectedAsset_ = static_cast<int>(i);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("删除")) {
                        selectedAsset_ = static_cast<int>(i);
                        DeleteSelectedAsset();
                    }
                    ImGui::EndPopup();
                }
                const bool hovered = ImGui::IsItemHovered();
                const bool dbl = hovered && ImGui::IsMouseDoubleClicked(0);
                if (dbl) {
                    if (e.isDir) {
                        assetDir_ = e.path;
                        RefreshAssetDir();
                    } else {
                        ImportAssetPath(e.path);
                    }
                }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 tl = ImGui::GetItemRectMin();
                const float tw = ImGui::GetItemRectSize().x;
                const ImVec2 thumbTl = {tl.x + 4.0f, tl.y + 2.0f};
                const ImVec2 thumbBr = {tl.x + tw - 4.0f, tl.y + 60.0f};
                const ImVec2 thumbSize = {thumbBr.x - thumbTl.x, thumbBr.y - thumbTl.y};
                // Selection highlight behind the tile.
                if (selectedAsset_ == static_cast<int>(i)) {
                    dl->AddRectFilled(tl, {tl.x + tw, tl.y + cellH - 8.0f},
                                      IM_COL32(60, 100, 160, 70));
                }
                ImTextureID tid = ImTextureID_Invalid;
                bool flipV = false;
                ImU32 tileCol = IM_COL32(70, 70, 80, 255);
                if (e.isDir) {
                    tileCol = IM_COL32(180, 140, 40, 255);
                } else if (IsImageExt(e.name)) {
                    gfx::Texture tex = assetMgr_.LoadTexture(e.path);
                    if (tex.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(tex.Handle());
                } else if (IsModelExt(e.name)) {
                    RequestMeshThumbnail(e.path);
                    auto thumb = meshThumbs_.find(e.path);
                    if (thumb != meshThumbs_.end()) {
                        tid = thumb->second.texId;
                        flipV = true; // FBO color textures are bottom-up
                    }
                    tileCol = IM_COL32(90, 130, 200, 255);
                } else if (IsMaterialExt(e.name)) {
                    RequestMaterialThumbnail(e.path);
                    auto thumb = materialThumbs_.find(e.path);
                    if (thumb != materialThumbs_.end()) {
                        tid = thumb->second.texId;
                        flipV = true;
                    }
                    tileCol = IM_COL32(150, 90, 190, 255);
                } else if (IsScriptExt(e.name)) {
                    tileCol = IM_COL32(80, 160, 80, 255);
                }
                if (tid != ImTextureID_Invalid) {
                    // Square display area: the thumbnail texture is square, so
                    // stretching it into the wider cell makes spheres look
                    // elliptical. Center a ts x ts square inside the cell.
                    const float ts = std::min(thumbBr.x - thumbTl.x,
                                              thumbBr.y - thumbTl.y);
                    const ImVec2 imgTl = {thumbTl.x + (thumbBr.x - thumbTl.x - ts) * 0.5f,
                                          thumbTl.y + (thumbBr.y - thumbTl.y - ts) * 0.5f};
                    const ImVec2 imgBr = {imgTl.x + ts, imgTl.y + ts};
                    dl->AddImage(tid, imgTl, imgBr, ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(1.0f, flipV ? 0.0f : 1.0f));
                    dl->AddRect(imgTl, imgBr, IM_COL32(30, 30, 35, 255));
                } else {
                    dl->AddRectFilled(thumbTl, thumbBr, tileCol);
                    dl->AddRect(thumbTl, thumbBr, IM_COL32(30, 30, 35, 255));
                    const char* tag = e.isDir ? "DIR"
                                      : IsMaterialExt(e.name) ? "MAT"
                                      : IsScriptExt(e.name) ? "LUA"
                                      : IsImageExt(e.name) ? "IMG"
                                      : IsModelExt(e.name) ? "MDL"
                                                           : "FILE";
                    dl->AddText(ImVec2(thumbTl.x + 4.0f, thumbTl.y + thumbSize.y * 0.5f - 8.0f),
                                IM_COL32(255, 255, 255, 220), tag);
                }
                // Name below the thumbnail (truncated to one line).
                const ImVec2 namePos = {tl.x + 3.0f, thumbBr.y + 2.0f};
                dl->PushClipRect(tl, {tl.x + tw, tl.y + cellH - 6.0f}, true);
                dl->AddText(namePos, IM_COL32(220, 225, 235, 255), e.name.c_str());
                dl->PopClipRect();
                // Drag sources work in grid mode too.
                if (!e.isDir && ImGui::BeginDragDropSource()) {
                    const char* kind = IsImageExt(e.name)   ? "ASSET_TEXTURE"
                                       : IsModelExt(e.name) ? "ASSET_MODEL"
                                       : IsScriptExt(e.name) ? "ASSET_SCRIPT"
                                       : IsMaterialExt(e.name) ? "ASSET_MATERIAL"
                                                               : nullptr;
                    if (kind) {
                        ImGui::SetDragDropPayload(kind, e.path.c_str(), e.path.size() + 1);
                        ImGui::Text("%s", e.name.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
            }
            ImGui::Dummy(ImVec2(1.0f, (visible / cols + 1) * cellH + 20.0f));
        } else {
            if (ImGui::Selectable("⬆ 上级目录##up")) {
                const std::string parent = ParentPath(assetDir_);
                if (parent != assetDir_) {
                    assetDir_ = parent;
                    RefreshAssetDir();
                }
            }
            ImGui::Separator();
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            const AssetEntry& e = assetEntries_[i];
            if (!AssetMatchesFilter(e, assetFilter_)) continue;
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##asset_%zu",
                          e.isDir ? "[D] " : "    ", e.name.c_str(), i);
            if (ImGui::Selectable(label, selectedAsset_ == static_cast<int>(i))) {
                selectedAsset_ = static_cast<int>(i);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("删除")) {
                    selectedAsset_ = static_cast<int>(i);
                    DeleteSelectedAsset();
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (e.isDir) {
                    assetDir_ = e.path;
                    RefreshAssetDir();
                } else {
                    ImportAssetPath(e.path);
                }
            }
            if (e.isDir) continue;
            // Drag sources: textures onto material slots, models onto the scene
            // (hierarchy), scripts onto a selected entity.
            if (IsImageExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_TEXTURE", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsModelExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_MODEL", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsScriptExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_SCRIPT", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            } else if (IsMaterialExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_MATERIAL", e.path.c_str(),
                                          e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        }
        // Built-in sample models live at the bottom of the asset list so they
        // never cover the project files; drag or double-click to add to scene.
        ImGui::Separator();
        if (ImGui::CollapsingHeader("内置模型 (拖入场景)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            const struct {
                const char* key;
                const char* label;
            } kBuiltinModels[] = {{"helmet", "头盔"}, {"tree", "松树"},
                                  {"house", "房屋"}, {"bush", "灌木"},
                                  {"hero", "英雄"}, {"npc", "NPC"}};
            for (size_t bi = 0; bi < sizeof(kBuiltinModels) / sizeof(kBuiltinModels[0]); ++bi) {
                if (bi) ImGui::SameLine(0.0f, 2.0f);
                ImGui::Button(kBuiltinModels[bi].label, ImVec2(52.0f, 0.0f));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    AddEntity(kBuiltinModels[bi].key);
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("ASSET_BUILTIN_MODEL", kBuiltinModels[bi].key,
                                              std::strlen(kBuiltinModels[bi].key) + 1);
                    ImGui::Text("添加 %s", kBuiltinModels[bi].label);
                    ImGui::EndDragDropSource();
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##asset_detail", ImVec2(detailW, 0), ImGuiChildFlags_Borders);
        if (selectedAsset_ >= 0 &&
            selectedAsset_ < static_cast<int>(assetEntries_.size())) {
            const AssetEntry& e = assetEntries_[static_cast<size_t>(selectedAsset_)];
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("%s", e.isDir ? "目录" : e.path.c_str());
            if (!e.isDir) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.1f KB", static_cast<double>(e.size) / 1024.0);
                ImGui::Separator();
                ImTextureID tid = ImTextureID_Invalid;
                bool flipV = false;
                if (IsModelExt(e.name)) {
                    if (ImGui::Button("导入到场景")) ImportAssetPath(e.path);
                    // Mesh thumbnail (T4.8): rendered into a small offscreen
                    // target by the frame's OnRender, cached by path+mtime.
                    RequestMeshThumbnail(e.path);
                    auto thumb = meshThumbs_.find(e.path);
                    if (thumb != meshThumbs_.end() &&
                        thumb->second.texId != ImTextureID_Invalid) {
                        tid = thumb->second.texId;
                        flipV = true; // FBO color textures are bottom-up
                    }
                } else if (IsImageExt(e.name)) {
                    if (ImGui::Button("添加精灵")) AddSpriteEntity(e.path);
                    ImGui::SameLine();
                    if (ImGui::Button("预览")) ImportAssetPath(e.path);
                    gfx::Texture tex = assetMgr_.LoadTexture(e.path);
                    if (tex.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(tex.Handle());
                } else if (IsScriptExt(e.name)) {
                    if (ImGui::Button("编辑")) OpenScriptEditor(e.path);
                    ImGui::SameLine();
                    if (ImGui::Button("外部打开")) OpenInExternalEditor(e.path);
                } else if (IsMaterialExt(e.name)) {
                    RequestMaterialThumbnail(e.path);
                    auto thumb = materialThumbs_.find(e.path);
                    if (thumb != materialThumbs_.end() &&
                        thumb->second.texId != ImTextureID_Invalid) {
                        tid = thumb->second.texId;
                        flipV = true;
                    }
                    // Material parameter summary under the sphere preview.
                    std::ifstream in(e.path, std::ios::binary);
                    if (in.is_open()) {
                        std::string text((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
                        std::string err;
                        core::Json root = core::Json::Parse(text, &err);
                        if (root.IsObject()) {
                            ImGui::Separator();
                            ImGui::TextDisabled("颜色: %s",
                                                root.Get("colorHex")
                                                    ? root.Get("colorHex")->GetString("#FFFFFF").c_str()
                                                    : "#FFFFFF");
                            ImGui::TextDisabled("金属度: %.2f  粗糙度: %.2f",
                                                root.Get("metallic")
                                                    ? root.Get("metallic")->GetNumber()
                                                    : 0.0,
                                                root.Get("roughness")
                                                    ? root.Get("roughness")->GetNumber()
                                                    : 0.8);
                            if (root.Get("albedoTex") &&
                                !root.Get("albedoTex")->GetString().empty())
                                ImGui::TextDisabled("贴图: %s",
                                                    root.Get("albedoTex")->GetString().c_str());
                        }
                    }
                }
                const float prevSize = std::min(140.0f, detailW - 24.0f);
                if (tid != ImTextureID_Invalid) {
                    ImGui::Image(tid, ImVec2(prevSize, prevSize),
                                 ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(1.0f, flipV ? 0.0f : 1.0f));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.0fx%.0f", prevSize, prevSize);
                } else if (IsModelExt(e.name)) {
                    ImGui::Dummy(ImVec2(prevSize, prevSize));
                    ImGui::SameLine();
                    ImGui::TextDisabled("生成缩略图中…");
                }
            }
            ImGui::Separator();
            if (ImGui::Button("删除资产", ImVec2(-1.0f, 0.0f))) DeleteSelectedAsset();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::BuildResourcePanel() {
    if (!showResources_) return;
    if (ImGui::Begin("资源", &showResources_)) {
        assets::AssetStats s = assetMgr_.Stats();
        ImGui::Text("纹理 %zu | 网格 %zu | 字体 %zu", s.textures, s.meshes, s.fonts);
        ImGui::Text("纹理内存 %.2f MB | 三角形 %zu",
                    static_cast<double>(s.textureBytes) / (1024.0 * 1024.0),
                    s.meshTriangles);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##res_tabs")) {
            if (ImGui::BeginTabItem("纹理")) {
                ImGui::BeginChild("##res_tex");
                for (const auto& kv : assetMgr_.Textures()) {
                    if (!kv.second.Valid()) continue;
                    // Strip the load-option cache suffix ("\x1Ff" = glTF flip)
                    // when displaying the asset path.
                    std::string display = kv.first;
                    const size_t sep = display.find('\x1F');
                    if (sep != std::string::npos) display = display.substr(0, sep);
                    ImGui::Text("%s", display.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%dx%d", kv.second.Width(), kv.second.Height());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("网格")) {
                ImGui::BeginChild("##res_mesh");
                for (const auto& kv : assetMgr_.Meshes()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s", kv.first.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%u 三角", kv.second.TriangleCount());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("字体")) {
                ImGui::BeginChild("##res_font");
                for (const auto& kv : assetMgr_.Fonts()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s (%dpx)", kv.first.first.c_str(), kv.first.second);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void EditorApp::BuildInspectorPanel() {
    if (!showInspector_ || selected_ < 0 ||
        selected_ >= static_cast<int>(entities_.size())) {
        return;
    }
    if (ImGui::Begin("属性", &showInspector_)) {
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        // P2-editor UX: multi-selection banner + batch operations. Field edits
        // below always target the ACTIVE (last-clicked) entity.
        if (selection_.size() > 1) {
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "多选: %zu 个实体 (编辑作用于 \"%s\")", selection_.size(),
                               e.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("批量复制")) {
                history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                    &entities_, SelectedIndices()));
                SetSelection(static_cast<int>(entities_.size()) - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("批量删除")) {
                history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                    &entities_, SelectedIndices()));
                ClampSelection();
            }
            ImGui::Separator();
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", e.name.c_str());
        // Every field edit routes through the undo/redo command stack; the old
        // value is captured before the widget mutates the entity.
        if (ImGui::InputText("名称", buf, sizeof(buf))) {
            const std::string oldName = e.name;
            e.name = buf;
            history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                &entities_, selected_, ApplyNameProp, oldName, e.name,
                /*mergeable=*/false)); // one undo step per keystroke
        }
        // Scene-tree parent is edited via the hierarchy panel (drag to
        // reparent); it is intentionally not shown here, the tree already
        // visualizes the parent-child relationship.
        // Node type table (P1-1): explicit type overrides the auto-derived
        // label; the inspector renders type-specific sections below.
        {
            const std::string current = e.nodeType.empty() ? EntityTypeLabel(e) : e.nodeType;
            int sel = 0;
            for (int i = 0; i < static_cast<int>(sizeof(kNodeTypes) / sizeof(kNodeTypes[0])); ++i)
                if (current == kNodeTypes[i]) sel = i;
            if (ImGui::Combo("类型", &sel, kNodeTypes,
                             static_cast<int>(sizeof(kNodeTypes) / sizeof(kNodeTypes[0])))) {
                const std::string oldType = e.nodeType;
                e.nodeType = kNodeTypes[sel];
                history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities_, selected_, ApplyNodeTypeProp, oldType, e.nodeType,
                    /*mergeable=*/false));
            }
            if (!e.nodeType.empty())
                ImGui::TextDisabled("自动类型: %s", EntityTypeLabel(e).c_str());
        }
        if (e.nodeType == "Camera3D") {
            const float oldFov = e.cameraFov;
            if (ImGui::DragFloat("视野 (度)", &e.cameraFov, 0.5f, 20.0f, 120.0f)) {
                history_.Push(std::make_unique<EditPropertyCommand<float>>(
                    &entities_, selected_, ApplyCameraFovProp, oldFov, e.cameraFov));
            }
            const bool oldOrtho = e.cameraOrtho;
            if (ImGui::Checkbox("正交相机", &e.cameraOrtho)) {
                history_.Push(std::make_unique<EditPropertyCommand<bool>>(
                    &entities_, selected_, ApplyCameraOrthoProp, oldOrtho, e.cameraOrtho));
            }
            if (e.cameraOrtho) {
                if (ImGui::DragFloat("正交尺寸", &e.cameraOrthoSize, 0.1f, 0.1f, 2000.0f))
                    sceneDirty_ = true;
            }
            ImGui::TextDisabled("将相机实体选中并设为视图: 使用右上角相机菜单的\"跟随选中\"");
            ImGui::Separator();
        }
        if (e.hasLight) {
            if (ImGui::CollapsingHeader("光源##light", ImGuiTreeNodeFlags_DefaultOpen)) {
                static const char* kLt[] = {"方向光", "点光源", "环境光"};
                const int typeIdx = e.light.type == "point" ? 1 : (e.light.type == "ambient" ? 2 : 0);
                int sel = typeIdx;
                if (ImGui::Combo("类型##lt", &sel, kLt, 3)) {
                    e.light.type = sel == 1 ? "point" : (sel == 2 ? "ambient" : "directional");
                    sceneDirty_ = true;
                }
                if (e.light.type == "directional") {
                    if (ImGui::DragFloat3("方向##lt", &e.light.sunDir.x, 0.05f)) sceneDirty_ = true;
                    if (ImGui::DragFloat("强度##lt", &e.light.intensity, 0.05f, 0.0f, 10.0f))
                        sceneDirty_ = true;
                } else if (e.light.type == "point") {
                    if (ImGui::DragFloat("半径##lt", &e.light.radius, 0.1f, 0.1f, 500.0f))
                        sceneDirty_ = true;
                    if (ImGui::DragFloat("强度##lt", &e.light.intensity, 0.05f, 0.0f, 10.0f))
                        sceneDirty_ = true;
                } else { // ambient
                    if (ImGui::DragFloat("环境光强度##lt", &e.light.ambientStrength, 0.01f, 0.0f, 2.0f))
                        sceneDirty_ = true;
                }
                float col[4] = {e.light.color.r, e.light.color.g, e.light.color.b, e.light.color.a};
                if (ImGui::ColorEdit4("颜色##lt", col)) {
                    e.light.color = {col[0], col[1], col[2], col[3]};
                    sceneDirty_ = true;
                }
                ImGui::Separator();
            }
        }
        if (e.nodeType == "Sprite" && e.spriteTex.empty()) {
            ImGui::TextColored(ImVec4(0.8f, 0.85f, 1.0f, 1.0f),
                               "精灵类型: 在下方\"精灵\"区块设置贴图");
        }
        // P2-1 ground decal: a flat textured quad projected onto the ground.
        if (ImGui::CollapsingHeader("贴花##decal")) {
            static char decalBuf[512] = {};
            std::snprintf(decalBuf, sizeof(decalBuf), "%s", e.decalTex.c_str());
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::InputText("贴图", decalBuf, sizeof(decalBuf))) {
                const std::string old = e.decalTex;
                e.decalTex = decalBuf;
                e.decalMesh = {};
                history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities_, selected_, ApplyDecalTexProp, old, e.decalTex,
                    /*mergeable=*/false));
            }
            if (!e.decalTex.empty()) {
                const float oldSize = e.decalSize;
                if (ImGui::DragFloat("尺寸", &e.decalSize, 0.1f, 0.1f, 100.0f)) {
                    e.decalMesh = {};
                    history_.Push(std::make_unique<EditPropertyCommand<float>>(
                        &entities_, selected_, ApplyDecalSizeProp, oldSize, e.decalSize));
                }
                const float oldAlpha = e.decalAlpha;
                if (ImGui::DragFloat("不透明度", &e.decalAlpha, 0.01f, 0.0f, 1.0f)) {
                    history_.Push(std::make_unique<EditPropertyCommand<float>>(
                        &entities_, selected_, ApplyDecalAlphaProp, oldAlpha, e.decalAlpha));
                }
            } else {
                ImGui::TextDisabled("填入贴图路径后保存/导出即生成地面贴花");
            }
            ImGui::Separator();
        }
        if (!e.spriteTex.empty()) {
            ImGui::TextDisabled("精灵贴图: %s", e.spriteTex.c_str());
            const SpriteFlipValue oldFlip{e.spriteFlipX, e.spriteFlipY};
            bool fx = e.spriteFlipX, fy = e.spriteFlipY;
            ImGui::Checkbox("水平翻转", &fx);
            ImGui::SameLine();
            ImGui::Checkbox("垂直翻转", &fy);
            if (fx != e.spriteFlipX || fy != e.spriteFlipY) {
                e.spriteFlipX = fx;
                e.spriteFlipY = fy;
                history_.Push(std::make_unique<EditPropertyCommand<SpriteFlipValue>>(
                    &entities_, selected_, ApplySpriteFlip, oldFlip, SpriteFlipValue{fx, fy}));
            }
            ImGui::Separator();
        }

        if (!e.prefab.empty()) {
            ImGui::TextDisabled("预置体: %s", e.prefab.c_str());
        }
        {
            static char prefabName[128] = {};
            std::snprintf(prefabName, sizeof(prefabName), "%s", e.name.c_str());
            ImGui::SetNextItemWidth(140.0f);
            ImGui::InputText("预置体名", prefabName, sizeof(prefabName));
            ImGui::SameLine();
            if (ImGui::Button("另存为预置体")) {
                std::string name(prefabName);
                if (!name.empty()) {
                    const size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);
                    SavePrefab(name);
                }
            }
        }
        ImGui::Separator();
        // Unity-style: every entity is a type + a list of components. The
        // default components (变换/网格/生命) are ordinary blocks in this same
        // list; transform is mandatory like Unity's Transform, mesh and health
        // are removable.
        if (ImGui::CollapsingHeader("组件", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::CollapsingHeader("变换##transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        const math::Vec3 oldPos = e.pos;
        if (ImGui::DragFloat3("位置", &e.pos.x, 0.1f)) {
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, oldPos, e.rot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kPos));
        }
        math::Vec3 euler = e.rot.ToMat4().TransformDir({0, 0, -1});
        float rotDeg = std::atan2(euler.x, euler.z) * math::kRadToDeg;
        const math::Quat oldRot = e.rot;
        if (ImGui::DragFloat("旋转 Y", &rotDeg, 0.5f, -180.0f, 180.0f)) {
            e.rot = math::Quat::FromEuler(0, rotDeg * math::kDegToRad, 0);
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, oldRot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kRot));
        }
        const math::Vec3 oldScale = e.scale;
        if (ImGui::DragFloat3("缩放", &e.scale.x, 0.05f, 0.05f, 50.0f)) {
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, e.rot, oldScale, e.pos, e.rot, e.scale,
                EditTransformCommand::kScale));
        }
        const float oldZ = e.zOrder;
        if (ImGui::DragFloat("Z 排序", &e.zOrder, 0.1f, -10000.0f, 10000.0f)) {
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyZOrderProp, oldZ, e.zOrder));
        }
        }
        // 网格 (MeshRenderer-like): mesh key + material + textures in one
        // block. Hidden for sprites (the sprite quad replaces it); removable -
        // the entity becomes a logical/sprite-only object, and 网格 reappears
        // in the 添加组件 dropdown to re-add it.
        if (e.spriteTex.empty() && !e.meshKey.empty()) {
        // Collapsed by default: the mesh block (key + material + 4 texture
        // slots) is tall and pushed 生命/脚本 below the panel's visible area,
        // making their remove buttons unreachable without scrolling.
        const bool meshOpen = ImGui::CollapsingHeader("网格##mesh", ImGuiTreeNodeFlags_None);
        if (meshOpen && !e.meshKey.empty()) {
        char meshBuf[2048];
        std::snprintf(meshBuf, sizeof(meshBuf), "%s", e.meshKey.c_str());
        if (ImGui::InputText("网格键", meshBuf, sizeof(meshBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string newKey(meshBuf);
            if (newKey != e.meshKey) {
                const std::string oldKey = e.meshKey;
                history_.Push(std::make_unique<EditMeshKeyCommand>(
                    this, &entities_, selected_, oldKey, newKey));
            }
        }
        // Drag a model from the asset panel to replace the mesh.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                std::string path(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize));
                if (!path.empty() && path.back() == '\0') path.pop_back();
                if (!path.empty()) {
                    const std::string lower = ToLower(path);
                    const std::string key =
                        lower.rfind(".obj") != std::string::npos ? "obj:" + path
                                                                 : "gltf:" + path;
                    const std::string oldKey = e.meshKey;
                    history_.Push(std::make_unique<EditMeshKeyCommand>(
                        this, &entities_, selected_, oldKey, key));
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (e.mesh.Valid()) {
            ImGui::TextDisabled("%u 三角形", e.mesh.TriangleCount());
            const math::AABB& b = e.mesh.Bounds();
            ImGui::TextDisabled("包围盒 (%.1f, %.1f, %.1f) ~ (%.1f, %.1f, %.1f)", b.min.x,
                                b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
        }
        ImGui::Separator();
        if (!e.materialRef.empty()) {
            ImGui::TextDisabled("材质球: %s", e.materialRef.c_str());
        }
        {
            static char matName[128] = {};
            std::snprintf(matName, sizeof(matName), "%s", e.name.c_str());
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputText("材质球名", matName, sizeof(matName));
            ImGui::SameLine();
            if (ImGui::Button("另存为材质球")) {
                std::string name(matName);
                if (!name.empty()) {
                    const size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);
                    SaveMaterialAsset(name);
                }
            }
        }
        // P2-6 custom shader (fragment .glsl) with hot reload: compiled
        // against the built-in unlit vertex contract; the file is re-watched
        // by PollHotReload (--hot) and the 重编译 button recompiles now.
        {
            static char shaderBuf[512] = {};
            std::snprintf(shaderBuf, sizeof(shaderBuf), "%s", e.shaderPath.c_str());
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::InputText("着色器 (.glsl 片元)", shaderBuf, sizeof(shaderBuf))) {
                const std::string oldPath = e.shaderPath;
                e.shaderPath = shaderBuf;
                history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities_, selected_, ApplyShaderPathProp, oldPath, e.shaderPath,
                    /*mergeable=*/false));
                ReloadEntityShader(e);
            }
            ImGui::SameLine();
            if (ImGui::Button("重编译")) ReloadEntityShader(e);
            if (!e.shaderPath.empty())
                ImGui::TextDisabled(e.customShader.Valid() ? "已编译 ✓ (GL)"
                                                           : "未编译 / 后端不支持自定义着色器");
        }
        // Drag a material-ball asset from the asset panel onto the entity.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MATERIAL")) {
                std::string path(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize));
                if (!path.empty() && path.back() == '\0') path.pop_back();
                if (!path.empty()) ApplyMaterialAsset(path);
            }
            ImGui::EndDragDropTarget();
        }
        const gfx::Color oldTint = e.tint;
        if (ImGui::ColorEdit3("颜色", &e.tint.r)) {
            e.material.tint = e.tint;
            history_.Push(std::make_unique<EditPropertyCommand<gfx::Color>>(
                &entities_, selected_, ApplyColorProp, oldTint, e.tint));
        }
        const float oldMetallic = e.metallic;
        if (ImGui::DragFloat("金属度", &e.metallic, 0.01f, 0.0f, 1.0f)) {
            e.material.metallic = e.metallic;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyMetallicProp, oldMetallic, e.metallic));
        }
        const float oldRoughness = e.roughness;
        if (ImGui::DragFloat("粗糙度", &e.roughness, 0.01f, 0.0f, 1.0f)) {
            e.material.roughness = e.roughness;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyRoughnessProp, oldRoughness, e.roughness));
        }
        const float oldAO = e.ao;
        if (ImGui::DragFloat("环境光遮蔽", &e.ao, 0.01f, 0.0f, 1.0f)) {
            e.material.aoStrength = e.ao;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyAOProp, oldAO, e.ao));
        }
        const float oldEmissiveIntensity = e.emissiveIntensity;
        if (ImGui::DragFloat("自发光强度", &e.emissiveIntensity, 0.05f, 0.0f, 5.0f)) {
            e.material.emissiveIntensity = e.emissiveIntensity;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyEmissiveIntensityProp, oldEmissiveIntensity,
                e.emissiveIntensity));
        }
        // One slot per PBR texture: thumbnail preview, editable path (Enter to
        // commit), clear button, and a drag-drop target from the asset panel.
        // Every change routes through the undo history as a texture-slot edit.
        auto textureSlot = [this, &e](const char* label, std::string& path,
                                      gfx::TextureHandle& handle,
                                      void (*apply)(SceneEntity&, const TextureSlotValue&)) {
            ImTextureID tid = ImTextureID_Invalid;
            if (handle.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(handle);
            const ImVec2 previewSize(22.0f, 22.0f);
            // The thumbnail is also the drop target for textures dragged from
            // the asset panel.
            if (tid != ImTextureID_Invalid)
                ImGui::ImageButton(("##thumb_" + std::string(label)).c_str(), tid,
                                   previewSize);
            else
                ImGui::Button(("##thumb_" + std::string(label)).c_str(), previewSize);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    std::string newPath(static_cast<const char*>(payload->Data),
                                        static_cast<size_t>(payload->DataSize));
                    if (!newPath.empty() && newPath.back() == '\0') newPath.pop_back();
                    if (!newPath.empty() && newPath != path) {
                        const TextureSlotValue oldVal{path, handle};
                        gfx::Texture tex = assetMgr_.LoadTexture(newPath);
                        if (tex.Valid()) {
                            const TextureSlotValue newVal{newPath, tex.Handle()};
                            history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities_, selected_, apply, oldVal, newVal));
                        } else {
                            NEON_LOG_WARN("Editor: dropped texture '%s' failed to load", newPath.c_str());
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s", path.c_str());
            ImGui::SetNextItemWidth(-190.0f);
            if (ImGui::InputText((std::string("##path_") + label).c_str(), buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string newPath(buf);
                if (newPath != path) {
                    const TextureSlotValue oldVal{path, handle};
                    gfx::Texture tex =
                        newPath.empty() ? gfx::Texture{} : assetMgr_.LoadTexture(newPath);
                    if (newPath.empty() || tex.Valid()) {
                        const TextureSlotValue newVal{newPath, tex.Handle()};
                        history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                            &entities_, selected_, apply, oldVal, newVal));
                    } else {
                        NEON_LOG_WARN("Editor: texture '%s' failed to load (slot '%s')",
                                      newPath.c_str(), label);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button((std::string("清除##") + label).c_str())) {
                const TextureSlotValue oldVal{path, handle};
                const TextureSlotValue newVal{"", gfx::TextureHandle{}};
                history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                    &entities_, selected_, apply, oldVal, newVal));
            }
            // P2-editor UX: drag a texture asset from the 资源 panel onto the
            // slot to assign it.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    const char* src = static_cast<const char*>(p->Data);
                    if (src && *src) {
                        const std::string newPath(src);
                        gfx::Texture tex = assetMgr_.LoadTexture(newPath);
                        if (tex.Valid()) {
                            const TextureSlotValue oldVal{path, handle};
                            history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities_, selected_, apply, oldVal,
                                TextureSlotValue{newPath, tex.Handle()}));
                        } else {
                            NEON_LOG_WARN("Editor: texture '%s' failed to load (slot '%s')",
                                          newPath.c_str(), label);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Trailing label (like the material rows): thumb / input / clear
            // stay aligned regardless of label length.
            ImGui::SameLine();
            ImGui::TextDisabled("%s", label);
        };
        textureSlot("漫反射", e.albedoTex, e.material.albedo, ApplyAlbedoTexSlot);
        textureSlot("金属度/粗糙度", e.mrTex, e.material.metallicRoughness, ApplyMRTexSlot);
        textureSlot("环境光遮蔽图", e.aoTex, e.material.occlusion, ApplyAOTexSlot);
        textureSlot("自发光图", e.emissiveTex, e.material.emissive, ApplyEmissiveTexSlot);
        ImGui::Separator();
        if (ImGui::Button("移除网格")) {
            history_.Push(std::make_unique<EditMeshKeyCommand>(
                this, &entities_, selected_, e.meshKey, ""));
        }
        }
        }
        // 生命: attached when maxHp > 0; removable (sets maxHp back to 0).
        if (e.maxHp > 0.0f) {
        const bool healthOpen =
            ImGui::CollapsingHeader("生命##health", ImGuiTreeNodeFlags_DefaultOpen);
        if (healthOpen && e.maxHp > 0.0f) {
        const float oldHp = e.hp;
        if (ImGui::DragFloat("当前生命", &e.hp, 1.0f, 0.0f, e.maxHp)) {
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyHpProp, oldHp, e.hp));
        }
        const float oldMaxHp = e.maxHp;
        if (ImGui::DragFloat("最大生命", &e.maxHp, 1.0f, 0.0f, 1e9f)) {
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyMaxHpProp, oldMaxHp, e.maxHp));
        }
        ImGui::Separator();
        if (ImGui::Button("移除生命")) {
            const HealthValue oldV{e.hp, e.maxHp};
            history_.Push(std::make_unique<EditPropertyCommand<HealthValue>>(
                &entities_, selected_, ApplyHealth, oldV, HealthValue{},
                /*mergeable=*/false));
        }
        }
        }
        auto makeNum = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto makeStr = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto makeBool = [](bool v) {
            core::Json j;
            j.type_ = core::Json::Type::Bool;
            j.bool_ = v;
            return j;
        };
        auto makeArr = [&](const std::vector<double>& v) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            for (double x : v) j.array_.push_back(makeNum(x));
            return j;
        };
        // Script components: ordinary component blocks (schema backend/path/
        // vars), each with its own remove button - exactly like the
        // schema-driven components below. Multiple scripts = multiple blocks.
        {
            const scene::ComponentSchema* scriptSchema =
                scene::FindComponentSchema("script");
            for (size_t si = 0; si < e.scripts.size(); ++si) {
                SceneScriptFields& f = e.scripts[si];
                const std::string header = "脚本##script_" + std::to_string(si);
                const bool open = ImGui::CollapsingHeader(
                    header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                if (!open) continue;
                if (!scriptSchema) {
                    ImGui::TextWrapped("%s", core::JsonWriter::Write(f.vars).c_str());
                    continue;
                }
                for (const scene::FieldSchema& fs : scriptSchema->fields) {
                    if (fs.type == scene::FieldType::Enum) {
                        int sel = 0;
                        if (fs.options)
                            for (int o = 0; o < fs.optionCount; ++o)
                                if (f.backend == fs.options[o]) sel = o;
                        if (ImGui::Combo(fs.label.c_str(), &sel, fs.options,
                                         fs.optionCount)) {
                            const ScriptFieldEdit oldV{si, fs.key, makeStr(f.backend)};
                            const ScriptFieldEdit newV{si, fs.key,
                                                       makeStr(fs.options[sel])};
                            history_.Push(std::make_unique<
                                EditPropertyCommand<ScriptFieldEdit>>(
                                &entities_, selected_, ApplyScriptField, oldV, newV,
                                /*mergeable=*/false));
                        }
                    } else if (fs.type == scene::FieldType::Resource) {
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s", f.path.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(fs.label.c_str(), buf, sizeof(buf),
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                            const ScriptFieldEdit oldV{si, fs.key, makeStr(f.path)};
                            const ScriptFieldEdit newV{si, fs.key, makeStr(buf)};
                            history_.Push(std::make_unique<
                                EditPropertyCommand<ScriptFieldEdit>>(
                                &entities_, selected_, ApplyScriptField, oldV, newV,
                                /*mergeable=*/false));
                        }
                        const char* payloadKind =
                            fs.resourceKind && std::string(fs.resourceKind) == "script"
                                ? "ASSET_SCRIPT"
                                : "ASSET_TEXTURE";
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(payloadKind)) {
                                std::string dropped(
                                    static_cast<const char*>(payload->Data),
                                    static_cast<size_t>(payload->DataSize));
                                if (!dropped.empty() && dropped.back() == '\0')
                                    dropped.pop_back();
                                if (!dropped.empty()) {
                                    const ScriptFieldEdit oldV{si, fs.key, makeStr(f.path)};
                                    const ScriptFieldEdit newV{
                                        si, fs.key,
                                        makeStr(ToProjectRelPath(dropped, projectDir_))};
                                    history_.Push(std::make_unique<
                                        EditPropertyCommand<ScriptFieldEdit>>(
                                        &entities_, selected_, ApplyScriptField, oldV, newV,
                                        /*mergeable=*/false));
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                    } else if (fs.type == scene::FieldType::Json) {
                        ImGui::TextDisabled("%s: %s", fs.label.c_str(),
                                            core::JsonWriter::Write(f.vars).c_str());
                    }
                }
                ImGui::Separator();
                if (ImGui::Button("移除脚本")) {
                    std::vector<SceneScriptFields> newList = e.scripts;
                    newList.erase(newList.begin() + static_cast<ptrdiff_t>(si));
                    history_.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities_, selected_, ApplyScriptList, e.scripts, newList,
                        /*mergeable=*/false));
                    break; // the list changed; re-render next frame
                }
            }
            // Dragging a .lua onto the component section mounts a new script.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
                    const char* path = static_cast<const char*>(payload->Data);
                    if (path && selected_ >= 0 &&
                        selected_ < static_cast<int>(entities_.size())) {
                        std::vector<SceneScriptFields> newList = e.scripts;
                        newList.push_back({"lua", ToProjectRelPath(path, projectDir_), {}});
                        history_.Push(std::make_unique<
                            EditPropertyCommand<std::vector<SceneScriptFields>>>(
                            &entities_, selected_, ApplyScriptList, e.scripts, newList,
                            /*mergeable=*/false));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        for (auto it = e.extraComponents.begin(); it != e.extraComponents.end(); ++it) {
            const std::string& compName = it->first;
            core::Json& compData = it->second;
            if (compName == "plant" || compName == "zombie") continue; // 2D canvas edits
            const scene::ComponentSchema* schema = scene::FindComponentSchema(compName);
            if (!schema && pluginMgr_) schema = pluginMgr_->FindSchema(compName);
            const std::string header =
                schema ? (schema->label + "##" + compName) : (compName + "##raw");
            const bool open =
                ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (!open) continue;
            if (!schema) {
                ImGui::TextWrapped("%s", core::JsonWriter::Write(compData).c_str());
                continue;
            }
            for (const scene::FieldSchema& f : schema->fields) {
                if (!compData.IsObject()) {
                    compData.type_ = core::Json::Type::Object;
                }
                core::Json& node = compData.object_[f.key];
                if (node.IsNull()) node = makeNum(f.def);
                const core::Json oldField = node;
                bool changed = false;
                switch (f.type) {
                    case scene::FieldType::Number: {
                        float v = static_cast<float>(node.IsNumber() ? node.GetNumber() : f.def);
                        if (ImGui::DragFloat(f.label.c_str(), &v, static_cast<float>(f.step),
                                             static_cast<float>(f.min),
                                             static_cast<float>(f.max)))
                            node = makeNum(static_cast<double>(v)), changed = true;
                        break;
                    }
                    case scene::FieldType::Int: {
                        int v = node.IsNumber() ? static_cast<int>(node.GetNumber())
                                                : static_cast<int>(f.def);
                        if (ImGui::DragInt(f.label.c_str(), &v, 1, static_cast<int>(f.min),
                                           static_cast<int>(f.max)))
                            node = makeNum(v), changed = true;
                        break;
                    }
                    case scene::FieldType::Bool: {
                        bool v = node.IsBool() ? node.GetBool() : false;
                        if (ImGui::Checkbox(f.label.c_str(), &v))
                            node = makeBool(v), changed = true;
                        break;
                    }
                    case scene::FieldType::String: {
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s",
                                      node.IsString() ? node.GetString().c_str() : "");
                        if (ImGui::InputText(f.label.c_str(), buf, sizeof(buf))) {
                            node = makeStr(buf);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Vec3: {
                        float v[3] = {static_cast<float>(f.def), static_cast<float>(f.def),
                                      static_cast<float>(f.def)};
                        if (node.IsArray() && node.Size() == 3) {
                            for (int i = 0; i < 3; ++i)
                                v[i] = static_cast<float>(node.At(static_cast<size_t>(i))
                                                              ->GetNumber());
                        }
                        if (ImGui::DragFloat3(f.label.c_str(), v,
                                              static_cast<float>(f.step),
                                              static_cast<float>(f.min),
                                              static_cast<float>(f.max))) {
                            node = makeArr({v[0], v[1], v[2]});
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Color: {
                        float col[4] = {1, 1, 1, 1};
                        if (node.IsString()) {
                            gfx::Color c = ColorFromHex(node.GetString());
                            col[0] = c.r;
                            col[1] = c.g;
                            col[2] = c.b;
                        }
                        if (ImGui::ColorEdit3(f.label.c_str(), col)) {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          static_cast<int>(col[0] * 255.0f),
                                          static_cast<int>(col[1] * 255.0f),
                                          static_cast<int>(col[2] * 255.0f));
                            node = makeStr(hex);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Enum: {
                        int sel = 0;
                        if (node.IsString() && f.options) {
                            for (int i = 0; i < f.optionCount; ++i)
                                if (node.GetString() == f.options[i]) sel = i;
                        }
                        if (ImGui::Combo(f.label.c_str(), &sel, f.options, f.optionCount)) {
                            node = makeStr(f.options[sel]);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Resource: {
                        std::string path = node.IsString() ? node.GetString() : "";
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(f.label.c_str(), buf, sizeof(buf),
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                            node = makeStr(buf);
                            changed = true;
                        }
                        // Drag a matching asset from the asset panel.
                        const char* payloadKind =
                            f.resourceKind && std::string(f.resourceKind) == "model"
                                ? "ASSET_MODEL"
                                : f.resourceKind && std::string(f.resourceKind) == "script"
                                      ? "ASSET_SCRIPT"
                                      : "ASSET_TEXTURE";
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(payloadKind)) {
                                std::string dropped(static_cast<const char*>(payload->Data),
                                                    static_cast<size_t>(payload->DataSize));
                                if (!dropped.empty() && dropped.back() == '\0')
                                    dropped.pop_back();
                                if (!dropped.empty()) {
                                    node = makeStr(dropped);
                                    changed = true;
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        break;
                    }
                    case scene::FieldType::Json:
                        ImGui::TextDisabled("%s: %s", f.label.c_str(),
                                            core::JsonWriter::Write(node).c_str());
                        break;
                }
                if (changed) {
                    history_.Push(std::make_unique<EditComponentCommand>(
                        &entities_, selected_, compName, f.key, oldField, node));
                }
            }
            ImGui::Separator();
            if (ImGui::Button("移除组件")) {
                history_.Push(std::make_unique<AddComponentCommand>(
                    &entities_, selected_, compName, compData, /*remove=*/true));
                break; // the command already removed the component
            }
        }
        // 添加组件: every component type is here - default components that
        // were removed (网格/生命) reappear, scripts are multi-instance, and
        // schema components (刚体/行为树/...) work as before. Transform is
        // mandatory (Unity's Transform) and never listed.
        {
            const auto& allSchemas = scene::AllComponentSchemas();
            std::vector<const scene::ComponentSchema*> addable;
            for (const scene::ComponentSchema& s : allSchemas) {
                if (s.name == "transform" || s.name == "name" ||
                    s.name == "plant" || s.name == "zombie")
                    continue;
                if (s.name == "mesh" && (!e.meshKey.empty() || !e.spriteTex.empty()))
                    continue; // already has a renderer
                if (s.name == "health" && e.maxHp > 0.0f)
                    continue; // already attached
                if (e.extraComponents.count(s.name)) continue; // already present
                addable.push_back(&s);
            }
            // Plugin-registered component schemas (NeonEditor.registerComponent).
            if (pluginMgr_) {
                for (const scene::ComponentSchema& s : pluginMgr_->Schemas()) {
                    if (s.name == "transform" || e.extraComponents.count(s.name)) continue;
                    addable.push_back(&s);
                }
            }
            if (!addable.empty()) {
                static int addCompSel = 0;
                if (addCompSel >= static_cast<int>(addable.size())) addCompSel = 0;
                std::vector<const char*> addLabels;
                for (const scene::ComponentSchema* s : addable)
                    addLabels.push_back(s->label.c_str());
                ImGui::SetNextItemWidth(110.0f);
                ImGui::Combo("##add_component", &addCompSel, addLabels.data(),
                             static_cast<int>(addLabels.size()));
                ImGui::SameLine();
                if (ImGui::Button("添加组件")) {
                    const scene::ComponentSchema* schema =
                        addable[static_cast<size_t>(addCompSel)];
                    if (schema->name == "script") {
                        // Scripts are a multi-instance list (extraComponents is
                        // a name-keyed map, so scripts keep their own vector).
                        // Default path: the script currently selected in the
                        // script panel (if any); editable in the block.
                        if (scriptFiles_.empty()) RefreshScriptChecks();
                        std::string defPath;
                        if (scriptAttachIndex_ >= 0 &&
                            scriptAttachIndex_ < static_cast<int>(scriptFiles_.size()))
                            defPath = scriptFiles_[static_cast<size_t>(scriptAttachIndex_)];
                        std::vector<SceneScriptFields> newList = e.scripts;
                        newList.push_back({"lua", defPath, {}});
                        history_.Push(std::make_unique<
                            EditPropertyCommand<std::vector<SceneScriptFields>>>(
                            &entities_, selected_, ApplyScriptList, e.scripts, newList,
                            /*mergeable=*/false));
                    } else if (schema->name == "mesh") {
                        // Re-add the mesh renderer (default cube).
                        history_.Push(std::make_unique<EditMeshKeyCommand>(
                            this, &entities_, selected_, "", "cube"));
                    } else if (schema->name == "health") {
                        const HealthValue oldV{e.hp, e.maxHp};
                        history_.Push(std::make_unique<EditPropertyCommand<HealthValue>>(
                            &entities_, selected_, ApplyHealth, oldV, HealthValue{100, 100},
                            /*mergeable=*/false));
                    } else {
                        core::Json data;
                        data.type_ = core::Json::Type::Object;
                        for (const scene::FieldSchema& f : schema->fields) {
                            switch (f.type) {
                                case scene::FieldType::Number:
                                case scene::FieldType::Int:
                                    data.object_[f.key] = makeNum(f.def);
                                    break;
                                case scene::FieldType::Bool:
                                    data.object_[f.key] = makeBool(f.def != 0.0);
                                    break;
                                case scene::FieldType::Vec3:
                                    data.object_[f.key] = makeArr({f.def, f.def, f.def});
                                    break;
                                case scene::FieldType::Color:
                                    data.object_[f.key] = makeStr("#FFFFFF");
                                    break;
                                case scene::FieldType::Json: {
                                    core::Json o;
                                    o.type_ = core::Json::Type::Object;
                                    data.object_[f.key] = std::move(o);
                                    break;
                                }
                                case scene::FieldType::Enum:
                                    // Default to the first option so enum
                                    // components are valid immediately.
                                    data.object_[f.key] = makeStr(
                                        f.options && f.optionCount > 0 ? f.options[0] : "");
                                    break;
                                default:
                                    data.object_[f.key] = makeStr("");
                                    break;
                            }
                        }
                        history_.Push(std::make_unique<AddComponentCommand>(
                            &entities_, selected_, schema->name, std::move(data),
                            /*remove=*/false));
                    }
                }
            }
        }
        }
    }
    ImGui::End();
}

void EditorApp::BuildLogPanel() {
    if (!showLog_) return;
    logEntries_ = core::GetRecentLogs(800);
    if (ImGui::Begin("日志", &showLog_)) {
        const char* filters[] = {"全部", "INFO+", "WARN+", "ERROR"};
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Combo("级别", &logFilter_, filters, 4);
        ImGui::SameLine();
        ImGui::Checkbox("自动滚动", &logAutoScroll_);
        ImGui::SameLine();
        if (ImGui::Button("清空")) core::ClearLogs();
        ImGui::Separator();

        static bool wasAtBottom = true;
        ImGui::BeginChild("##log_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const ImVec4 colors[4] = {
            ImVec4(0.55f, 0.58f, 0.65f, 1.0f), // debug
            ImVec4(0.90f, 0.95f, 1.00f, 1.0f), // info
            ImVec4(1.00f, 0.85f, 0.35f, 1.0f), // warn
            ImVec4(1.00f, 0.40f, 0.35f, 1.0f), // error
        };
        size_t shown = 0;
        for (const core::LogEntry& entry : logEntries_) {
            if (logFilter_ == 1 && entry.level < core::LogLevel::Info) continue;
            if (logFilter_ == 2 && entry.level < core::LogLevel::Warn) continue;
            if (logFilter_ == 3 && entry.level < core::LogLevel::Error) continue;
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  colors[static_cast<int>(entry.level)]);
            ImGui::TextWrapped("%s", entry.text.c_str());
            ImGui::PopStyleColor();
            ++shown;
        }
        if (logAutoScroll_ && wasAtBottom && shown > 0) ImGui::SetScrollHereY(1.0f);
        wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::BuildProfilerPanel() {
    if (!showProfiler_) return;
    if (ImGui::Begin("性能", &showProfiler_)) {
        const float ms = TimeRef().delta * 1000.0f;
        const gfx::Renderer::RenderStats& st = renderer_.Stats();
        profilerMs_[profilerMsHead_] = ms;
        profilerMsHead_ = (profilerMsHead_ + 1) % kProfilerSamples;

        ImGui::Text("帧时间 %.2f ms | %.1f FPS", ms, TimeRef().Fps());
        ImGui::Text("Draw 调用 %u | 三角形 %u | 实例 %u", st.drawCalls, st.triangles,
                    st.instances);

        const bool playing = playtestActive_ && playtest_ != nullptr;
        const size_t sceneEnts = entities_.size();
        const size_t playEnts = playing ? playtest_->EntityCount() : 0;
        const size_t bodies = playing ? playtest_->PhysicsBodyCount() : 0;
        const size_t trees = playing ? playtest_->BehaviorTreeCount() : 0;
        const size_t scripts = playing ? playtest_->ScriptCount() : 0;
        ImGui::Text("实体 %zu (试玩 %zu) | 物理刚体 %zu", sceneEnts, playEnts, bodies);
        ImGui::Text("行为树 %zu | 脚本 %zu", trees, scripts);

        const assets::AssetStats a = assetMgr_.Stats();
        ImGui::Text("纹理 %zu | 网格 %zu | 三角 %zu", a.textures, a.meshes, a.meshTriangles);
        ImGui::Text("纹理内存 %.2f MB",
                    static_cast<double>(a.textureBytes) / (1024.0 * 1024.0));

        ImGui::Separator();
        // Plot the ring buffer in chronological order (oldest = profilerMsHead_)
        // so the graph reads left-to-right instead of jumping when the head
        // wraps around the fixed array.
        float wrapped[kProfilerSamples];
        for (int i = 0; i < kProfilerSamples; ++i)
            wrapped[i] = profilerMs_[(profilerMsHead_ + i) % kProfilerSamples];
        ImGui::PlotLines("##frame_ms", wrapped, kProfilerSamples, 0, "帧时间 (ms)",
                         0.0f, 40.0f, ImVec2(-1.0f, 88.0f));
        profilerDrawn_ = true;
    }
    ImGui::End();
}

void EditorApp::BuildViewportPanel() {
    ImGuiWindowFlags vpFlags = ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoBackground;
    // NOTE: deliberately NOT ImGuiWindowFlags_NoInputs. That flag disables
    // manual moving/resizing (imgui.cpp:7938), so the viewport could never be
    // undocked or re-docked elsewhere. The 3D camera reads the platform input
    // directly; ImGui just sees a normal (backgroundless) docked panel.
    if (ImGui::Begin("视口", nullptr, vpFlags)) {
        // The viewport is a normal dockable panel defaulting to the central
        // node. The user's saved layout (with a DockId) is always restored as
        //-is; only when the DockId was LOST (a previous session left it
        // floating) do we fall back to docking it in the center, on its first
        // frame - so an intentional mid-session undock is never yanked back.
        if (!viewportDockFallbackDone_) {
            viewportDockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (dockspaceId_ && !win->DockId && !win->DockIsActive) {
                if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId_)) {
                    ImGui::DockBuilderDockWindow("视口", central->ID);
                    NEON_LOG_INFO("Editor: viewport DockId was lost; re-docked to the central node");
                }
            }
        }
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        math::Vec2 uiPos = renderer_.ScreenToUI({pos.x, pos.y});
        float scale = renderer_.UIScale();
        viewportRect_ = {uiPos.x, uiPos.y, size.x / scale, size.y / scale};
        viewportScreenRect_ = {pos.x, pos.y, size.x, size.y};

        ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                           "右键旋转 | 中键平移 | 滚轮缩放 | 左键拾取");
        if (playtestActive_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "试玩中 (F5 停止)");
        }
                const char* camLabel = viewCam_ == ViewCam::Top
                                   ? "顶视 (正交)"
                                   : viewCam_ == ViewCam::Front ? "前视 (正交)" : "透视";
        std::string physInfo;
        if (playtestActive_ && playtest_) {
            physInfo = " | 物理 " + std::to_string(playtest_->PhysicsBodyCount());
        }
        ImGui::TextDisabled("%s | 实体 %zu%s | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f", camLabel,
                            entities_.size(), physInfo.c_str(), camTarget_.x, camTarget_.y,
                            camTarget_.z, camDist_);
        // Transform gizmo for the selected entity (drawn into this window's
        // draw list; interacts via ImGui's mouse state).
        DrawTransformGizmo();
    }
    ImGui::End();
}

// Navigation tool: edit a 2D walkability grid (.navgrid.json), place a start
// and goal, and preview the A* path. The runtime/scripts consume the same
// asset format (neon::nav::NavGrid).
void EditorApp::BuildNavPanel() {
    if (!showNav_) return;
    if (ImGui::Begin("导航", &showNav_)) {
        char navBuf[512];
        std::snprintf(navBuf, sizeof(navBuf), "%s", navAssetPath_.c_str());
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText("导航资产", navBuf, sizeof(navBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            navAssetPath_ = navBuf;
        }
        if (ImGui::Button("加载")) {
            std::ifstream in(navAssetPath_, std::ios::binary);
            if (!in.is_open()) {
                NEON_LOG_ERROR("Nav: cannot open '%s'", navAssetPath_.c_str());
            } else {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto r = nav::NavGrid::FromJson(text);
                if (!r.Ok()) {
                    NEON_LOG_ERROR("Nav: parse failed: %s", r.Error().c_str());
                } else {
                    navGrid_ = r.Value();
                    navAssetPath_.clear();
                    navStart_ = {-5, -5};
                    navGoal_ = {-5, -5};
                    NEON_LOG_INFO("Nav: loaded '%s' (%dx%d)", navBuf, navGrid_.Width(),
                                  navGrid_.Height());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (!navGrid_.Valid()) {
                NEON_LOG_ERROR("Nav: nothing to save");
            } else {
                const std::string path =
                    navAssetPath_.empty() ? projectDir_ + "/nav/grid.json" : navAssetPath_;
                const std::string dir = ParentPath(path);
                if (!dir.empty() && dir != "." && dir != "/") MakeDirSingle(dir);
                auto json = navGrid_.ToJson();
                if (json.Ok()) {
                    std::ofstream out(path, std::ios::binary);
                    if (out.is_open()) {
                        out << core::JsonWriter::Write(json.Value());
                        navAssetPath_ = path;
                        NEON_LOG_INFO("Nav: saved -> %s", path.c_str());
                    } else {
                        NEON_LOG_ERROR("Nav: cannot write '%s'", path.c_str());
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("新建 16x16")) navGrid_ = nav::NavGrid::Create(16, 16, 1.0f, {0, 0});

        ImGui::Separator();
        ImGui::TextDisabled("左键: 翻转可行走 | Shift+左键: 起点 | Ctrl+左键: 终点");
        if (!navGrid_.Valid()) {
            ImGui::TextDisabled("未加载导航网格 (加载或新建)");
            ImGui::End();
            return;
        }
        const float cellPx = 18.0f;
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(cellPx * navGrid_.Width(), cellPx * navGrid_.Height());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int y = 0; y < navGrid_.Height(); ++y) {
            for (int x = 0; x < navGrid_.Width(); ++x) {
                const ImVec2 a(canvasOrigin.x + x * cellPx, canvasOrigin.y + y * cellPx);
                const ImVec2 b(a.x + cellPx, a.y + cellPx);
                dl->AddRectFilled(a, b, navGrid_.Walkable(x, y)
                                            ? IM_COL32(30, 90, 40, 255)
                                            : IM_COL32(150, 40, 40, 255));
                dl->AddRect(a, b, IM_COL32(20, 20, 20, 160));
            }
        }
        // A* path preview (yellow polyline through cell centers).
        if (!navPath_.empty()) {
            for (size_t i = 1; i < navPath_.size(); ++i) {
                const math::Vec2& p0 = navPath_[i - 1];
                const math::Vec2& p1 = navPath_[i];
                dl->AddLine(ImVec2(canvasOrigin.x + p0.x * cellPx,
                                   canvasOrigin.y + p0.y * cellPx),
                            ImVec2(canvasOrigin.x + p1.x * cellPx,
                                   canvasOrigin.y + p1.y * cellPx),
                            IM_COL32(255, 220, 60, 255), 3.0f);
            }
        }
        ImGui::InvisibleButton("##nav_canvas", canvasSize);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int cx = static_cast<int>((mouse.x - canvasOrigin.x) / cellPx);
            const int cy = static_cast<int>((mouse.y - canvasOrigin.y) / cellPx);
            if (navGrid_.InBounds(cx, cy)) {
                if (ImGui::GetIO().KeyShift) {
                    navStart_ = {static_cast<float>(cx), static_cast<float>(cy)};
                } else if (ImGui::GetIO().KeyCtrl) {
                    navGoal_ = {static_cast<float>(cx), static_cast<float>(cy)};
                } else {
                    navGrid_.SetWalkable(cx, cy, !navGrid_.Walkable(cx, cy));
                }
                // Recompute the path whenever the input state changes.
                navPath_.clear();
                if (navStart_.x >= 0 && navGoal_.x >= 0) {
                    navPath_ = navGrid_.FindPath(
                        navGrid_.CellToWorld(static_cast<int>(navStart_.x),
                                             static_cast<int>(navStart_.y)),
                        navGrid_.CellToWorld(static_cast<int>(navGoal_.x),
                                             static_cast<int>(navGoal_.y)));
                    // Convert world -> canvas pixel cells for the preview.
                    for (size_t i = 0; i < navPath_.size(); ++i) {
                        math::Vec2& p = navPath_[i];
                        int cx2 = 0, cy2 = 0;
                        navGrid_.WorldToCell(p, &cx2, &cy2);
                        p = {static_cast<float>(cx2) + 0.5f,
                             static_cast<float>(cy2) + 0.5f};
                    }
                }
            }
        }
        if (navStart_.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (navStart_.x + 0.5f) * cellPx,
                       canvasOrigin.y + (navStart_.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(80, 220, 255, 255));
        }
        if (navGoal_.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (navGoal_.x + 0.5f) * cellPx,
                       canvasOrigin.y + (navGoal_.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(255, 120, 80, 255));
        }
        ImGui::Text("起点 (%d,%d)  终点 (%d,%d)  路径 %zu 段",
                    static_cast<int>(navStart_.x), static_cast<int>(navStart_.y),
                    static_cast<int>(navGoal_.x), static_cast<int>(navGoal_.y),
                    navPath_.size());
    }
    ImGui::End();
}

// Data-driven UI editor (Godot/Unity-style): browse ui/*.ui.json documents,
// edit the node tree + node properties, and preview the result in the main
// viewport (1:1 design pixels). Saved JSON is consumed at runtime by the
// UIShow/UIClicked Lua bindings.
void EditorApp::BuildUIEditorPanel() {
    if (!showUIEditor_) return;
    // Give the panel a usable size the first time it opens (docking/resize
    // afterwards persists); otherwise it can float tiny and hide the fields.
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("UI 编辑器", &showUIEditor_)) {
        // --- File bar -----------------------------------------------------
        if (ImGui::Button("新建")) {
            uiDoc_ = ui::UiDocument{};
            uiSelection_.clear();
            uiDoc_.root.name = "root";
            uiDoc_.root.rect = {0, 0, 1280, 720};
            ui::UiNode* menu = uiDoc_.root.AddChild(ui::UiNodeType::Panel, "Menu");
            menu->rect = {340, 180, 600, 360};
            menu->color = {0.08f, 0.12f, 0.20f, 0.92f};
            ui::UiNode* title = menu->AddChild(ui::UiNodeType::Label, "Title");
            title->rect = {0, 20, 600, 60};
            title->text = "新界面";
            title->fontSize = 40.0f;
            ui::UiNode* startBtn = menu->AddChild(ui::UiNodeType::Button, "Start");
            startBtn->rect = {180, 200, 240, 56};
            startBtn->text = "开始";
            startBtn->color = {0.15f, 0.45f, 0.28f, 1.0f};
            ui::UiNode* bar = menu->AddChild(ui::UiNodeType::Bar, "Hp");
            bar->rect = {140, 300, 320, 20};
            bar->fill = 0.7f;
            bar->color = {0.85f, 0.25f, 0.25f, 1.0f};
            uiDocPath_ = projectDir_ + "/ui/untitled.ui.json";
            uiDocOpen_ = true;
            UISelectNode(&uiDoc_.root);
            uiDirty_ = true; // untitled: wait for the explicit 保存 button
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (uiDocOpen_ && !uiDocPath_.empty()) {
                MakeDirSingle(projectDir_ + "/ui");
                if (uiDoc_.Save(uiDocPath_)) {
                    uiDirty_ = false;
                    NEON_LOG_INFO("UI: saved '%s'", uiDocPath_.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(uiDirty_ ? "有未保存修改" : "");

        // P5-editor UX: align tools + grid snap + batch copy/delete.
        ImGui::Separator();
        if (ImGui::Button("左对齐")) UIAlignSelected(0);
        ImGui::SameLine();
        if (ImGui::Button("水平居中")) UIAlignSelected(1);
        ImGui::SameLine();
        if (ImGui::Button("右对齐")) UIAlignSelected(2);
        ImGui::SameLine();
        if (ImGui::Button("顶对齐")) UIAlignSelected(3);
        ImGui::SameLine();
        if (ImGui::Button("垂直居中")) UIAlignSelected(4);
        ImGui::SameLine();
        if (ImGui::Button("底对齐")) UIAlignSelected(5);
        ImGui::SameLine();
        ImGui::Checkbox("网格吸附", &uiSnapToGrid_);
        ImGui::SameLine();
        if (ImGui::Button("复制选中")) UIDuplicateSelectedNodes();
        ImGui::SameLine();
        if (ImGui::Button("删除选中")) UIDeleteSelectedNodes();
        if (uiSelection_.size() > 1)
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "已选中 %zu 个节点 (Ctrl 加选, 方向键微调)",
                               uiSelection_.size());
        ImGui::Separator();

        // --- Document list ------------------------------------------------
        ImGui::Separator();
        ImGui::TextDisabled("项目 UI 文档 (ui/*.ui.json)");
        uiFiles_.clear();
        std::vector<AssetEntry> entries;
        if (ListDirectory(projectDir_ + "/ui", entries)) {
            for (const AssetEntry& f : entries) {
                if (f.isDir || f.name.size() < 9 ||
                    f.name.compare(f.name.size() - 8, 8, ".ui.json") != 0)
                    continue;
                uiFiles_.push_back(f.path);
            }
        }
        std::sort(uiFiles_.begin(), uiFiles_.end());
        if (uiFiles_.empty()) {
            ImGui::TextDisabled("(无文档 — 点“新建”创建)");
        }
        for (const std::string& path : uiFiles_) {
            const bool isOpen = path == uiDocPath_;
            if (ImGui::Selectable(UiFileBaseName(path).c_str(), isOpen)) {
                if (uiDoc_.Load(path)) {
                    uiDocPath_ = path;
                    uiDocOpen_ = true;
                    uiSelection_.clear();
                    UISelectNode(&uiDoc_.root);
                    uiDirty_ = false;
                    NEON_LOG_INFO("UI: opened '%s'", path.c_str());
                }
            }
        }
        // Auto-open the first document when the panel is enabled with nothing
        // loaded, so the viewport preview is immediately usable.
        if (!uiDocOpen_ && !uiFiles_.empty()) {
            if (uiDoc_.Load(uiFiles_[0])) {
                uiDocPath_ = uiFiles_[0];
                uiDocOpen_ = true;
                uiSelection_.clear();
                UISelectNode(uiDoc_.Find("Start") ? uiDoc_.Find("Start") : &uiDoc_.root);
                uiDirty_ = false;
            }
        }

        if (!uiDocOpen_) {
            ImGui::End();
            return;
        }

        ImGui::Separator();
        ImGui::BeginChild("##ui_editor_body", ImVec2(0, 0), true);
        {
            // Left: node tree.
            ImGui::BeginChild("##ui_tree", ImVec2(230, -30), true);
            std::function<void(ui::UiNode*)> drawTree = [&](ui::UiNode* node) {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_OpenOnDoubleClick |
                ImGuiTreeNodeFlags_SpanAvailWidth;
                if (uiSelection_.count(node)) flags |= ImGuiTreeNodeFlags_Selected;
                const bool open =
                    ImGui::TreeNodeEx(node->name.c_str(), flags, "%s##%p",
                                      node->name.c_str(), static_cast<void*>(node));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    if (ImGui::GetIO().KeyCtrl)
                        UIToggleSelectNode(node);
                    else
                        UISelectNode(node);
                }
                if (open) {
                    for (auto& c : node->children) drawTree(c.get());
                    ImGui::TreePop();
                }
            };
            drawTree(&uiDoc_.root);
            ImGui::EndChild();

            // Right: properties.
            ImGui::SameLine();
            ImGui::BeginChild("##ui_props", ImVec2(0, -30), true);
            if (!uiSelected_) {
                ImGui::TextDisabled("未选择节点 — 在视口或左侧树中点击选择");
            } else {
                char nameBuf[128];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", uiSelected_->name.c_str());
                if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
                    uiSelected_->name = nameBuf;
                    MarkUIDirty();
                }
                ImGui::TextDisabled("类型: %s", ui::UiNodeTypeName(uiSelected_->type));
                ImGui::Separator();
                bool rectChanged = false;
                rectChanged |= ImGui::DragFloat("X", &uiSelected_->rect.x, 1.0f);
                rectChanged |= ImGui::DragFloat("Y", &uiSelected_->rect.y, 1.0f);
                rectChanged |= ImGui::DragFloat("宽", &uiSelected_->rect.w, 1.0f, 1.0f, 4096.0f);
                rectChanged |= ImGui::DragFloat("高", &uiSelected_->rect.h, 1.0f, 1.0f, 4096.0f);
                if (rectChanged) MarkUIDirty();

                float color[4] = {uiSelected_->color.r, uiSelected_->color.g,
                                  uiSelected_->color.b, uiSelected_->color.a};
                if (ImGui::ColorEdit4("颜色", color)) {
                    uiSelected_->color = {color[0], color[1], color[2], color[3]};
                    MarkUIDirty();
                }
                if (uiSelected_->type == ui::UiNodeType::Label ||
                    uiSelected_->type == ui::UiNodeType::Button) {
                    char textBuf[256];
                    std::snprintf(textBuf, sizeof(textBuf), "%s", uiSelected_->text.c_str());
                    if (ImGui::InputText("文本", textBuf, sizeof(textBuf))) {
                        uiSelected_->text = textBuf;
                        MarkUIDirty();
                    }
                    if (ImGui::DragFloat("字号", &uiSelected_->fontSize, 1.0f, 6.0f, 96.0f))
                        MarkUIDirty();
                }
                if (uiSelected_->type == ui::UiNodeType::Bar) {
                    if (ImGui::SliderFloat("填充", &uiSelected_->fill, 0.0f, 1.0f))
                        MarkUIDirty();
                }
                bool visible = uiSelected_->visible;
                if (ImGui::Checkbox("可见", &visible)) {
                    uiSelected_->visible = visible;
                    MarkUIDirty();
                }
            }
            ImGui::EndChild();

            // Bottom: add/delete node.
            if (uiSelected_) {
                ImGui::Separator();
                ImGui::TextDisabled("添加子节点:");
                int typeCount[5] = {0, 0, 0, 0, 0};
                for (auto& c : uiSelected_->children)
                    typeCount[static_cast<int>(c->type)] += 1;
                auto addBtn = [&](const char* label, ui::UiNodeType t) {
                    if (!ImGui::Button(label)) return;
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s_%d",
                                  ui::UiNodeTypeName(t), typeCount[static_cast<int>(t)] + 1);
                    uiSelected_ = uiSelected_->AddChild(t, name);
                    MarkUIDirty();
                };
                addBtn("面板", ui::UiNodeType::Panel);
                ImGui::SameLine();
                addBtn("文本", ui::UiNodeType::Label);
                ImGui::SameLine();
                addBtn("按钮", ui::UiNodeType::Button);
                ImGui::SameLine();
                addBtn("进度条", ui::UiNodeType::Bar);
                if (uiSelected_ != &uiDoc_.root) {
                    ImGui::SameLine();
                    if (ImGui::Button("删除节点")) {
                        ui::UiNode* doomed = uiSelected_;
                        uiSelected_ = nullptr;
                        std::function<bool(ui::UiNode*)> removeFrom =
                            [&](ui::UiNode* n) -> bool {
                            for (auto it = n->children.begin(); it != n->children.end(); ++it) {
                                if (it->get() == doomed) {
                                    n->children.erase(it);
                                    return true;
                                }
                                if (removeFrom(it->get())) return true;
                            }
                            return false;
                        };
                        removeFrom(&uiDoc_.root);
                        MarkUIDirty();
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Localization editor (Godot-style): load <project>/locales/*.json, pick the
// preview language, edit key/value pairs, save back to locales.json.
void EditorApp::BuildLocPanel() {
    if (!showLoc_) return;
    if (ImGui::Begin("本地化", &showLoc_)) {
        if (ImGui::Button("加载项目字符串表")) {
            locEdit_ = core::Localization();
            std::vector<AssetEntry> files;
            if (ListDirectory(projectDir_ + "/locales", files)) {
                for (const AssetEntry& f : files) {
                    if (f.isDir) continue;
                    const std::string& n = f.name;
                    const bool isJson =
                        n.size() > 5 && (n.compare(n.size() - 5, 5, ".json") == 0 ||
                                         n.compare(n.size() - 5, 5, ".JSON") == 0);
                    if (!isJson) continue;
                    std::ifstream in(f.path, std::ios::binary);
                    if (!in.is_open()) continue;
                    std::string text((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    std::string err;
                    if (!locEdit_.LoadTable(text, &err)) {
                        NEON_LOG_WARN("Loc: '%s' failed to load: %s", f.path.c_str(),
                                      err.c_str());
                        continue;
                    }
                    locPath_ = f.path;
                }
            }
            locEdit_.SetLanguage(locLanguage_);
            NEON_LOG_INFO("Loc: loaded project strings (%zu keys)", locEdit_.Keys().size());
        }
        ImGui::SameLine();
        if (ImGui::Button("保存 (locales.json)")) {
            const std::string dir = projectDir_ + "/locales";
            MakeDirSingle(dir);
            const std::string path = dir + "/locales.json";
            std::ofstream out(path, std::ios::binary);
            if (out.is_open()) {
                out << core::JsonWriter::Write(locEdit_.ToJson());
                locPath_ = path;
                NEON_LOG_INFO("Loc: saved -> %s", path.c_str());
            } else {
                NEON_LOG_ERROR("Loc: cannot write '%s'", path.c_str());
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("路径: %s", locPath_.c_str());
        ImGui::Separator();

        const std::vector<std::string> langs = locEdit_.Languages();
        if (!langs.empty()) {
            std::vector<const char*> langLabels;
            for (const std::string& l : langs) langLabels.push_back(l.c_str());
            int sel = 0;
            for (size_t i = 0; i < langs.size(); ++i)
                if (langs[i] == locLanguage_) sel = static_cast<int>(i);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("语言", &sel, langLabels.data(),
                             static_cast<int>(langLabels.size()))) {
                locLanguage_ = langs[static_cast<size_t>(sel)];
                locEdit_.SetLanguage(locLanguage_);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("默认: %s", locEdit_.DefaultLanguage().c_str());
        } else {
            ImGui::TextDisabled("未加载字符串表");
        }

        static char newKey[128] = {};
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputText("新键", newKey, sizeof(newKey));
        ImGui::SameLine();
        if (ImGui::Button("添加键") && newKey[0] != '\0') {
            locEdit_.Set(locLanguage_, newKey, "");
            newKey[0] = '\0';
        }
        ImGui::Separator();

        ImGui::BeginChild("##loc_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const std::vector<std::string> keys = locEdit_.Keys();
        for (const std::string& key : keys) {
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s",
                          locEdit_.GetIn(locLanguage_, key).c_str());
            const std::string label = key + "##" + locLanguage_;
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                locEdit_.Set(locLanguage_, key, buf);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// Re-scan <projectDir>/scripts/ and run a syntax check on every *.lua / *.js.
// Each file is routed to the matching throwaway host (Lua vs QuickJS);
// nothing ever runs, so a failed check leaves the host reusable.
void EditorApp::RefreshScriptChecks() {
    scriptFiles_.clear();
    scriptChecks_.clear();
    std::vector<std::string> files;
    ListScriptFiles(ScriptsDir(projectDir_), "scripts", files);
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    for (const std::string& rel : files) {
        if (script::IScriptHost* checkHost = ScriptCheckHostFor(rel)) {
            scriptChecks_.push_back(CheckScriptFile(*checkHost, base, rel));
        } else {
            ScriptCheckResult failed;
            failed.path = rel;
            failed.ok = false;
            failed.message = "脚本宿主不可用";
            scriptChecks_.push_back(failed);
        }
        scriptFiles_.push_back(rel);
    }
    if (scriptAttachIndex_ >= static_cast<int>(scriptFiles_.size()))
        scriptAttachIndex_ = static_cast<int>(scriptFiles_.size()) - 1;
    if (scriptAttachIndex_ < 0 && !scriptFiles_.empty()) scriptAttachIndex_ = 0;
}

void EditorApp::BuildScriptPanel() {
    if (!showScripts_) return;
    if (ImGui::Begin("脚本", &showScripts_)) {
        // Throttle the scripts/ scan + syntax checks: run on panel open and
        // every ~1s of frames (60 @ 60fps), never per frame.
        const uint64_t now = TimeRef().frameIndex;
        if (now - scriptRefreshFrame_ >= 60 || scriptFiles_.empty()) {
            RefreshScriptChecks();
            scriptRefreshFrame_ = now;
        }
        if (ImGui::Button("刷新检查")) RefreshScriptChecks();
        ImGui::SameLine();
        ImGui::TextDisabled("项目: %s", projectDir_.c_str());
        ImGui::Separator();

        ImGui::BeginChild("##script_list", ImVec2(0, -150.0f), ImGuiChildFlags_Borders);
        if (scriptFiles_.empty()) {
            ImGui::TextDisabled("scripts/ 目录下没有 .lua 脚本");
        }
        for (size_t i = 0; i < scriptFiles_.size(); ++i) {
            const ScriptCheckResult& r = scriptChecks_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "%s##script_%zu", scriptFiles_[i].c_str(), i);
            if (ImGui::Selectable(label, scriptAttachIndex_ == static_cast<int>(i)))
                scriptAttachIndex_ = static_cast<int>(i);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                const std::string base = projectDir_.empty() ? "." : projectDir_;
                OpenScriptEditor(base + "/" + scriptFiles_[i]);
            }
            if (r.ok) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "✗ 错误 (行 %d): %s",
                                   r.line, r.message.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::Separator();

        ImGui::TextUnformatted("附加到选中实体");
        if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
            ImGui::TextDisabled("未选中实体");
            ImGui::End();
            return;
        }
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];

        // The vars editor applies to the NEXT 附加: with a flat mounted list
        // there is no single "the" script whose vars this box would show, so
        // switching entities just resets the buffer (a too-large buffer still
        // surfaces a truncation warning).
        auto reloadVars = [&]() {
            const int n = std::snprintf(scriptVarsBuf_, sizeof(scriptVarsBuf_), "{}");
            if (static_cast<size_t>(n) >= sizeof(scriptVarsBuf_)) {
                char warn[128];
                std::snprintf(warn, sizeof(warn),
                              "变量 JSON 过大 (%.1f KB)，已截断到缓冲区上限",
                              static_cast<double>(sizeof(scriptVarsBuf_)) / 1024.0);
                scriptVarsError_ = warn;
            } else {
                scriptVarsError_.clear();
            }
        };

        // Sync the dropdown selection + vars buffer when the selected entity
        // changes (SetSelection / list mutations reset scriptSyncEntity_ so an
        // entity re-selected at the same index still re-syncs), so the panel
        // never shows a previous entity's state.
        if (scriptSyncEntity_ != selected_) {
            scriptSyncEntity_ = selected_;
            scriptAttachIndex_ = -1;
            if (scriptAttachIndex_ < 0 && !scriptFiles_.empty()) scriptAttachIndex_ = 0;
            reloadVars();
        }

        if (scriptFiles_.empty()) {
            ImGui::TextDisabled("没有可附加的脚本");
        } else {
            std::vector<const char*> names;
            names.reserve(scriptFiles_.size());
            for (const auto& f : scriptFiles_) names.push_back(f.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##script_attach", &scriptAttachIndex_, names.data(),
                         static_cast<int>(names.size()));
        }
        ImGui::TextUnformatted("变量 (JSON)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##script_vars", scriptVarsBuf_, sizeof(scriptVarsBuf_),
                                  ImVec2(-1.0f, 88.0f));
        if (!scriptVarsError_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", scriptVarsError_.c_str());
        // Mounted scripts: one list like the inspector's component section
        // (flat, no "primary"), each entry removable.
        {
            ImGui::TextDisabled("已附加 (%zu)", e.scripts.size());
            for (size_t si = 0; si < e.scripts.size(); ++si) {
                ImGui::Text("  %s", e.scripts[si].path.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(("移除##script_" + std::to_string(si)).c_str())) {
                    std::vector<SceneScriptFields> newList = e.scripts;
                    newList.erase(newList.begin() + static_cast<ptrdiff_t>(si));
                    history_.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities_, selected_, ApplyScriptList, e.scripts, newList,
                        /*mergeable=*/false));
                }
            }
        }

        const bool haveScript = !scriptFiles_.empty() && scriptAttachIndex_ >= 0 &&
                                scriptAttachIndex_ < static_cast<int>(scriptFiles_.size());
        if (ImGui::Button("附加")) {
            if (!haveScript) {
                scriptVarsError_ = "没有可附加的脚本";
            } else {
                std::string perr;
                core::Json parsed = core::Json::Parse(scriptVarsBuf_, &perr);
                if (!perr.empty()) {
                    scriptVarsError_ = "变量 JSON 无效: " + perr;
                } else if (!parsed.IsNull() && !parsed.IsObject()) {
                    scriptVarsError_ = "变量必须是 JSON 对象";
                } else {
                    std::vector<SceneScriptFields> newList = e.scripts;
                    newList.push_back(
                        {"lua", scriptFiles_[static_cast<size_t>(scriptAttachIndex_)],
                         parsed.IsNull() ? core::Json{} : parsed});
                    history_.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities_, selected_, ApplyScriptList, e.scripts, newList,
                        /*mergeable=*/false)); // one click = one undo step
                }
            }
        }
    }
    ImGui::End();
}

void EditorApp::RunPackage() {
    pack::PackConfig cfg;
    cfg.projectDir = projectDir_;
    cfg.outDir = packOutDirBuf_;
    cfg.playerSource = "build/neon_game.exe";
    packReport_ = pack::PackProject(cfg);
    packRan_ = true;
    if (packReport_.ok) {
        NEON_LOG_INFO("Editor: packaged '%s' -> %s (%zu files, %zu bytes)",
                      projectDir_.c_str(), packReport_.packPath.c_str(),
                      packReport_.fileCount, packReport_.bytesWritten);
    } else {
        NEON_LOG_ERROR("Editor: package failed for '%s' (%zu errors)",
                       projectDir_.c_str(), packReport_.errors.size());
    }
}

// Built-in script editor (Godot-style): open/save a .lua, live syntax check,
// and a one-click external-editor binding.
void EditorApp::BuildScriptEditorPanel() {
    if (!showScriptEditor_) return;
    if (ImGui::Begin("脚本编辑器", &showScriptEditor_)) {
        // Same fallback as the viewport: a session that lost the DockId (the
        // window floats over the bottom tabs / Inspector) is re-docked into
        // the bottom tab group on its first frame; a saved user layout with a
        // DockId is always restored as-is.
        if (!scriptEditorDockFallbackDone_) {
            scriptEditorDockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (dockspaceId_ && !win->DockId && !win->DockIsActive) {
                ImGuiDockNode* bottom = nullptr;
                if (ImGuiDockNode* central =
                        ImGui::DockBuilderGetCentralNode(dockspaceId_)) {
                    if (central->ParentNode && central->ParentNode->IsSplitNode()) {
                        ImGuiDockNode* parent = central->ParentNode;
                        bottom = (parent->ChildNodes[0] == central)
                                     ? parent->ChildNodes[1]
                                     : parent->ChildNodes[0];
                    }
                }
                if (bottom && !bottom->IsSplitNode()) {
                    ImGui::DockBuilderDockWindow("脚本编辑器", bottom->ID);
                    NEON_LOG_INFO(
                        "Editor: script editor DockId was lost; re-docked to the bottom tabs");
                }
            }
        }
        if (scriptEditorPath_.empty()) {
            ImGui::TextDisabled("未打开脚本 — 在资产面板或脚本面板双击 .lua 打开");
            ImGui::End();
            return;
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            SaveScriptEditor();
        ImGui::TextDisabled("文件: %s", scriptEditorPath_.c_str());
        ImGui::SameLine();
        if (ImGui::Button("保存")) SaveScriptEditor();
        ImGui::SameLine();
        if (ImGui::Button("外部编辑器打开")) OpenInExternalEditor(scriptEditorPath_);
        ImGui::SameLine();
        if (scriptEditorDirty_) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.6f, 1.0f), "已保存");
        }
        if (scriptEditorCheck_.ok) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                               "✗ 语法错误 (行 %d): %s", scriptEditorCheck_.line,
                               scriptEditorCheck_.message.c_str());
        }
        ImGui::Separator();
        // P1-2 debugger: breakpoints for the open script + live pause state.
        if (!scriptEditorPath_.empty()) {
            ImGui::TextDisabled("断点 (行号，逗号分隔)");
            ImGui::SetNextItemWidth(180.0f);
            const bool bpEnter =
                ImGui::InputText("##bp_add", breakpointLineBuf_, sizeof(breakpointLineBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("添加") || (bpEnter && breakpointLineBuf_[0] != '\0')) {
                char* p = breakpointLineBuf_;
                while (*p) {
                    while (*p == ' ' || *p == ',') ++p;
                    if (!*p) break;
                    const int line = std::atoi(p);
                    if (line > 0) scriptBreakpoints_[scriptEditorPath_].insert(line);
                    while (*p && *p != ',') ++p;
                }
                breakpointLineBuf_[0] = '\0';
                scriptBreakpointsDirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("清空断点")) {
                scriptBreakpoints_[scriptEditorPath_].clear();
                scriptBreakpointsDirty_ = true;
            }
            auto& bps = scriptBreakpoints_[scriptEditorPath_];
            if (!bps.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("当前: ");
                for (auto it = bps.begin(); it != bps.end();) {
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(*it));
                    if (ImGui::SmallButton(("行" + std::to_string(*it)).c_str())) {
                        it = bps.erase(it);
                        scriptBreakpointsDirty_ = true;
                    } else {
                        ++it;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Separator();
        }
        // P1-2 autocomplete: engine binding reference + prefix completion for
        // the word being typed.
        {
            static const struct {
                const char* name;
                const char* doc;
            } kBindings[] = {
                {"Spawn", "Spawn(kind, pos[, scriptPath]) 生成实体"},
                {"Despawn", "Despawn(entity) 销毁实体"},
                {"GetPosition", "GetPosition(entity) -> x,y,z"},
                {"SetPosition", "SetPosition(entity, x,y,z)"},
                {"GetVar", "GetVar(name) 读全局变量"},
                {"SetVar", "SetVar(name, value) 写全局变量"},
                {"Raycast", "Raycast(x,y,z, dx,dy,dz, range) -> t,owner"},
                {"PhysicsAddSphere", "PhysicsAddSphere(x,y,z, r, dynamic, {mass,...}) -> id"},
                {"PhysicsAddBox", "PhysicsAddBox(cx,cy,cz, hx,hy,hz, dynamic, {..}) -> id"},
                {"PhysicsAddCharacter", "PhysicsAddCharacter(x,y,z, r, halfH, {layer,mask}) -> id"},
                {"PhysicsSetCharacterMove", "PhysicsSetCharacterMove(id, x,y,z) 角色移动速度"},
                {"PhysicsSetVelocity", "PhysicsSetVelocity(id, x,y,z)"},
                {"PhysicsGetVelocity", "PhysicsGetVelocity(id) -> x,y,z"},
                {"PhysicsSetPosition", "PhysicsSetPosition(id, x,y,z)"},
                {"PhysicsGetPosition", "PhysicsGetPosition(id) -> x,y,z"},
                {"PhysicsIsOnGround", "PhysicsIsOnGround(id) -> bool"},
                {"PhysicsCollisions", "PhysicsCollisions() -> [{a,b},...]"},
                {"PhysicsRemove", "PhysicsRemove(id)"},
                {"Tween", "Tween(entity, prop, fx,fy,fz, tx,ty,tz, time, easing) 属性动画"},
                {"GetEntitiesInGroup", "GetEntitiesInGroup('enemy') -> 实体表"},
                {"PlaySfx", "PlaySfx(name) 播放音效"},
                {"InputAxis", "InputAxis(name) 输入轴 (-1..1)"},
                {"InputKey", "InputKey(name) -> 按键是否按下"},
                {"ActionDown", "ActionDown(name) 动作按下"},
                {"ActionPressed", "ActionPressed(name) 动作按下沿"},
                {"ActionReleased", "ActionReleased(name) 动作抬起沿"},
                {"SetRotationY", "SetRotationY(entity, radians)"},
                {"GetHealth", "GetHealth(entity) -> hp"},
                {"SetHealth", "SetHealth(entity, hp)"},
                {"SpawnProjectile", "SpawnProjectile(pos, dir, speed, damage, life, caster)"},
                {"MeleeAttack", "MeleeAttack(pos, dir, range, arcDeg, damage) -> 命中数"},
                {"CastSkill", "CastSkill(name, pos, dir, caster) -> 结果"},
                {"SkillCooldown", "SkillCooldown(name, caster) -> 剩余秒"},
                {"AttackBox", "AttackBox(center, half, yaw, damage) -> 命中数"},
                {"ApplyStatus", "ApplyStatus(entity, name, duration, magnitude)"},
                {"HasStatus", "HasStatus(entity, name) -> bool"},
                {"StatusMagnitude", "StatusMagnitude(entity, name) -> 数值"},
                {"RemoveStatus", "RemoveStatus(entity, name)"},
                {"DrawRect", "DrawRect(x,y,w,h, r,g,b,a)"},
                {"DrawSprite", "DrawSprite(tex, x,y,w,h, flipX, flipY)"},
                {"DrawText", "DrawText(text, x,y, size, r,g,b,a)"},
                {"ReadText", "ReadText(path) 读数据文件"},
                {"WriteText", "WriteText(path, content) 写数据文件"},
                {"UIShow", "UIShow(path) 显示 UI 文档"},
                {"UIHide", "UIHide() 隐藏 UI"},
                {"UIClicked", "UIClicked(name) -> 按钮是否被点击"},
                {"Loc", "Loc(key) 本地化文本"},
                {"FindNamedEntity", "FindNamedEntity(name) -> entity"},
                {"SetVisible", "SetVisible(entity, bool)"},
                {"ChangeScene", "ChangeScene(path) 切换场景"},
                {"SignalConnect", "SignalConnect(name, fn) 连接信号"},
                {"SignalEmit", "SignalEmit(name, arg) 发射信号"},
            };
            const size_t len = std::strlen(scriptEditorBuf_);
            size_t wordStart = len;
            while (wordStart > 0) {
                const char c = scriptEditorBuf_[wordStart - 1];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_'))
                    break;
                --wordStart;
            }
            const std::string word(scriptEditorBuf_ + wordStart, len - wordStart);
            if (ImGui::Button("插入第一个匹配 (Ctrl+Space)") ||
                (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false))) {
                for (const auto& b : kBindings) {
                    if (word.empty() || std::strncmp(b.name, word.c_str(), word.size()) == 0) {
                        const std::string rest = b.name + word.size();
                        if (wordStart + rest.size() < sizeof(scriptEditorBuf_)) {
                            std::memmove(scriptEditorBuf_ + wordStart + rest.size(),
                                         scriptEditorBuf_ + len,
                                         sizeof(scriptEditorBuf_) - len - wordStart);
                            std::memcpy(scriptEditorBuf_ + wordStart, rest.c_str(), rest.size());
                            scriptEditorDirty_ = true;
                        }
                        break;
                    }
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("前缀: %s", word.c_str());
            if (ImGui::CollapsingHeader("引擎绑定参考", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginChild("##binding_ref", ImVec2(0, 180), true)) {
                    for (const auto& b : kBindings) {
                        if (!word.empty() &&
                            std::strncmp(b.name, word.c_str(), word.size()) != 0)
                            continue;
                        ImGui::TextUnformatted(b.name);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", b.doc);
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
        }
        if (playtest_ && playtest_->DebuggerPaused()) {
            const script::IScriptHost::DebugFrame& f = playtest_->ScriptHost()->PausedFrame();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("⏸ 已暂停: %s 行 %d", f.script.c_str(), f.line);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("继续")) playtest_->ScriptHost()->DebuggerResume(false);
            ImGui::SameLine();
            if (ImGui::Button("单步")) playtest_->ScriptHost()->DebuggerResume(true);
            if (ImGui::CollapsingHeader("局部变量")) {
                if (f.locals.empty()) ImGui::TextDisabled("(无)");
                for (const auto& l : f.locals) {
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", l.value.c_str());
                }
            }
            if (ImGui::CollapsingHeader("调用栈")) {
                for (size_t i = 0; i < f.callstack.size(); ++i)
                    ImGui::Text("%zu. %s", i, f.callstack[i].c_str());
            }
            ImGui::Separator();
        }
        const ImVec2 editSize = ImGui::GetContentRegionAvail();
        ImGui::InputTextMultiline(
            "##script_editor", scriptEditorBuf_, sizeof(scriptEditorBuf_), editSize,
            ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemEdited()) scriptEditorDirty_ = true;
    }
    ImGui::End();
}

// P1-1 animation timeline editor: opens .anim.json clips (the anim module's
// data-driven clip format), scrubs a playhead, edits tracks/keyframes and
// saves. Playback advances the playhead but has no live mesh preview in the
// editor (clips play in the runtime animator / playtest).
void EditorApp::BuildAnimEditorPanel() {
    if (!showAnimEditor_) return;
    if (ImGui::Begin("动画时间线", &showAnimEditor_)) {
        ImGui::SetNextItemWidth(340.0f);
        ImGui::InputText("##anim_path", animPathBuf_, sizeof(animPathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("打开")) {
            std::ifstream in(animPathBuf_, std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto res = anim::LoadClipJson(text);
                if (res.Ok()) {
                    animClip_ = res.Value();
                    animClipPath_ = animPathBuf_;
                    animPlayhead_ = 0.0f;
                    animPlaying_ = false;
                    animClipDirty_ = false;
                } else {
                    NEON_LOG_ERROR("Anim editor: %s", res.Error().c_str());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && !animClipPath_.empty()) {
            if (std::ofstream out(animClipPath_, std::ios::binary); out.is_open()) {
                out << anim::SaveClipJson(animClip_);
                animClipDirty_ = false;
                NEON_LOG_INFO("Anim editor: saved '%s'", animClipPath_.c_str());
            }
        }
        if (animClipPath_.empty()) {
            ImGui::TextDisabled("未打开 clip — 输入 .anim.json 路径后点\"打开\"");
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("文件: %s", animClipPath_.c_str());
        ImGui::SameLine();
        if (animClipDirty_) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "● 未保存");

        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", animClip_.name.c_str());
        if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
            animClip_.name = nameBuf;
            animClipDirty_ = true;
        }
        if (ImGui::DragFloat("时长 (秒)", &animClip_.duration, 0.01f, 0.01f, 1000.0f)) {
            animClip_.duration = std::fmax(animClip_.duration, 0.01f);
            animClipDirty_ = true;
        }

        // Playhead transport.
        if (ImGui::Button(animPlaying_ ? "暂停" : "播放")) animPlaying_ = !animPlaying_;
        ImGui::SameLine();
        if (ImGui::Button("回到起点")) {
            animPlayhead_ = 0.0f;
            animPlaying_ = false;
        }
        ImGui::SameLine();
        if (ImGui::DragFloat("时间", &animPlayhead_, 0.01f, 0.0f, animClip_.duration)) {
            animPlayhead_ = std::fmax(0.0f, animPlayhead_);
            animClipDirty_ = true;
        }
        if (animPlaying_) {
            animPlayhead_ += ImGui::GetIO().DeltaTime;
            if (animPlayhead_ > animClip_.duration) animPlayhead_ = 0.0f;
        }

        ImGui::Separator();
        if (ImGui::Button("添加轨道")) {
            anim::Track tr;
            tr.bone = static_cast<int>(animClip_.tracks.size());
            animClip_.tracks.push_back(std::move(tr));
            animClipDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("移除最后轨道") && !animClip_.tracks.empty()) {
            animClip_.tracks.pop_back();
            animClipDirty_ = true;
        }
        ImGui::Separator();

        const char* kInterp[] = {"linear", "step", "cubic"};
        for (size_t ti = 0; ti < animClip_.tracks.size(); ++ti) {
            anim::Track& tr = animClip_.tracks[ti];
            char header[64];
            std::snprintf(header, sizeof(header), "轨道 %zu##anim_tr%zu", ti, ti);
            if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(static_cast<int>(ti));
            if (ImGui::InputInt("骨骼", &tr.bone)) animClipDirty_ = true;
            int ip = tr.interp == anim::Interp::Step
                         ? 1
                         : (tr.interp == anim::Interp::CubicSpline ? 2 : 0);
            if (ImGui::Combo("插值", &ip, kInterp, 3)) {
                tr.interp = ip == 1 ? anim::Interp::Step
                                    : (ip == 2 ? anim::Interp::CubicSpline : anim::Interp::Linear);
                animClipDirty_ = true;
            }
            // Keyframe rows: translations / rotations / scales share times.
            auto keyRow = [&](const char* label, std::vector<math::Vec3>& values, size_t comps) {
                if (values.size() != tr.times.size()) values.resize(tr.times.size());
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (ImGui::SmallButton("在此处添加关键帧")) {
                        tr.times.push_back(animPlayhead_);
                        values.push_back(comps == 4 ? math::Vec3{0, 0, 0} : math::Vec3{});
                        animClipDirty_ = true;
                    }
                    for (size_t k = 0; k < tr.times.size(); ++k) {
                        ImGui::PushID(static_cast<int>(k));
                        float t = tr.times[k];
                        if (ImGui::DragFloat("时间", &t, 0.01f, 0.0f, animClip_.duration)) {
                            tr.times[k] = std::fmax(0.0f, t);
                            animClipDirty_ = true;
                        }
                        float v[3] = {values[k].x, values[k].y, values[k].z};
                        if (ImGui::DragFloat3("值", v, 0.01f)) {
                            values[k] = {v[0], v[1], v[2]};
                            animClipDirty_ = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("删除")) {
                            tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                            values.erase(values.begin() + static_cast<ptrdiff_t>(k));
                            animClipDirty_ = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
            };
            keyRow("位移", tr.translations, 3);
            if (ImGui::TreeNodeEx("旋转##r", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (tr.rotations.size() != tr.times.size()) tr.rotations.resize(tr.times.size());
                if (ImGui::SmallButton("在此处添加关键帧")) {
                    tr.times.push_back(animPlayhead_);
                    tr.rotations.push_back({0, 0, 0, 1});
                    animClipDirty_ = true;
                }
                for (size_t k = 0; k < tr.times.size(); ++k) {
                    ImGui::PushID(static_cast<int>(k + 1000));
                    float q[4] = {tr.rotations[k].x, tr.rotations[k].y,
                                  tr.rotations[k].z, tr.rotations[k].w};
                    if (ImGui::DragFloat4("四元数", q, 0.01f)) {
                        tr.rotations[k] = {q[0], q[1], q[2], q[3]};
                        tr.rotations[k] = tr.rotations[k].Normalized();
                        animClipDirty_ = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("删除")) {
                        tr.times.erase(tr.times.begin() + static_cast<ptrdiff_t>(k));
                        tr.rotations.erase(tr.rotations.begin() + static_cast<ptrdiff_t>(k));
                        animClipDirty_ = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            keyRow("缩放", tr.scales, 3);
            ImGui::PopID();
            ImGui::Separator();
        }
    }
    ImGui::End();
}

// P1-1 terrain brush panel: select a "terrain" entity, enable 雕刻, then
// click / drag in the viewport to raise or lower the heightmap.
void EditorApp::BuildTerrainPanel() {
    if (!showTerrain_) return;
    if (ImGui::Begin("地形编辑", &showTerrain_)) {
        const bool hasTerrain =
            selected_ >= 0 && selected_ < static_cast<int>(entities_.size()) &&
            entities_[static_cast<size_t>(selected_)].meshKey == "terrain";
        if (!hasTerrain) {
            ImGui::TextDisabled("请先选中一个 \"terrain\" 实体");
            ImGui::Checkbox("雕刻模式", &terrainPaintMode_);
            ImGui::End();
            return;
        }
        ImGui::Checkbox("雕刻模式", &terrainPaintMode_);
        ImGui::SameLine();
        ImGui::Checkbox("抬高", &terrainRaise_);
        ImGui::SameLine();
        if (ImGui::Button("降低")) {
            terrainRaise_ = false;
        }
        if (ImGui::Button("重置为平地")) {
            SceneEntity& e = entities_[static_cast<size_t>(selected_)];
            e.terrainHeights_.assign(e.terrainHeights_.size(), 0.0f);
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        ImGui::DragFloat("笔刷半径", &terrainBrushRadius_, 0.1f, 0.5f, 30.0f);
        ImGui::DragFloat("笔刷强度", &terrainBrushStrength_, 0.01f, 0.005f, 2.0f);
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        if (ImGui::DragInt("细分", &e.terrainSegments_, 1, 4, 128)) {
            e.terrainSegments_ = std::max(4, std::min(e.terrainSegments_, 128));
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("尺寸", &e.terrainSize_, 1.0f, 4.0f, 500.0f)) {
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("高度缩放", &e.terrainHeightScale_, 0.05f, 0.1f, 10.0f)) {
            RebuildTerrainMesh(e);
            sceneDirty_ = true;
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("LOD / 植被 (G2-3)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::InputInt("分块数##lod", &e.chunkGridDiv_, 1, 4)) {
                e.chunkGridDiv_ = std::max(0, std::min(e.chunkGridDiv_, 16));
                sceneDirty_ = true;
            }
            if (e.chunkGridDiv_ > 0) {
                if (ImGui::InputInt("LOD 层数##lod", &e.chunkLodLevels_, 1, 1)) {
                    e.chunkLodLevels_ = std::max(1, std::min(e.chunkLodLevels_, 5));
                    sceneDirty_ = true;
                }
                if (ImGui::InputInt("LOD 细分##lod", &e.chunkBaseSubdiv_, 1, 4)) {
                    e.chunkBaseSubdiv_ = std::max(2, std::min(e.chunkBaseSubdiv_, 64));
                    sceneDirty_ = true;
                }
                ImGui::TextDisabled("播放时按分块渲染, 近密远疏 (运行时 LOD)");
            }
            static char vegKeyBuf[128] = {};
            std::snprintf(vegKeyBuf, sizeof(vegKeyBuf), "%s", e.vegMeshKey_.c_str());
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("植被网格##lod", vegKeyBuf, sizeof(vegKeyBuf))) {
                e.vegMeshKey_ = vegKeyBuf;
                sceneDirty_ = true;
            }
            int vegCount = static_cast<int>(e.vegCount_);
            if (ImGui::InputInt("植被数量##lod", &vegCount, 1, 20)) {
                e.vegCount_ = static_cast<uint32_t>(std::max(0, vegCount));
                sceneDirty_ = true;
            }
            if (e.vegCount_ > 0) {
                if (ImGui::DragFloat("植被尺寸##lod", &e.vegSize_, 0.05f, 0.1f, 8.0f)) {
                    e.vegSize_ = std::max(0.05f, e.vegSize_);
                    sceneDirty_ = true;
                }
                if (ImGui::DragFloat("Impostor 距离##lod", &e.vegImpostorDistance_, 1.0f, 1.0f, 500.0f)) {
                    e.vegImpostorDistance_ = std::max(1.0f, e.vegImpostorDistance_);
                    sceneDirty_ = true;
                }
                ImGui::TextDisabled("远处植被自动切换为 2D 面片 (Impostor)");
            }
        }
        ImGui::TextDisabled("提示: 雕刻模式下点击/拖拽视口即可塑形");
    }
    ImGui::End();
}

// P1-1 2D tilemap editor: paint cells with a texture path (or click a texture
// asset in the palette); cols/rows/cellSize resize the grid.
void EditorApp::BuildTilemapPanel() {
    if (!showTilemap_) return;
    if (ImGui::Begin("2D 地图", &showTilemap_)) {
        const bool has = selected_ >= 0 && selected_ < static_cast<int>(entities_.size()) &&
                         entities_[static_cast<size_t>(selected_)].meshKey == "tilemap";
        if (!has) {
            ImGui::TextDisabled("请先选中一个 \"tilemap\" 实体 (网格键填 tilemap)");
            ImGui::End();
            return;
        }
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        const size_t need = static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_;
        if (e.tilemapTiles_.size() != need) e.tilemapTiles_.resize(need);
        if (ImGui::DragInt("列", &e.tilemapCols_, 1, 1, 64)) {
            e.tilemapCols_ = std::max(1, std::min(e.tilemapCols_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            sceneDirty_ = true;
        }
        if (ImGui::DragInt("行", &e.tilemapRows_, 1, 1, 64)) {
            e.tilemapRows_ = std::max(1, std::min(e.tilemapRows_, 64));
            e.tilemapTiles_.resize(static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_);
            sceneDirty_ = true;
        }
        if (ImGui::DragFloat("格大小", &e.tilemapCellSize_, 1.0f, 1.0f, 512.0f)) {
            e.tilemapCellSize_ = std::max(1.0f, e.tilemapCellSize_);
            e.scale = {e.tilemapCellSize_, e.tilemapCellSize_, 1.0f};
            sceneDirty_ = true;
        }
        static char texBuf[512] = {};
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("贴图路径", texBuf, sizeof(texBuf));
        // Palette: texture assets in the current project asset dir.
        if (ImGui::CollapsingHeader("贴图调色板", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginChild("##tile_palette", ImVec2(0, 120), true)) {
                for (const AssetEntry& a : assetEntries_) {
                    if (a.isDir) continue;
                    const std::string lower = ToLower(a.name);
                    if (lower.find(".png") == std::string::npos &&
                        lower.find(".jpg") == std::string::npos)
                        continue;
                    if (ImGui::Button(a.name.c_str())) {
                        std::snprintf(texBuf, sizeof(texBuf), "%s", a.path.c_str());
                    }
                    // P2-editor UX: drag a palette tile straight onto a cell.
                    if (ImGui::BeginDragDropSource()) {
                        tileDragPath_ = a.path;
                        ImGui::SetDragDropPayload("TILE_TEXTURE", tileDragPath_.data(),
                                                  tileDragPath_.size() + 1);
                        ImGui::Text("放置: %s", a.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::TextDisabled("点击格子放置当前贴图; 右键格子弹窗菜单请先清除文本后点击");
        const float cellW = std::max(ImGui::GetContentRegionAvail().x /
                                         static_cast<float>(std::max(e.tilemapCols_, 1)) - 4.0f,
                                     20.0f);
        for (int r = 0; r < e.tilemapRows_; ++r) {
            for (int c = 0; c < e.tilemapCols_; ++c) {
                size_t idx = static_cast<size_t>(r) * e.tilemapCols_ + c;
                ImGui::PushID(static_cast<int>(idx));
                std::string label = e.tilemapTiles_[idx].empty()
                                        ? "·"
                                        : "■";
                if (ImGui::Button(label.c_str(), ImVec2(cellW, 24))) {
                    if (texBuf[0] != '\0') {
                        e.tilemapTiles_[idx] = texBuf;
                        sceneDirty_ = true;
                    } else {
                        e.tilemapTiles_[idx].clear();
                        sceneDirty_ = true;
                    }
                }
                // P2-editor UX: drop a palette tile or an asset texture here.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TILE_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            sceneDirty_ = true;
                        }
                    } else if (const ImGuiPayload* p =
                                   ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                        const char* path = static_cast<const char*>(p->Data);
                        if (path && *path) {
                            e.tilemapTiles_[idx] = path;
                            sceneDirty_ = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && !e.tilemapTiles_[idx].empty())
                    ImGui::SetTooltip("%s", e.tilemapTiles_[idx].c_str());
                if ((c + 1) % e.tilemapCols_ != 0) ImGui::SameLine();
                ImGui::PopID();
            }
        }
        if (ImGui::Button("清空地图")) {
            for (std::string& t : e.tilemapTiles_) t.clear();
            sceneDirty_ = true;
        }
    }
    ImGui::End();
}

void EditorApp::BuildPackagePanel() {
    if (!showPackage_) return;
    if (ImGui::Begin("打包", &showPackage_)) {
        ImGui::TextDisabled("项目目录 (game.json + scenes/ + prefabs/ + behaviors/ + scripts/ + assets/)");
        if (ImGui::InputText("##pack_proj", projectDirBuf_, sizeof(projectDirBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            projectDir_ = projectDirBuf_;
            if (projectDir_.empty()) projectDir_ = ".";
            SaveEditorConfig();
        }
        ImGui::TextDisabled("输出目录");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##pack_out", packOutDirBuf_, sizeof(packOutDirBuf_));
        ImGui::Separator();
        // Export presets (Godot-style): edit game.json's "export" block.
        ImGui::TextUnformatted("导出配置 (game.json \"export\")");
        static std::string expProj;
        static char expPlatform[64] = "windows";
        static char expIcon[512] = {};
        static char expDesc[512] = {};
        if (expProj != projectDir_) {
            expProj = projectDir_;
            std::strcpy(expPlatform, "windows");
            expIcon[0] = '\0';
            expDesc[0] = '\0';
            std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                std::string err;
                core::Json root = core::Json::Parse(text, &err);
                if (const core::Json* ex = root.Get("export")) {
                    if (const core::Json* p = ex->Get("platform"))
                        std::strncpy(expPlatform, p->GetString().c_str(),
                                     sizeof(expPlatform) - 1);
                    if (const core::Json* i = ex->Get("icon"))
                        std::strncpy(expIcon, i->GetString().c_str(), sizeof(expIcon) - 1);
                    if (const core::Json* d = ex->Get("description"))
                        std::strncpy(expDesc, d->GetString().c_str(), sizeof(expDesc) - 1);
                }
            }
        }
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputText("平台", expPlatform, sizeof(expPlatform));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("图标", expIcon, sizeof(expIcon));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("说明", expDesc, sizeof(expDesc));
        if (ImGui::Button("保存导出配置")) {
            std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            std::string err;
            core::Json root = core::Json::Parse(text, &err);
            if (!root.IsObject()) {
                NEON_LOG_ERROR("Export: cannot read '%s/game.json'", projectDir_.c_str());
            } else {
                core::Json ex;
                ex.type_ = core::Json::Type::Object;
                auto str = [](const char* s) {
                    core::Json j;
                    j.type_ = core::Json::Type::String;
                    j.string_ = s;
                    return j;
                };
                ex.object_["platform"] = str(expPlatform);
                if (expIcon[0]) ex.object_["icon"] = str(expIcon);
                if (expDesc[0]) ex.object_["description"] = str(expDesc);
                root.object_["export"] = ex;
                std::ofstream out(projectDir_ + "/game.json", std::ios::binary);
                if (out.is_open()) {
                    out << core::JsonWriter::Write(root);
                    NEON_LOG_INFO("Export: preset saved -> %s/game.json", projectDir_.c_str());
                } else {
                    NEON_LOG_ERROR("Export: cannot write '%s/game.json'", projectDir_.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("windows | linux | macos | web");
        ImGui::Separator();
        if (ImGui::Button("一键打包")) RunPackage();
        ImGui::SameLine();
        ImGui::TextDisabled("生成 game.pack + run.bat + neon_game.exe");
        ImGui::Separator();

        if (packRan_) {
            if (packReport_.ok) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "打包成功");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "打包失败 (%zu 个错误)",
                                   packReport_.errors.size());
            }
            ImGui::TextDisabled("文件: %zu    字节: %zu", packReport_.fileCount,
                                packReport_.bytesWritten);
            if (!packReport_.packPath.empty()) ImGui::TextUnformatted(packReport_.packPath.c_str());
            if (!packReport_.runScriptPath.empty())
                ImGui::TextUnformatted(packReport_.runScriptPath.c_str());
            if (!packReport_.playerPath.empty())
                ImGui::TextUnformatted(packReport_.playerPath.c_str());
            if (!packReport_.errors.empty()) {
                ImGui::TextUnformatted("错误:");
                for (const std::string& e : packReport_.errors)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "- %s", e.c_str());
            }
            if (!packReport_.warnings.empty()) {
                ImGui::TextUnformatted("警告:");
                for (const std::string& w : packReport_.warnings)
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "- %s", w.c_str());
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled(
            "neon_game.exe 为数据驱动播放器（读取 game.pack）。\n"
            "若尚未构建 neon_game，播放器复制会给出警告，打包仍会生成 game.pack 与 run.bat。");
    }
    ImGui::End();
}

void EditorApp::OpenModelPreview(const std::string& path) {
    std::string p = path;
    if (p.rfind("gltf:", 0) == 0) p = p.substr(5);
    if (p.empty()) return;
    core::Result<scene::SkinnedModel> sm = scene::LoadSkinnedModel(assetMgr_, p);
    if (sm.Ok()) {
        previewModel_ = std::make_shared<scene::SkinnedModel>(std::move(sm.Value()));
        previewPath_ = p;
        previewPlaying_ = true;
        previewTime_ = 0.0f;
        previewClip_ = previewModel_->defaultClip >= 0 ? previewModel_->defaultClip : 0;
        NEON_LOG_INFO("Model preview: '%s' (%zu parts, %zu clips, %zu bones)", p.c_str(),
                      previewModel_->parts.size(), previewModel_->clips.size(),
                      previewModel_->skeleton.bones.size());
    } else {
        NEON_LOG_ERROR("Model preview: failed to load '%s': %s", p.c_str(),
                       sm.Error().c_str());
    }
}

void EditorApp::RenderModelPreviewPanel() {
    if (!showModelPreview_ || !previewModel_) return;
    gfx::IRenderBackend* b = renderer_.Backend();
    if (!b) return;
    const int w = static_cast<int>(previewScreenRect_.w);
    const int h = static_cast<int>(previewScreenRect_.h);
    if (w < 8 || h < 8) return;

    if (!previewRT_.Valid() || previewRTW_ != w || previewRTH_ != h) {
        if (previewRT_.Valid()) b->DestroyRenderTarget(previewRT_);
        if (previewRTId_ != ImTextureID_Invalid) {
            gfx::ImGuiNeon_UnregisterTexture(previewRTColor_);
            previewRTId_ = ImTextureID_Invalid;
        }
        previewRT_ = b->CreateRenderTarget(w, h, true, 0);
        previewRTColor_ = b->RenderTargetColorTexture(previewRT_);
        previewRTW_ = w;
        previewRTH_ = h;
        if (previewRTColor_.Valid())
            previewRTId_ = gfx::ImGuiNeon_RegisterTexture(previewRTColor_);
    }
    if (!previewRT_.Valid()) return;

    b->BindRenderTarget(previewRT_);
    b->Clear({0.30f, 0.34f, 0.40f, 1.0f}, 1.0f);

    math::AABB bounds;
    bool first = true;
    for (const scene::SkinnedModel::Part& part : previewModel_->parts) {
        const math::AABB pb = part.mesh.Bounds();
        if (first) {
            bounds = pb;
            first = false;
        } else {
            bounds.min = {std::min(bounds.min.x, pb.min.x), std::min(bounds.min.y, pb.min.y),
                          std::min(bounds.min.z, pb.min.z)};
            bounds.max = {std::max(bounds.max.x, pb.max.x), std::max(bounds.max.y, pb.max.y),
                          std::max(bounds.max.z, pb.max.z)};
        }
    }
    const math::Vec3 center = (bounds.min + bounds.max) * 0.5f;
    const float size =
        std::max({bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                  bounds.max.z - bounds.min.z, 0.5f});
    gfx::Camera pcam;
    pcam.position = center +
                    math::Vec3{std::sin(previewYaw_) * std::cos(previewPitch_),
                               std::sin(previewPitch_),
                               std::cos(previewYaw_) * std::cos(previewPitch_)} *
                        size * 2.6f;
    pcam.target = center;
    renderer_.SetCamera(pcam, static_cast<float>(w) / static_cast<float>(h));
    renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.9f}, 0.5f);
    anim::Pose pose = previewModel_->skeleton.BindPose();
    if (previewClip_ >= 0 &&
        previewClip_ < static_cast<int>(previewModel_->clips.size())) {
        const anim::AnimationClip& clip =
            previewModel_->clips[static_cast<size_t>(previewClip_)];
        clip.Sample(previewTime_, pose);
    }
    const std::vector<math::Mat4> bones =
        previewModel_->skeleton.ComputeBoneMatrices(pose);
    for (const scene::SkinnedModel::Part& part : previewModel_->parts)
        renderer_.DrawSkinnedMesh(part.mesh, part.material, math::Mat4::Identity(), bones,
                                  static_cast<int>(bones.size()));
    b->BindDefaultTarget();
}

void EditorApp::BuildModelPreviewPanel() {
    if (!showModelPreview_) return;
    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("模型查看器", &showModelPreview_)) {
        static char pathBuf[512] = "assets/models/wolf/Wolf-Blender-2.82a.gltf";
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("路径", pathBuf, sizeof(pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) OpenModelPreview(pathBuf);
        ImGui::Separator();

        if (!previewModel_) {
            ImGui::TextDisabled("未打开模型 — 输入 .gltf 路径后点\"打开\"（右键拖拽旋转）");
            ImGui::End();
            return;
        }

        ImGui::Text("%s", previewPath_.c_str());
        ImGui::TextDisabled("%zu 部件 | %zu 骨骼 | %zu 动画",
                            previewModel_->parts.size(), previewModel_->skeleton.bones.size(),
                            previewModel_->clips.size());
        if (!previewModel_->clips.empty()) {
            std::vector<const char*> names;
            for (const anim::AnimationClip& c : previewModel_->clips) names.push_back(c.name.c_str());
            if (previewClip_ < 0 || previewClip_ >= static_cast<int>(names.size()))
                previewClip_ = 0;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("动画", &previewClip_, names.data(),
                             static_cast<int>(names.size()))) {
                previewTime_ = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button(previewPlaying_ ? "暂停" : "播放")) previewPlaying_ = !previewPlaying_;
            float t = previewTime_;
            if (ImGui::SliderFloat("时间", &t, 0.0f,
                                   std::max(0.01f, previewModel_->clips[static_cast<size_t>(previewClip_)].duration))) {
                previewTime_ = t;
            }
        }
        if (ImGui::Button("重置视角")) {
            previewYaw_ = 0.6f;
            previewPitch_ = 0.3f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("预览区右键拖拽旋转");

        // Preview area fills the panel's remaining space. Its screen rect is
        // consumed by RenderModelPreviewPanel (drawn after the main scene so
        // it coexists with the edit/playtest viewport).
        ImGui::BeginChild("##preview_area", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        previewScreenRect_ = {pos.x, pos.y, sz.x, sz.y};
        if (previewRTId_ != ImTextureID_Invalid)
            // FBO color texture is bottom-left origin; flip V so it displays
            // upright in ImGui (Image assumes top-left).
            ImGui::Image(previewRTId_, ImVec2(sz.x, sz.y), ImVec2(0.0f, 1.0f),
                         ImVec2(1.0f, 0.0f));
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace neon::editor
