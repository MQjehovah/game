#pragma once

// 场景面板（层次树）——面板插件化（阶段 1）的第一个真实迁移样板：
// 原 EditorApp::BuildScenePanel（panels_scene.inc）整体迁入本类。
//
// 状态边界：
// - 面板私有状态（添加类型、过滤缓冲、拖拽载荷、预置体保存提示）是本类成员；
// - 共享状态（实体表/选中/多选集合/撤销栈/后处理开关/项目目录）经 EditorContext
//   访问；跨面板操作（添加实体、选择变更、保存预置体等）走 ctx 注入回调；
// - 可见标志在过渡期指向 EditorApp::showHierarchy_（窗口菜单勾选 + ini 持久化
//   仍走 PanelDef 成员指针表，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <vector>

#include "editor_context.hpp"

namespace neon::editor {

class ScenePanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showHierarchy_）。
    explicit ScenePanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "场景"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showHierarchy_
    int addType_ = 0;           // "添加" 工具栏的类型组合框（原函数内 static）
    char filterBuf_[128] = {};  // P2-editor UX: 实体名过滤（原函数内 static）
    std::vector<int> dragPayload_;  // P2-editor UX: 多选拖拽载荷缓冲
    // "保存为预置体" 名称提示（场景右键菜单）：确认模板名后调 savePrefab。
    bool prefabSavePrompt_ = false;
    char prefabSaveBuf_[128] = {};
    int prefabSaveTarget_ = -1; // 待保存实体索引（-1 = 无）
};

} // namespace neon::editor
