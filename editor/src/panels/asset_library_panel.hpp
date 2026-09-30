#pragma once

// 素材库面板 — 按源目录层级逐级浏览，图标网格显示，批量导入到当前工程。
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor_context.hpp"

namespace neon::editor {

// 独立结构（目录列举需要公开访问）
struct AssetLibEntry {
    std::string relPath;   // 相对当前目录的文件/子目录名
    std::string fullPath;  // 绝对/完整路径
    std::string category;  // models/textures/audio/misc/dir
    size_t size = 0;
    bool isDir = false;
};

class AssetLibraryPanel : public IPanel {
public:
    explicit AssetLibraryPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "素材库"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    // 设定根目录并载入（清空选择与缩略图缓存）。
    void LoadRoot(const std::string& root);
    // 只列举当前目录的直接子项（不递归）——大目录不再拖慢界面。
    void LoadDir(const std::string& dir);
    void ImportSelected(EditorContext& ctx);
    std::string TargetPath(EditorContext& ctx, const std::string& category,
                           const std::string& rel) const;
    std::uint64_t TileTexture(EditorContext& ctx, const std::string& fullPath, int& budget);

    bool* visible_;
    char rootPath_[1024]{}; // 素材根目录（导入基准）
    char curDir_[1024]{};   // 当前浏览目录
    std::vector<AssetLibEntry> entries_;
    std::set<std::string> selected_; // 选中文件（完整路径，跨目录保留）
    std::unordered_map<std::string, std::uint64_t> texCache_; // 贴图预览 ImTextureID
    char filter_[128]{};
    int categoryFilter_ = -1;
    int rootDriveIdx_ = 0;
    int loadBudget_ = 0;   // 每帧最多新解码的缩略图数（防止一进目录全部卡住）
    bool previews_ = true; // 是否生成缩略图（关闭后仅显示类型图标，零解码）
    bool overwrite_ = false;
    int imported_ = 0;
    int skipped_ = 0;
};

} // namespace neon::editor
