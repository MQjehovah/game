#pragma once
#include <memory>
#include <string>

#include "neon/ui/ui_system.hpp"

namespace neon::scene {

// 游戏 UI 系统（数据驱动 ui/*.ui.json 文档）的薄包装，持有一个可替换的
// ui::IUiSystem（cfg_.uiSystem 注入，否则默认 CreateDocumentUiSystem）。纯机械
// 拆分自 GameRuntime 的 ui_ + ShowUI/HideUI/UIClicked/UISet*/DrawUI（Task 11）：
// 所有方法都是对 IUiSystem 的转发（空系统时安全 no-op），渲染与输入更新照旧
// 委托给底层系统。2D 设计空间映射（uiScale_/uiOffset_ → screenToUi）留在
// GameRuntime：它是 Draw 里从 live renderer 快照的脚本画布映射，InputMousePos
// binding 与 UI 命中测试共用，收进此类会改变首帧取值时序。
class UiSystem {
public:
    void Set(std::shared_ptr<ui::IUiSystem> system) { ui_ = std::move(system); }
    bool Show(const std::string& path) { return ui_ && ui_->Show(path); }
    void Hide() { if (ui_) ui_->Hide(); }
    bool Active() const { return ui_ && ui_->Active(); }
    void Update(const math::Vec2& pointer, bool clickEdge) { if (ui_) ui_->Update(pointer, clickEdge); }
    bool Clicked(const std::string& name) const { return ui_ && ui_->Clicked(name); }
    void SetText(const std::string& name, const std::string& text) { if (ui_) ui_->SetText(name, text); }
    void SetFill(const std::string& name, float fill) { if (ui_) ui_->SetFill(name, fill); }
    void SetVisible(const std::string& name, bool visible) { if (ui_) ui_->SetVisible(name, visible); }
    void SetColor(const std::string& name, float r, float g, float b, float a) { if (ui_) ui_->SetColor(name, r, g, b, a); }
    void ConsumeClicks() { if (ui_) ui_->ConsumeClicks(); }
    void Draw(gfx::Renderer& renderer, const gfx::Font& font, const ui::UiTextureLoader& loadTexture,
              const math::Vec2& viewportSize);
    ui::IUiSystem* Raw() { return ui_.get(); }

private:
    std::shared_ptr<ui::IUiSystem> ui_;
};

} // namespace neon::scene
