// 素材库面板实现 — 扫描本地目录 → 列出资产 → 一键导入工程
#include "panels/asset_library_panel.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include "editor.hpp"

namespace neon::editor {
namespace {

constexpr const char* kCategories[] = {"模型", "贴图", "音频", "其他"};

bool IsModel(const std::string& ext) {
    return ext == ".obj" || ext == ".glb" || ext == ".gltf" || ext == ".fbx";
}
bool IsTexture(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" ||
           ext == ".bmp" || ext == ".webp";
}
bool IsAudio(const std::string& ext) {
    return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
}
std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string CategoryOf(const std::string& ext) {
    if (IsModel(ext)) return "models";
    if (IsTexture(ext)) return "textures";
    if (IsAudio(ext)) return "audio";
    return "misc";
}
int CategoryIndex(const std::string& cat) {
    if (cat == "models") return 0;
    if (cat == "textures") return 1;
    if (cat == "audio") return 2;
    return 3;
}

void WalkDir(const std::string& dir, const std::string& rel, std::vector<AssetLibEntry>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        const std::string r = rel.empty() ? name : rel + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDir(full, r, out);
        } else {
            const size_t dot = name.find_last_of('.');
            if (dot == std::string::npos) continue;
            const std::string ext = ToLower(name.substr(dot));
            if (!IsModel(ext) && !IsTexture(ext) && !IsAudio(ext)) continue;
            AssetLibEntry e;
            e.relPath = r;
            e.fullPath = full;
            e.category = CategoryOf(ext);
            LARGE_INTEGER sz;
            sz.HighPart = fd.nFileSizeHigh;
            sz.LowPart = fd.nFileSizeLow;
            e.size = static_cast<size_t>(sz.QuadPart);
            out.push_back(e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void EnsureDir(const std::string& dir) {
    std::string cmd = "mkdir \"" + dir + "\" 2>nul";
    std::system(cmd.c_str());
}

bool CopyFileTo(const std::string& src, const std::string& dst) {
    const size_t slash = dst.find_last_of("/\\");
    if (slash != std::string::npos) EnsureDir(dst.substr(0, slash));
    return CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0;
}

bool FileExists(const std::string& p) {
    struct _stat64 st;
    return _stat64(p.c_str(), &st) == 0;
}

} // namespace

void AssetLibraryPanel::Scan(const std::string& root) {
    entries_.clear();
    if (root.empty()) return;
    WalkDir(root, "", entries_);
    std::sort(entries_.begin(), entries_.end(),
              [](const AssetLibEntry& a, const AssetLibEntry& b) { return a.relPath < b.relPath; });
    scanned_ = true;
}

std::string AssetLibraryPanel::TargetPath(EditorContext& ctx, const AssetLibEntry& e) const {
    if (!ctx.projectDir) return "";
    return *ctx.projectDir + "/assets/" + e.category + "/" + e.relPath;
}

void AssetLibraryPanel::ImportSelected(EditorContext& ctx) {
    imported_ = 0;
    skipped_ = 0;
    for (auto& e : entries_) {
        if (!e.selected) continue;
        const std::string dst = TargetPath(ctx, e);
        if (dst.empty()) continue;
        if (!overwrite_ && FileExists(dst)) {
            ++skipped_;
            continue;
        }
        if (CopyFileTo(e.fullPath, dst)) {
            ++imported_;
            e.selected = false;
        } else {
            ++skipped_;
        }
    }
}

void AssetLibraryPanel::Draw(EditorContext& ctx) {
    if (!ImGui::Begin(Title(), VisibleFlag())) {
        ImGui::End();
        return;
    }

    // === 路径输入 ===
    ImGui::Text("素材目录：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("##root", rootPath_, sizeof(rootPath_));
    ImGui::SameLine();
    if (ImGui::Button("浏览...")) {
        BROWSEINFOA bi = {};
        char path[MAX_PATH] = {};
        bi.lpszTitle = "选择素材目录";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            SHGetPathFromIDListA(pidl, path);
            IMalloc* im = nullptr;
            SHGetMalloc(&im);
            if (im) { im->Free(pidl); im->Release(); }
            if (path[0]) {
                std::snprintf(rootPath_, sizeof(rootPath_), "%s", path);
                Scan(rootPath_);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("扫描") && rootPath_[0]) {
        Scan(rootPath_);
    }

    ImGui::Separator();

    if (scanned_ && !entries_.empty()) {
        // === 过滤 ===
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##filter", "过滤...", filter_, sizeof(filter_));
        ImGui::SameLine();
        const char* cats[] = {"全部", "模型", "贴图", "音频", "其他"};
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("##cat", &categoryFilter_, cats, 5);
        ImGui::SameLine();
        if (ImGui::Button("全选")) for (auto& e : entries_) e.selected = true;
        ImGui::SameLine();
        if (ImGui::Button("全不选")) for (auto& e : entries_) e.selected = false;
        ImGui::SameLine();
        ImGui::Checkbox("覆盖", &overwrite_);

        int selCount = 0;
        for (auto& e : entries_) if (e.selected) ++selCount;
        ImGui::TextDisabled("%zu 项（选中 %d）", entries_.size(), selCount);

        ImGui::BeginDisabled(selCount == 0);
        if (ImGui::Button("导入选中") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I)) {
            ImportSelected(ctx);
        }
        ImGui::EndDisabled();
        if (imported_ > 0 || skipped_ > 0) {
            ImGui::SameLine();
            ImGui::Text("导入 %d · 跳过 %d", imported_, skipped_);
        }
        ImGui::Separator();

        // === 列表 ===
        ImGui::BeginChild("asset_list", ImVec2(0, 0), 0, 0);
        const std::string filterStr = filter_;
        for (auto& e : entries_) {
            if (!filterStr.empty() && e.relPath.find(filterStr) == std::string::npos) continue;
            if (categoryFilter_ > 0 && CategoryIndex(e.category) != categoryFilter_ - 1) continue;

            ImGui::PushID(e.relPath.c_str());
            const int ci = CategoryIndex(e.category);
            const ImVec4 colors[] = {
                ImVec4(0.39f, 0.67f, 1.0f, 1), ImVec4(0.47f, 0.86f, 0.47f, 1),
                ImVec4(1.0f, 0.78f, 0.39f, 1), ImVec4(0.71f, 0.71f, 0.71f, 1)};
            ImGui::TextColored(colors[ci], "[%s]", kCategories[ci]);
            ImGui::SameLine();
            ImGui::Selectable(e.relPath.c_str(), &e.selected);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
            ImGui::TextDisabled("%.1fMB", e.size / 1048576.0);
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else if (scanned_) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.5f, 1), "目录为空或无可导入资产");
    } else {
        ImGui::TextDisabled("指定你的素材目录后点扫描");
        ImGui::Spacing();
        ImGui::TextDisabled("支持: .obj .glb .gltf .fbx .png .jpg .tga .dds .wav .mp3 .ogg");
        ImGui::TextDisabled("导入到: assets/models/ · assets/textures/ · assets/audio/");
    }

    ImGui::End();
}

} // namespace neon::editor
