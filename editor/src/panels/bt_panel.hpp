#pragma once

// 行为树编辑器 —— 面板插件化（Task 18b，最后一个特殊面板）：
// 原 EditorApp::BuildBtPanel/BuildBtToolbar/BuildBtPalette/BuildBtCanvas/
// BuildBtParams（bt_editor.cpp）迁入本类。
//
// 特殊面板：画布图 btGraph_ 仍由 EditorApp 持有（OnCreate:1080 播种默认图 +
// 冒烟测试直接读写），经 EditorContext::btGraph 指针共享；BT 文件 IO
// （EditorApp::BtLoadFromFile/BtSaveToFile，冒烟测试也调）经 ctx 回调；播放
// 高亮（play_ 是 GameRuntime，面板不直接持有）经 ctx.playActiveTreePath 回调。
// 其余状态（btHistory_/btFileName_/选中/拖拽/视图变换/参数拖拽）全部迁入本类。
//
// 可见标志过渡期指向 EditorApp::showBt_（窗口菜单勾选 + ini 持久化 + 冒烟
// 强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。

#include <map>
#include <string>
#include <vector>

#include "bt_editor.hpp"
#include "editor_context.hpp"
#include "imgui.h"
#include "neon/bt/behavior_tree.hpp"
#include "neon/math/vec2.hpp"

namespace neon::editor {

class BtPanel : public IPanel {
public:
    explicit BtPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "行为树"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

    // --- 外部入口（EditorApp 冒烟测试 / Ctrl+Z 路由经此访问） ---------------
    // 冒烟测试原直接读写 EditorApp::btSelected_/btHistory_/btCanvasDrawn_/
    // BtCanvasClick/BtPushSnapshot；状态迁入面板后改经这些方法，行为一致。
    void SetSelected(const std::string& id) { btSelected_ = id; }
    bool CanvasDrawn() const { return btCanvasDrawn_; }      // 冒烟：画布本帧发出几何
    bool PanelFocused() const { return btPanelFocused_; }    // undo/redo 路由
    bool CanUndoBt() const { return btHistory_.CanUndo(); }
    bool CanRedoBt() const { return btHistory_.CanRedo(); }
    void UndoBt() { btHistory_.Undo(); }
    void RedoBt() { btHistory_.Redo(); }
    // Canvas 鼠标处理（提取自原 BtCanvasClick，冒烟测试驱动真实连线路径）。
    void CanvasClick(EditorContext& ctx, const math::Vec2& cm, bool ctrl, bool shift);
    // 整图快照入撤销栈（原 BtPushSnapshot）。
    void PushSnapshot(EditorContext& ctx, const btgraph::BtGraph& before);

private:
    void BuildToolbar(EditorContext& ctx);
    void BuildPalette(EditorContext& ctx);
    void BuildCanvas(EditorContext& ctx);
    void BuildParams(EditorContext& ctx);
    void NewTree(EditorContext& ctx);
    void UpdatePlayHighlight(EditorContext& ctx);
    void RefreshBehaviorFiles(EditorContext& ctx);
    std::string BehaviorsDir(const EditorContext& ctx) const;
    bool LoadFromFile(EditorContext& ctx, const std::string& path);
    void ParamNumber(EditorContext& ctx, const btgraph::BtGraphNode& n,
                     const bt::ParamInfo& p);
    void ParamString(EditorContext& ctx, const btgraph::BtGraphNode& n,
                     const bt::ParamInfo& p);
    void ParamBool(EditorContext& ctx, const btgraph::BtGraphNode& n,
                   const bt::ParamInfo& p);
    void ParamJson(EditorContext& ctx, const btgraph::BtGraphNode& n,
                   const bt::ParamInfo& p);

    bool* visible_ = nullptr; // 不拥有；过渡期 = &EditorApp::showBt_

    // 原 EditorApp::btHistory_ 及画布/工具栏状态（editor.hpp:918-949）逐字段迁入。
    HistoryManager btHistory_;
    std::string btFileName_ = "behavior";
    char btFileNameBuf_[256]{};
    std::string btSelected_;   // selected canvas node id
    std::string btPendingType_; // armed palette node type (click canvas to place)
    std::string btActivePath_;  // play highlight: tree-path id of the running node
    std::vector<std::string> btBehaviorFiles_;
    uint64_t btFilesRefreshFrame_ = 0; // throttle: refresh behaviors/ listing periodically
    bool btCanvasDrawn_ = false; // smoke: the BT canvas emitted geometry this frame
    bool btPanelFocused_ = false; // undo/redo routing: BT panel owns Ctrl+Z while focused
    // Canvas drag state.
    std::string btDragNode_;
    math::Vec2 btDragStart_{0.f, 0.f};
    math::Vec2 btNodeStartPos_{0.f, 0.f};
    bool btDragging_ = false;
    // Canvas view transform: pan offset (screen px) + zoom factor (no
    // scrollbars — the canvas only pans/zooms).
    float btZoom_ = 1.0f;
    math::Vec2 btPan_{30.f, 30.f};
    // Anchor-link drag: drawing a connection from a node's output anchor.
    bool btLinking_ = false;
    std::string btLinkFrom_;
    // Graph snapshot captured when a node drag began, pushed as one undo step
    // on release (only when the node actually moved).
    btgraph::BtGraph btGraphBeforeDrag_;
    bool btHasGraphBeforeDrag_ = false;
    // Per-param drag origin: args snapshot captured when a slider drag began,
    // so the undo step reverts to the pre-drag value (one drag = one undo step).
    std::map<std::string, btgraph::BtGraph> btArgDragOrigin_;
};

} // namespace neon::editor
