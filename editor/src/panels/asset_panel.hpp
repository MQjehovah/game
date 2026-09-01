#pragma once

// 资产面板 —— 面板插件化（阶段 1）的第二个迁移（沿用 Task 2 ScenePanel 样板）：
// 原 EditorApp::BuildAssetPanel（panels_asset_panel.inc）整体迁入本类。
//
// 状态边界：
// - 面板私有状态（导入对话框延迟打开标志、新建资产行的类型/名称缓冲）是本类
//   成员（原函数内 static）；
// - 资产浏览共享状态（当前目录/条目表/过滤页签/网格视图/选中/Delete 键待处理
//   标志）仍由 EditorApp 拥有——核心方法（RefreshAssetDir/Import*/Create/
//   DeleteSelectedAsset）、资产目录监视（PollHotReload）与冒烟测试都直接读写
//   它们，经 EditorContext 注入指针；缩略图 GPU 缓存由视口每帧的
//   Generate*Thumbnails 填充，面板经 ctx 查询回调读取（登记请求 + 取纹理 id）；
// - 跨面板操作（导入/新建/删除资产、导入到场景、脚本编辑器、外部编辑器）走
//   ctx 注入回调；
// - 可见标志在过渡期指向 EditorApp::showAssets_（窗口菜单勾选 + ini 持久化 +
//   冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class AssetPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showAssets_）。
    explicit AssetPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "资产"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showAssets_
    // 导入对话框：一个入口同时支持文件与目录, 二级菜单选择后, 下一帧开原生
    // 对话框 (不能在 popup 模态内直接开)。原函数内 static。
    bool pendingFile_ = false;
    bool pendingDir_ = false;
    // "新建" 行：类型组合框 + 名称输入（原函数内 static）。
    int newKind_ = -1;          // -1 = 行收起, 0..5 = 待创建的资产类型
    char newName_[128] = {};
};

} // namespace neon::editor
