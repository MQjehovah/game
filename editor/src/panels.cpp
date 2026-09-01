#include "editor.hpp"
#include "editor_util.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/assets/asset_path.hpp"
#include "neon/assets/mesh_format.hpp"

#include <algorithm>
#include <thread>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <functional>
#include <cstdio>
#include <fstream>
#include "neon/core/mem_stats.hpp"
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
// project-relative path — defined in editor_util.cpp (shared with the editor).

namespace {

// Mesh-key 显示标签（TypeLabel）原在此处（匿名命名空间），仅属性面板的
// EntityTypeLabel 使用——已随属性面板迁移进 panels/inspector_panel.cpp（Task 4）。

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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

// --- 资产面板/导入共享工具（原匿名命名空间助手） ----------------------------
// AssetPanel（editor/src/panels/asset_panel.cpp）迁移时从上面的匿名命名空间
// 提升到外部链接（声明在 editor_util.hpp），实现保持在本 TU 与原实现逐行一致。

bool IsImageExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

bool IsModelExt(const std::string& name) {
    // Any registered mesh format (obj/gltf/glb/fbx/...) is a model. New formats
    // registered in the mesh-format registry are auto-recognized here.
    return !assets::MeshFormatRegistry::Instance().FormatFromExt(name).empty();
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

std::string ParentPath(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return p;
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
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
std::string PickImportFile(void* owner) {
    // 主线程同步执行 (模态循环自己泵消息); owner 已设置, 对话框正常置顶。
    std::string out;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comHere = SUCCEEDED(hrInit);
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IFileDialog), reinterpret_cast<void**>(&dialog)))) {
        // Build the model extension filter from the mesh-format registry so new
        // formats appear in the import dialog automatically.
        std::string modelPattern;
        for (const std::string& ext : assets::MeshFormatRegistry::Instance().Extensions())
            modelPattern += "*" + ext + ";";
        std::wstring allAssets = L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;" +
                                 std::wstring(modelPattern.begin(), modelPattern.end()) +
                                 L"*.lua;*.js;*.json;*.wav;*.mp3";
        const COMDLG_FILTERSPEC filters[] = {
            { L"所有支持的资产", allAssets.c_str() },
            { L"所有文件 (*.*)", L"*.*" },
        };
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(L"选择要导入的文件");
        if (SUCCEEDED(dialog->Show(owner ? static_cast<HWND>(owner) : nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    int wlen = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                    if (wlen > 0) {
                        std::vector<char> bufA(static_cast<size_t>(wlen));
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, bufA.data(), wlen, nullptr, nullptr);
                        out.assign(bufA.data());
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            dialog->Release();
        }
    }
    if (comHere) CoUninitialize();
    if (out.empty()) NEON_LOG_INFO("Editor: file picker cancelled");
    return out;
}

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
std::string PickImportDir(void* owner) {
    std::string out;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comHere = SUCCEEDED(hrInit);
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IFileDialog), reinterpret_cast<void**>(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);
        dialog->SetTitle(L"选择要导入的资源目录");
        if (SUCCEEDED(dialog->Show(owner ? static_cast<HWND>(owner) : nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    int wlen = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                    if (wlen > 0) {
                        std::vector<char> bufA(static_cast<size_t>(wlen));
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, bufA.data(), wlen, nullptr, nullptr);
                        out.assign(bufA.data());
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            dialog->Release();
        }
    }
    if (comHere) CoUninitialize();
    if (out.empty()) NEON_LOG_INFO("Editor: folder picker cancelled");
    return out;
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

// 资产操作方法（RefreshAssetDir/ImportAssetFile/CreateAssetFile/ImportAssetPath/
// DeleteSelectedAsset/ImportSelectedAsset）实现在 panels_assets.inc（EditorApp
// 成员；Task 3 迁移 AssetPanel 时保留在 EditorApp，经 ctx 回调访问）。
#include "panels_assets.inc"

} // namespace neon::editor
