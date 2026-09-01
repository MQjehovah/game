#include "panel_registry.hpp"

#include <utility>

namespace neon::editor {

void PanelRegistry::Register(std::unique_ptr<IPanel> panel) {
    if (!panel) return;
    panels_.push_back(std::move(panel));
}

std::unique_ptr<IPanel> PanelRegistry::Unregister(const std::string& title) {
    for (auto it = panels_.begin(); it != panels_.end(); ++it) {
        if (*it && title == (*it)->Title()) {
            std::unique_ptr<IPanel> out = std::move(*it);
            panels_.erase(it);
            return out;
        }
    }
    return nullptr;
}

IPanel* PanelRegistry::Find(const std::string& title) {
    for (auto& p : panels_) {
        if (p && title == p->Title()) return p.get();
    }
    return nullptr;
}

void PanelRegistry::OpenAll(EditorContext& ctx) {
    for (auto& p : panels_) {
        if (p) p->OnOpen(ctx);
    }
}

void PanelRegistry::Shutdown() {
    for (auto& p : panels_) {
        if (p) p->OnClose();
    }
    panels_.clear();
}

void PanelRegistry::UpdateAll(float dt) {
    for (auto& p : panels_) {
        if (p) p->OnUpdate(dt);
    }
}

void PanelRegistry::DrawAll(EditorContext& ctx) {
    // ImGui-free dispatch：只对 VisibleFlag 打开的面板调 Draw；窗口包裹
    // （Begin/End）是面板自己的责任。VisibleFlag() 返回 null 视为不可见。
    for (auto& p : panels_) {
        if (!p) continue;
        if (bool* v = p->VisibleFlag(); v && *v) p->Draw(ctx);
    }
}

} // namespace neon::editor
