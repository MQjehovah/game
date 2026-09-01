#pragma once

// 面板注册表：EditorApp 持有，负责注册/注销/统一渲染/开关状态。
// 新增面板 = 写一个 IPanel 子类 + Register，无需改 EditorApp 核心。
//
// ImGui 边界决策：注册表保持 ImGui-free（可独立单测，无 ImGui context）。
// DrawAll 只对可见面板调 Draw(ctx)；ImGui::Begin/End 由各面板自己的 Draw
// 负责（面板自治）。窗口菜单的 ImGui::MenuItem 勾选项同理留在 EditorApp
// 侧（遍历 Panels() + VisibleFlag()），后续阶段接入。

#include <memory>
#include <string>
#include <vector>

#include "editor_context.hpp"

namespace neon::editor {

class PanelRegistry {
public:
    void Register(std::unique_ptr<IPanel> panel);                 // 加载
    std::unique_ptr<IPanel> Unregister(const std::string& title); // 卸载并返回（可重新注册）
    IPanel* Find(const std::string& title);
    void OpenAll(EditorContext& ctx);   // 全部 OnOpen
    void Shutdown();                    // 全部 OnClose 并清空
    void UpdateAll(float dt);           // 全部 OnUpdate（不按可见性过滤）
    void DrawAll(EditorContext& ctx);   // 只对可见面板调 Draw（ImGui-free 分发）
    std::vector<std::unique_ptr<IPanel>>& Panels() { return panels_; }
    size_t Count() const { return panels_.size(); }

private:
    std::vector<std::unique_ptr<IPanel>> panels_;
};

} // namespace neon::editor
