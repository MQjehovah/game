// 素材库面板实现 — 按源目录层级浏览 → 图标网格 → 批量导入工程。
//
// 关键点：
//  * 逐级加载（每次只 stat 当前目录的直接子项），不再递归扫描整棵树，
//    因此打开素材根目录不会再卡。
//  * 纯 ImGui 图标网格（不用原生对话框 / 列表）：原生模态对话框的消息泵会
//    和引擎的 PeekMessage 渲染循环抢消息，导致编辑器假死。
#include "panels/asset_library_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "editor.hpp"
#include "imgui.h"

#include "neon/gfx/imgui_neon.hpp"

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
std::string ExtOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : ToLower(name.substr(dot));
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
// full 相对 root 的路径（用于保持源目录层级）；不在 root 下时原样返回。
std::string RelToRoot(const std::string& full, const std::string& root) {
    if (root.empty()) return full;
    if (full.size() > root.size() + 1 && full.compare(0, root.size(), root) == 0 &&
        (full[root.size()] == '/' || full[root.size()] == '\\'))
        return full.substr(root.size() + 1);
    return full;
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

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

// 上一级目录；盘根（"C:/"）保持不变。
std::string ParentDir(const std::string& p) {
    if (p.size() <= 3) return p;
    const size_t slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return p;
    if (slash <= 2) return p.substr(0, 3);
    return p.substr(0, slash);
}

std::vector<std::string> ListDrives() {
    std::vector<std::string> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) out.push_back(std::string(1, static_cast<char>('A' + i)) + ":");
    return out;
}
#endif

} // namespace

void AssetLibraryPanel::LoadRoot(const std::string& root) {
    std::string r = root;
    while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    if (r.empty()) return;
    std::snprintf(rootPath_, sizeof(rootPath_), "%s", r.c_str());
    std::snprintf(curDir_, sizeof(curDir_), "%s", r.c_str());
    selected_.clear();
    texCache_.clear();
    entries_.clear();
    LoadDir(curDir_);
}

// 仅列举当前目录的直接子项（目录 + 支持的资产文件），不递归。
void AssetLibraryPanel::LoadDir(const std::string& dir) {
    entries_.clear();
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            AssetLibEntry e;
            e.relPath = name;
            e.fullPath = full;
            e.category = "dir";
            e.isDir = true;
            entries_.push_back(std::move(e));
        } else {
            const std::string ext = ExtOf(name);
            if (!IsModel(ext) && !IsTexture(ext) && !IsAudio(ext)) continue;
            AssetLibEntry e;
            e.relPath = name;
            e.fullPath = full;
            e.category = CategoryOf(ext);
            LARGE_INTEGER sz;
            sz.HighPart = fd.nFileSizeHigh;
            sz.LowPart = fd.nFileSizeLow;
            e.size = static_cast<size_t>(sz.QuadPart);
            entries_.push_back(std::move(e));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(entries_.begin(), entries_.end(),
              [](const AssetLibEntry& a, const AssetLibEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir; // 目录在前
                  return ToLower(a.relPath) < ToLower(b.relPath);
              });
}

std::string AssetLibraryPanel::TargetPath(EditorContext& ctx, const std::string& category,
                                          const std::string& rel) const {
    if (!ctx.projectDir) return "";
    return *ctx.projectDir + "/assets/" + category + "/" + rel;
}

void AssetLibraryPanel::ImportSelected(EditorContext& ctx) {
    imported_ = 0;
    skipped_ = 0;
    std::vector<std::string> done;
    for (const std::string& full : selected_) {
        const std::string ext = ExtOf(full);
        const std::string dst = TargetPath(ctx, CategoryOf(ext), RelToRoot(full, rootPath_));
        if (dst.empty()) continue;
        if (!overwrite_ && FileExists(dst)) {
            ++skipped_;
            continue;
        }
        if (CopyFileTo(full, dst)) {
            ++imported_;
            done.push_back(full);
        } else {
            ++skipped_;
        }
    }
    for (const std::string& p : done) selected_.erase(p);
}

std::uint64_t AssetLibraryPanel::TileTexture(EditorContext& ctx, const std::string& fullPath,
                                             bool isTexture) {
    if (!isTexture || !ctx.assetMgr) return 0;
    auto it = texCache_.find(fullPath);
    if (it != texCache_.end() && it->second != 0) return it->second;
    gfx::Texture tex = ctx.assetMgr->LoadTexture(fullPath);
    if (!tex.Valid()) return 0;
    const std::uint64_t id = static_cast<std::uint64_t>(gfx::ImGuiNeon_RegisterTexture(tex.Handle()));
    texCache_[fullPath] = id;
    return id;
}

void AssetLibraryPanel::Draw(EditorContext& ctx) {
    if (!ImGui::Begin(Title(), VisibleFlag())) {
        ImGui::End();
        return;
    }

    // === 根目录 + 盘符 ===
    ImGui::Text("素材目录：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-160);
    ImGui::InputText("##root", rootPath_, sizeof(rootPath_));
    ImGui::SameLine();
    if (ImGui::Button("载入") && rootPath_[0]) LoadRoot(rootPath_);
    ImGui::SameLine();
#if defined(_WIN32)
    const std::vector<std::string> drives = ListDrives();
    if (!drives.empty()) {
        std::vector<const char*> items;
        items.reserve(drives.size());
        for (auto& d : drives) items.push_back(d.c_str());
        ImGui::SetNextItemWidth(70);
        if (ImGui::Combo("##rootdrive", &rootDriveIdx_, items.data(), static_cast<int>(items.size()))) {
            LoadRoot(drives[rootDriveIdx_] + "/");
        }
    }
#endif

    if (curDir_[0] == '\0') {
        ImGui::Separator();
        ImGui::TextDisabled("输入素材根目录（或选盘符）后点“载入”，再逐级进入子目录；");
        ImGui::TextDisabled("勾选文件 → “导入选中”，按源目录层级拷入 assets/ 下。");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // === 路径行 + 上级 ===
    const bool atRoot = std::strcmp(curDir_, rootPath_) == 0;
    ImGui::BeginDisabled(atRoot);
    if (ImGui::Button("⬆ 上级")) {
        const std::string parent = ParentDir(curDir_);
        std::snprintf(curDir_, sizeof(curDir_), "%s", parent.c_str());
        LoadDir(curDir_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", curDir_);

    // === 过滤 + 批量 ===
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##filter", "过滤...", filter_, sizeof(filter_));
    ImGui::SameLine();
    const char* cats[] = {"全部", "模型", "贴图", "音频", "其他"};
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##cat", &categoryFilter_, cats, 5);
    ImGui::SameLine();
    if (ImGui::Button("全选本目录")) {
        for (auto& e : entries_)
            if (!e.isDir) selected_.insert(e.fullPath);
    }
    ImGui::SameLine();
    if (ImGui::Button("全不选")) selected_.clear();
    ImGui::SameLine();
    ImGui::Checkbox("覆盖", &overwrite_);

    const std::string filterStr = filter_;
    ImGui::TextDisabled("选中 %zu 项", selected_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_.empty());
    if (ImGui::Button("导入选中") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I)) {
        ImportSelected(ctx);
    }
    ImGui::EndDisabled();
    if (imported_ > 0 || skipped_ > 0) {
        ImGui::SameLine();
        ImGui::Text("导入 %d · 跳过 %d", imported_, skipped_);
    }
    ImGui::Separator();

    // === 图标网格 ===
    const float cellW = 88.0f;
    const float cellH = 100.0f;
    ImGui::BeginChild("##lib_grid", ImVec2(0, 0), 0, 0);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(1, static_cast<int>(avail.x / cellW));
    int slot = 0; // slot 0 = 上级

    // “上级”格子
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushID("up");
    ImGui::InvisibleButton("##up", ImVec2(cellW - 6.0f, cellH - 8.0f));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetItemRectMin();
        const ImVec2 br = ImGui::GetItemRectMax();
        dl->AddRectFilled(tl, br, IM_COL32(70, 70, 80, 255));
        dl->AddRect(tl, br, IM_COL32(30, 30, 35, 255));
        const ImVec2 ts = ImGui::CalcTextSize("⬆");
        dl->AddText(ImVec2((tl.x + br.x - ts.x) * 0.5f, tl.y + 18.0f),
                    IM_COL32(230, 230, 235, 255), "⬆");
        const ImVec2 ts2 = ImGui::CalcTextSize("上级");
        dl->AddText(ImVec2((tl.x + br.x - ts2.x) * 0.5f, br.y - 20.0f),
                    IM_COL32(200, 205, 215, 255), "上级");
    }
    if (!atRoot && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const std::string parent = ParentDir(curDir_);
        std::snprintf(curDir_, sizeof(curDir_), "%s", parent.c_str());
        LoadDir(curDir_);
    }
    ImGui::PopID();
    ++slot;

    for (size_t i = 0; i < entries_.size(); ++i) {
        AssetLibEntry& e = entries_[i];
        const std::string ext = e.isDir ? std::string() : ExtOf(e.relPath);
        const bool isTex = IsTexture(ext);
        const bool isMdl = IsModel(ext);
        if (!e.isDir) {
            if (!filterStr.empty() && e.relPath.find(filterStr) == std::string::npos) continue;
            if (categoryFilter_ > 0 && CategoryIndex(e.category) != categoryFilter_ - 1) continue;
        }

        const int col = slot % cols;
        const int row = slot / cols;
        ++slot;
        ImGui::SetCursorPos(ImVec2(col * cellW, row * cellH));
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 cellSize(cellW - 6.0f, cellH - 8.0f);
        const bool clicked = ImGui::InvisibleButton("##cell", cellSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        const bool isSelected = !e.isDir && selected_.count(e.fullPath) > 0;
        if (e.isDir) {
            if (dbl) {
                const std::string next = std::string(curDir_) + "/" + e.relPath;
                std::snprintf(curDir_, sizeof(curDir_), "%s", next.c_str());
                LoadDir(curDir_);
            }
        } else if (clicked) {
            if (isSelected) selected_.erase(e.fullPath);
            else selected_.insert(e.fullPath);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 tl = ImGui::GetItemRectMin();
        const float tw = cellSize.x;
        const ImVec2 thumbTl(tl.x + 4.0f, tl.y + 2.0f);
        const ImVec2 thumbBr(tl.x + tw - 4.0f, tl.y + 62.0f);

        if (isSelected) dl->AddRectFilled(tl, ImVec2(tl.x + tw, tl.y + cellH - 8.0f),
                                          IM_COL32(60, 110, 170, 110));

        ImU32 tileCol = IM_COL32(70, 70, 80, 255);
        ImTextureID tid = ImTextureID_Invalid;
        bool flipV = false;
        const char* tag = "FILE";
        if (e.isDir) {
            tileCol = IM_COL32(185, 145, 45, 255);
            tag = "DIR";
        } else if (isTex) {
            tileCol = IM_COL32(70, 150, 90, 255);
            tag = "IMG";
            const std::uint64_t handle = TileTexture(ctx, e.fullPath, true);
            if (handle) tid = static_cast<ImTextureID>(handle);
        } else if (isMdl) {
            tileCol = IM_COL32(85, 125, 200, 255);
            tag = "MDL";
            tid = static_cast<ImTextureID>(ctx.meshThumbnail ? ctx.meshThumbnail(e.fullPath) : 0);
            flipV = tid != ImTextureID_Invalid;
        } else {
            tileCol = IM_COL32(200, 130, 55, 255);
            tag = "WAV";
        }

        if (tid != ImTextureID_Invalid) {
            const float tw2 = thumbBr.x - thumbTl.x;
            const float th2 = thumbBr.y - thumbTl.y;
            const float ts = std::min(tw2, th2);
            const ImVec2 imgTl(thumbTl.x + (tw2 - ts) * 0.5f, thumbTl.y + (th2 - ts) * 0.5f);
            dl->AddImage(tid, imgTl, ImVec2(imgTl.x + ts, imgTl.y + ts),
                         ImVec2(0.0f, flipV ? 1.0f : 0.0f), ImVec2(1.0f, flipV ? 0.0f : 1.0f));
            dl->AddRect(imgTl, ImVec2(imgTl.x + ts, imgTl.y + ts), IM_COL32(30, 30, 35, 255));
        } else {
            dl->AddRectFilled(thumbTl, thumbBr, tileCol);
            dl->AddRect(thumbTl, thumbBr, IM_COL32(30, 30, 35, 255));
            const ImVec2 ts = ImGui::CalcTextSize(tag);
            dl->AddText(ImVec2((thumbTl.x + thumbBr.x - ts.x) * 0.5f,
                               (thumbTl.y + thumbBr.y - ts.y) * 0.5f),
                        IM_COL32(255, 255, 255, 225), tag);
        }

        // 文件名（单行，裁剪）
        dl->PushClipRect(tl, ImVec2(tl.x + tw, tl.y + cellH - 6.0f), true);
        dl->AddText(ImVec2(tl.x + 3.0f, thumbBr.y + 3.0f), IM_COL32(220, 225, 235, 255),
                    e.relPath.c_str());
        dl->PopClipRect();

        // 选中勾
        if (isSelected) {
            const ImVec2 c(thumbBr.x - 11.0f, thumbTl.y + 11.0f);
            dl->AddCircleFilled(c, 9.0f, IM_COL32(60, 150, 220, 235));
            dl->AddCircle(c, 9.0f, IM_COL32(240, 245, 255, 255));
            dl->AddText(ImVec2(c.x - 4.0f, c.y - 7.0f), IM_COL32(255, 255, 255, 255), "✓");
        }
        ImGui::PopID();
    }

    const int rows = (slot + cols - 1) / cols;
    ImGui::Dummy(ImVec2(1.0f, rows * cellH + 8.0f));
    ImGui::EndChild();

    ImGui::End();
}

} // namespace neon::editor
