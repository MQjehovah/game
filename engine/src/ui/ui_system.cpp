#include "neon/ui/ui_system.hpp"

#include <set>

#include "neon/core/log.hpp"

namespace neon::ui {

// ---------------------------------------------------------------------------
// DocumentUiSystem: the default IUiSystem. Wraps one active UiDocument and
// implements the show/hide, hit-test/click and mutation surface on top of it.
// ---------------------------------------------------------------------------

class DocumentUiSystem final : public IUiSystem {
public:
    explicit DocumentUiSystem(DocumentUiConfig cfg) : cfg_(std::move(cfg)) {}

    bool Show(const std::string& path) override {
        const std::string text = cfg_.readFile ? cfg_.readFile(path) : std::string();
        auto doc = std::make_unique<UiDocument>();
        if (text.empty() || !doc->LoadJson(text)) return false;
        doc_ = std::move(doc);
        clicked_.clear();
        return true;
    }

    void Hide() override {
        doc_.reset();
        clicked_.clear();
    }

    bool Active() const override { return doc_ != nullptr; }

    void Update(const math::Vec2& pointer, bool clickEdge) override {
        // LATCH, don't clear: a click recorded by this render frame must
        // survive until the simulation tick that reads Clicked() (0-tick
        // rendered frames must not swallow it). ConsumeClicks() clears.
        if (!doc_ || !clickEdge) return;
        if (UiNode* hit = doc_->HitTest(pointer, lastViewport_)) {
            // Bubble up to the clickable ancestor: clicking a card's icon or
            // label hits THAT node, not the button - the whole subtree of a
            // button is its click target.
            for (UiNode* n = hit; n; n = n->parent) {
                if (n->type == UiNodeType::Button) {
                    clicked_.insert(n->name);
                    break;
                }
            }
        }
    }

    bool Clicked(const std::string& name) const override {
        return clicked_.count(name) != 0;
    }

    void ConsumeClicks() override { clicked_.clear(); }

    void SetText(const std::string& node, const std::string& text) override {
        if (doc_)
            if (UiNode* n = doc_->Find(node)) n->text = text;
    }

    void SetFill(const std::string& node, float fill) override {
        if (doc_)
            if (UiNode* n = doc_->Find(node)) n->fill = fill;
    }

    void SetVisible(const std::string& node, bool visible) override {
        if (doc_)
            if (UiNode* n = doc_->Find(node)) n->visible = visible;
    }

    void SetColor(const std::string& node, float r, float g, float b, float a) override {
        if (doc_)
            if (UiNode* n = doc_->Find(node)) n->color = {r, g, b, a};
    }

    bool NodeRect(const std::string& node, math::Rect2& out) const override {
        if (!doc_) return false;
        const UiNode* n = doc_->Find(node);
        if (!n || !n->resolvedValid) return false;
        out = n->resolved;
        return true;
    }

    void Draw(gfx::Renderer& renderer, const gfx::Font& font,
              const UiTextureLoader& loadTexture,
              const math::Vec2& viewportSize) override {
        lastViewport_ = viewportSize;
        if (!doc_) return;
        doc_->Draw(renderer, font, loadTexture, viewportSize);
    }

private:
    DocumentUiConfig cfg_;
    std::unique_ptr<UiDocument> doc_;
    std::set<std::string> clicked_;
    math::Vec2 lastViewport_{1280.0f, 720.0f};
};

std::unique_ptr<IUiSystem> CreateDocumentUiSystem(const DocumentUiConfig& cfg) {
    return std::make_unique<DocumentUiSystem>(cfg);
}

} // namespace neon::ui
