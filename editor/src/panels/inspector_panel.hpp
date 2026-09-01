#pragma once

// 属性面板 —— 面板插件化（阶段 1）的第三个迁移（沿用 Task 2/3 样板）：
// 原 EditorApp::BuildInspectorPanel（panels_inspector.inc）整体迁入本类。
//
// 状态边界：
// - 面板私有状态（材质球名/着色器路径/贴花贴图输入缓冲、添加组件组合框选择）
//   是本类成员（原函数内 static）；
// - 共享状态（实体表/选中/多选集合/撤销栈/场景脏标志/预置体库/项目目录/资产
//   管理器/插件管理器/场景 World）经 EditorContext 指针访问；
// - 跨面板操作与被多处共用的 EditorApp 方法（ResolveMesh/ApplyMaterialParams/
//   SaveMaterialAsset/ApplyMaterialAsset/MaterializePrefabEntity/ReloadEntityShader，
//   视口/播放/加载/冒烟同样调用）走 ctx 注入回调；EditMeshKeyCommand 也改为
//   持 resolve/apply 回调（不再依赖 EditorApp*，无逃生舱）；
// - 可见标志在过渡期指向 EditorApp::showInspector_（窗口菜单勾选 + ini 持久化
//   + 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include "editor_context.hpp"

namespace neon::editor {

class InspectorPanel : public IPanel {
public:
    // visibleFlag: 过渡期由 EditorApp 注入（指向 showInspector_）。
    explicit InspectorPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "属性"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

private:
    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showInspector_
    char matNameBuf_[128] = {}; // "材质球名" 输入缓冲（原函数内 static）
    char shaderBuf_[512] = {};  // "着色器 (.glsl 片元)" 输入缓冲（原函数内 static）
    char decalBuf_[512] = {};   // 贴花 "贴图" 输入缓冲（原函数内 static）
    int addCompSel_ = 0;        // "添加组件" 组合框（原函数内 static）
};

} // namespace neon::editor
