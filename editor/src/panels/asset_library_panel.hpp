#pragma once

// 素材库面板 — 从任意本地目录批量导入资产到当前工程。
#include <string>
#include <vector>

#include "editor_context.hpp"

namespace neon::editor {

// 独立结构（WalkDir 递归需要公开访问）
struct AssetLibEntry {
    std::string relPath;
    std::string fullPath;
    std::string category;
    size_t size = 0;
    bool selected = false;
};

class AssetLibraryPanel : public IPanel {
public:
    explicit AssetLibraryPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "素材库"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    void Scan(const std::string& root);
    void ImportSelected(EditorContext& ctx);
    std::string TargetPath(EditorContext& ctx, const AssetLibEntry& e) const;

    bool* visible_;
    char rootPath_[1024]{};
    std::vector<AssetLibEntry> entries_;
    bool scanned_ = false;
    char filter_[128]{};
    int categoryFilter_ = -1;
    bool overwrite_ = false;
    int imported_ = 0;
    int skipped_ = 0;
};

} // namespace neon::editor
