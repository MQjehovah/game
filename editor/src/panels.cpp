#include "editor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <functional>
#include <cstdio>
#include <fstream>
#if defined(_WIN32)
#include <direct.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#include "editor_history.hpp"
#include "imgui_internal.h"
#include "neon/gfx/imgui_neon.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neon::editor {
namespace {

std::string TypeLabel(const std::string& key) {
    if (key == "terrain") return "地形";
    if (key == "helmet") return "头盔 (glTF PBR)";
    if (key == "cube") return "方块";
    if (key == "tree") return "松树 (OBJ)";
    if (key.rfind("obj:", 0) == 0) return "OBJ 模型";
    if (key.rfind("gltf:", 0) == 0) return "glTF 模型";
    return key;
}

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
        "脚本 (*.lua)\0*.lua\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn)) return buf;
#else
    (void)0;
#endif
    return {};
}

std::string GetCurrentDir();
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
    return ext == ".lua";
}

// Material ball asset: materials/*.mat.json.
bool IsMaterialExt(const std::string& name) {
    const std::string lower = ToLower(name);
    return lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".mat.json") == 0;
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
    std::string normBase = base;
    if (normBase.back() != '/' && normBase.back() != '\\') normBase += '/';
    std::string normPath = path;
    if (normPath.rfind(normBase, 0) == 0) return normPath.substr(normBase.size());
    // Also match with a leading "./".
    std::string dotBase = "./" + base;
    if (normPath.rfind(dotBase, 0) == 0)
        return normPath.substr(dotBase.size() + 1); // skip "./" + base + "/"
    return path;
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

std::string GetCurrentDir() {
    char buf[4096];
#if defined(_WIN32)
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    if (::getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
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

// Native folder picker for importing a whole resource directory (model +
// textures + subfolders). Non-Windows hosts fall back to the path input row.
std::string PickImportDir() {
#if defined(_WIN32)
    std::string out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd)))) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = WideToUtf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        pfd->Release();
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
// script, 2 = JSON, 3 = empty text file.
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
    } else {
        NEON_LOG_INFO("Editor: '%s' (%llu bytes) - no import action",
                      path.c_str(), static_cast<unsigned long long>(0));
    }
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
        const char* types[] = {"地形", "头盔", "方块", "松树"};
        ImGui::SetNextItemWidth(86.0f);
        ImGui::Combo("##addtype", &addType, types, 4);
        ImGui::SameLine();
        if (ImGui::Button("添加")) {
            const char* keys[] = {"terrain", "helmet", "cube", "tree"};
            AddEntity(keys[addType]);
        }
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
        if (ImGui::Button("复制") && selected_ >= 0) {
            history_.Push(std::make_unique<DuplicateEntityCommand>(
                &entities_, static_cast<size_t>(selected_)));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("删除") && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size())) {
            history_.Push(std::make_unique<DeleteEntityCommand>(
                &entities_, static_cast<size_t>(selected_)));
            if (selected_ >= static_cast<int>(entities_.size()))
                SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("↑") && selected_ > 0) {
            history_.Push(std::make_unique<ReorderEntityCommand>(
                &entities_, static_cast<size_t>(selected_), static_cast<size_t>(selected_ - 1)));
            SetSelection(selected_ - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("↓") && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()) - 1) {
            history_.Push(std::make_unique<ReorderEntityCommand>(
                &entities_, static_cast<size_t>(selected_), static_cast<size_t>(selected_ + 1)));
            SetSelection(selected_ + 1);
        }
        ImGui::Separator();
        ImGui::BeginChild("##scene_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        // Godot-style scene tree: entities group under their "parent" name
        // (root = empty/missing parent). Drag one row onto another to reparent;
        // drag onto the empty area to detach back to root.
        {
            std::map<std::string, std::vector<int>> childrenByParent;
            std::set<std::string> names;
            for (const SceneEntity& e : entities_) names.insert(e.name);
            for (size_t i = 0; i < entities_.size(); ++i)
                childrenByParent[entities_[i].parent].push_back(static_cast<int>(i));
            auto reparent = [this](int from, const std::string& toParent) {
                if (from < 0 || from >= static_cast<int>(entities_.size())) return;
                const std::string oldParent = entities_[static_cast<size_t>(from)].parent;
                if (oldParent == toParent) return;
                entities_[static_cast<size_t>(from)].parent = toParent;
                history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities_, from, ApplyParentProp, oldParent, toParent,
                    /*mergeable=*/false));
            };
            std::function<void(const std::string&)> drawNode = [&](const std::string& parentName) {
                const auto it = childrenByParent.find(parentName);
                if (it == childrenByParent.end()) return;
                for (int idx : it->second) {
                    const SceneEntity& e = entities_[static_cast<size_t>(idx)];
                    char label[256];
                    std::snprintf(label, sizeof(label), "%s%s##scene_%d", e.name.c_str(),
                                  e.prefab.empty() ? "" : " (预置体)", idx);
                    const bool hasChildren = childrenByParent.count(e.name) != 0;
                    if (hasChildren) {
                        const bool open = ImGui::TreeNodeEx(
                            label, ImGuiTreeNodeFlags_OpenOnArrow |
                                       (selected_ == idx ? ImGuiTreeNodeFlags_Selected : 0));
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                            SetSelection(idx);
                        if (open) {
                            drawNode(e.name);
                            ImGui::TreePop();
                        }
                    } else {
                        if (ImGui::Selectable(label, selected_ == idx)) SetSelection(idx);
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("SCENE_ENTITY", &idx, sizeof(int));
                        ImGui::Text("移动 %s", e.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p =
                                ImGui::AcceptDragDropPayload("SCENE_ENTITY")) {
                            int from = 0;
                            std::memcpy(&from, p->Data, sizeof(from));
                            reparent(from, e.name);
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
            };
            drawNode("");
            // Detach target: drag an entity here to clear its parent.
            ImGui::TextDisabled("(拖到此处取消父子关系)");
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SCENE_ENTITY")) {
                    int from = 0;
                    std::memcpy(&from, p->Data, sizeof(from));
                    reparent(from, "");
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
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
                const char* path = static_cast<const char*>(p->Data);
                if (path && selected_ >= 0 &&
                    selected_ < static_cast<int>(entities_.size())) {
                    const SceneScriptFields oldV{entities_[static_cast<size_t>(selected_)].scriptBackend,
                                                 entities_[static_cast<size_t>(selected_)].scriptPath,
                                                 entities_[static_cast<size_t>(selected_)].scriptVars};
                    const SceneScriptFields newV{"lua", ToProjectRelPath(path, projectDir_), oldV.vars};
                    history_.Push(std::make_unique<EditPropertyCommand<SceneScriptFields>>(
                        &entities_, selected_, ApplyScriptFields, oldV, newV,
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
    if (ImGui::Begin("资产", &showAssets_)) {
        if (ImGui::SmallButton("..")) {
            std::string parent = ParentPath(assetDir_);
            if (parent != assetDir_) {
                assetDir_ = parent;
                RefreshAssetDir();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("项目assets")) {
            assetDir_ = projectDir_ + "/assets";
            MakeDirSingle(assetDir_);
            RefreshAssetDir();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("项目根")) {
            assetDir_ = projectDir_;
            RefreshAssetDir();
        }
        ImGui::SameLine();
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
        // Project resource-dir quick nav (Unity-style project folders): jump
        // straight into assets/ / materials/ / scenes/ / scripts/ / prefabs/...
        const char* kProjDirs[] = {"assets", "materials", "scenes", "scripts",
                                   "prefabs", "behaviors", "nav", "locales"};
        for (int di = 0; di < static_cast<int>(sizeof(kProjDirs) / sizeof(kProjDirs[0])); ++di) {
            const char* d = kProjDirs[di];
            const std::string full = projectDir_ + "/" + d;
            if (!IsDirPath(full)) continue;
            ImGui::SameLine();
            if (ImGui::SmallButton(d)) {
                assetDir_ = full;
                RefreshAssetDir();
            }
        }
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
            static const char* kinds[] = {"目录", "Lua 脚本", "JSON 文件", "文本文件", "材质球"};
            static char newName[128] = {};
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##new_kind", &newKind, kinds, 5)) {
                // hint defaults per kind
                if (newKind == 1) std::strncpy(newName, "new_script.lua", sizeof(newName) - 1);
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
            int visible = 0;
            for (size_t i = 0; i < assetEntries_.size(); ++i) {
                const AssetEntry& e = assetEntries_[i];
                if (!AssetMatchesFilter(e, assetFilter_)) continue;
                const int col = visible % cols;
                const int row = visible / cols;
                ++visible;
                ImGui::SetCursorPos(ImVec2(col * cellW, row * cellH));
                const std::string id = "##acell_" + std::to_string(i);
                if (ImGui::InvisibleButton(id.c_str(), ImVec2(cellW - 6.0f, cellH - 8.0f))) {
                    selectedAsset_ = static_cast<int>(i);
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
                    dl->AddImage(tid, thumbTl, thumbBr, ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(1.0f, flipV ? 0.0f : 1.0f));
                    dl->AddRect(thumbTl, thumbBr, IM_COL32(30, 30, 35, 255));
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
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            const AssetEntry& e = assetEntries_[i];
            if (!AssetMatchesFilter(e, assetFilter_)) continue;
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##asset_%zu",
                          e.isDir ? "[D] " : "    ", e.name.c_str(), i);
            if (ImGui::Selectable(label, selectedAsset_ == static_cast<int>(i))) {
                selectedAsset_ = static_cast<int>(i);
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
                    gfx::Texture tex = assetMgr_.LoadTexture(e.path);
                    if (tex.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(tex.Handle());
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
                    ImGui::Text("%s", kv.first.c_str());
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
        // Scene-tree parent: a combo of every entity name + "" (root).
        {
            std::vector<const char*> names;
            std::vector<std::string> storage;
            storage.push_back("");
            for (const SceneEntity& other : entities_) {
                if (other.name == e.name) continue; // no self-parenting
                storage.push_back(other.name);
            }
            for (const std::string& s : storage) names.push_back(s.c_str());
            int sel = 0;
            for (size_t i = 0; i < storage.size(); ++i)
                if (storage[i] == e.parent) sel = static_cast<int>(i);
            if (ImGui::Combo("父实体", &sel, names.data(), static_cast<int>(names.size()))) {
                const std::string newParent = storage[static_cast<size_t>(sel)];
                if (newParent != e.parent) {
                    const std::string oldParent = e.parent;
                    e.parent = newParent;
                    history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                        &entities_, selected_, ApplyParentProp, oldParent, e.parent));
                }
            }
        }
        ImGui::TextDisabled("类型: %s", TypeLabel(e.meshKey).c_str());
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
        const gfx::Color oldTint = e.tint;
        if (ImGui::ColorEdit3("颜色", &e.tint.r)) {
            e.material.tint = e.tint;
            history_.Push(std::make_unique<EditPropertyCommand<gfx::Color>>(
                &entities_, selected_, ApplyColorProp, oldTint, e.tint));
        }
        ImGui::Separator();
        ImGui::TextUnformatted("材质");
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
        ImGui::Separator();
        ImGui::TextUnformatted("纹理 (拖入资产面板的图片)");
        // One slot per PBR texture: thumbnail preview, editable path (Enter to
        // commit), clear button, and a drag-drop target from the asset panel.
        // Every change routes through the undo history as a texture-slot edit.
        auto textureSlot = [this, &e](const char* label, std::string& path,
                                      gfx::TextureHandle& handle,
                                      void (*apply)(SceneEntity&, const TextureSlotValue&)) {
            char rowId[128];
            std::snprintf(rowId, sizeof(rowId), "##slot_%s", label);
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::BeginChild(rowId, ImVec2(-1.0f, 32.0f), ImGuiChildFlags_Borders);
            {
                ImTextureID tid = ImTextureID_Invalid;
                if (handle.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(handle);
                const ImVec2 previewSize(26.0f, 26.0f);
                if (tid != ImTextureID_Invalid) {
                    ImGui::Image(tid, previewSize);
                } else {
                    ImGui::Dummy(previewSize);
                }
                ImGui::SameLine();
                char buf[2048];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                ImGui::SetNextItemWidth(-64.0f);
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
            }
            ImGui::EndChild();
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
        };
        textureSlot("漫反射", e.albedoTex, e.material.albedo, ApplyAlbedoTexSlot);
        textureSlot("金属度/粗糙度", e.mrTex, e.material.metallicRoughness, ApplyMRTexSlot);
        textureSlot("环境光遮蔽图", e.aoTex, e.material.occlusion, ApplyAOTexSlot);
        textureSlot("自发光图", e.emissiveTex, e.material.emissive, ApplyEmissiveTexSlot);
        ImGui::Separator();
        ImGui::TextUnformatted("网格");
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
        // Schema-driven component editor (Godot @export / UE UPROPERTY style):
        // every component stored in extraComponents renders its registered
        // fields; components without a schema show their raw JSON read-only.
        // plant/zombie are skipped - the 2D canvas is their editor.
        ImGui::Separator();
        ImGui::TextUnformatted("组件");
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
        for (auto& [compName, compData] : e.extraComponents) {
            if (compName == "plant" || compName == "zombie") continue; // 2D canvas edits
            const scene::ComponentSchema* schema = scene::FindComponentSchema(compName);
            const std::string header =
                schema ? (schema->label + "##" + compName) : (compName + "##raw");
            if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                continue;
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
                               ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("视口", nullptr, vpFlags)) {
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
        ImGui::TextDisabled("%s | 实体 %zu | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f", camLabel,
                            entities_.size(), camTarget_.x, camTarget_.y, camTarget_.z,
                            camDist_);

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

// Re-scan <projectDir>/scripts/ and run a syntax check on every *.lua. Reuses
// one throwaway LuaHost across all checks (nothing ever runs, so a failed check
// leaves the host fully usable for the next one).
void EditorApp::RefreshScriptChecks() {
    scriptFiles_.clear();
    scriptChecks_.clear();
    if (!scriptCheckHost_) {
        scriptCheckHost_ = script::CreateLuaHost();
        if (scriptCheckHost_) scriptCheckHost_->Init();
    }
    std::vector<std::string> files;
    ListLuaFiles(ScriptsDir(projectDir_), "scripts", files);
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    for (const std::string& rel : files) {
        if (scriptCheckHost_) {
            scriptChecks_.push_back(CheckScriptFile(*scriptCheckHost_, base, rel));
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

        // Reload the vars editor from the entity's attached vars. Detects JSON
        // larger than the buffer so a silent truncation surfaces a warning.
        auto reloadVars = [&]() {
            const std::string json = e.scriptVars.IsObject()
                                         ? core::JsonWriter::Write(e.scriptVars)
                                         : "{}";
            const int n = std::snprintf(scriptVarsBuf_, sizeof(scriptVarsBuf_), "%s",
                                        json.c_str());
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
        // always reflects the attached script (if any).
        if (scriptSyncEntity_ != selected_) {
            scriptSyncEntity_ = selected_;
            scriptAttachIndex_ = -1;
            for (size_t i = 0; i < scriptFiles_.size(); ++i)
                if (scriptFiles_[i] == e.scriptPath) scriptAttachIndex_ = static_cast<int>(i);
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
        if (!e.scriptPath.empty())
            ImGui::TextDisabled("已附加: %s", e.scriptPath.c_str());

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
                    const SceneScriptFields oldV{e.scriptBackend, e.scriptPath, e.scriptVars};
                    const SceneScriptFields newV{
                        "lua", scriptFiles_[static_cast<size_t>(scriptAttachIndex_)],
                        parsed.IsNull() ? core::Json{} : parsed};
                    history_.Push(std::make_unique<EditPropertyCommand<SceneScriptFields>>(
                        &entities_, selected_, ApplyScriptFields, oldV, newV,
                        /*mergeable=*/false)); // one click = one undo step
                    reloadVars();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("分离") && !e.scriptPath.empty()) {
            const SceneScriptFields oldV{e.scriptBackend, e.scriptPath, e.scriptVars};
            const SceneScriptFields emptyV;
            history_.Push(std::make_unique<EditPropertyCommand<SceneScriptFields>>(
                &entities_, selected_, ApplyScriptFields, oldV, emptyV,
                /*mergeable=*/false));
            reloadVars(); // e.scriptVars is now null -> buffer shows "{}"
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

} // namespace neon::editor
