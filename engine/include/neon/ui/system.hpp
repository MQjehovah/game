#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neon/gfx/font.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/platform/input.hpp"
#include "neon/ui/ui.hpp"

namespace neon::ui {

class UIManager;

class Element {
public:
    virtual ~Element() = default;

    std::string name;
    math::Rect2 rect;
    bool visible = true;
    bool clipChildren = false;
    Element* parent = nullptr;
    std::vector<std::unique_ptr<Element>> children;

    virtual math::Vec2 Measure(const Theme& theme) { (void)theme; return {0, 0}; }
    virtual void Layout(const Theme& theme, const math::Rect2& area);
    virtual void Draw(gfx::Renderer& renderer, const Theme& theme) {}
    // Draws this element and, by default, its children. Containers that need
    // control over child drawing (tabs, scroll areas) override this.
    virtual void DrawTree(gfx::Renderer& renderer, const Theme& theme);

    // Event hooks. MouseDown returning true claims the press (and focus).
    virtual bool MouseDown(const math::Vec2&, platform::MouseButton) { return false; }
    virtual bool MouseUp(const math::Vec2&, platform::MouseButton) { return false; }
    virtual bool MouseMove(const math::Vec2&) { return false; }
    virtual bool Scroll(float) { return false; }
    virtual void Key(platform::Key, bool) {}
    virtual void TextInput(const std::string&) {}
    virtual void OnFocusChanged(bool) {}

    void Add(std::unique_ptr<Element> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

    math::Vec2 AbsolutePos() const {
        math::Vec2 pos{rect.x, rect.y};
        for (const Element* e = parent; e; e = e->parent) pos += {e->rect.x, e->rect.y};
        return pos;
    }

    bool Contains(const math::Vec2& p) const {
        return p.x >= rect.x && p.x <= rect.x + rect.w && p.y >= rect.y && p.y <= rect.y + rect.h;
    }
};

class UIManager {
public:
    void Init(gfx::Renderer* renderer, const gfx::Font& font);

    Element* Root() { return &root_; }
    Theme& ThemeRef() { return theme_; }

    void Update(platform::IInput& input);
    void Draw(gfx::Renderer& renderer);
    void TextInput(const std::string& text);
    void Key(platform::Key key, bool down);

    // Programmatic access: find a named element in the tree, or hit-test a
    // point in design coordinates (top-left origin).
    Element* Find(const std::string& name);
    Element* HitTestAt(const math::Vec2& p);

    void SetFocus(Element* element);
    Element* Focus() const { return focus_; }
    Element* Hovered() const { return hover_; }

private:
    Element* FindImpl(Element* node, const std::string& name);
    Element* HitTest(Element* node, const math::Vec2& p);
    void DispatchDraw(Element* node, gfx::Renderer& renderer);

    gfx::Renderer* renderer_ = nullptr;
    Theme theme_;
    Element root_;
    Element* hover_ = nullptr;
    Element* focus_ = nullptr;
    Element* dragTarget_ = nullptr;
    platform::MouseButton dragButton_ = platform::MouseButton::Left;
};

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

class Panel : public Element {
public:
    gfx::Color background;
    bool drawBorder = true;
    bool fill = true;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
};

class Label : public Element {
public:
    std::string text;
    gfx::Color color;

    Label() = default;
    explicit Label(std::string text_) : text(std::move(text_)) {}

    math::Vec2 Measure(const Theme& theme) override;
    void Draw(gfx::Renderer& r, const Theme& theme) override;
};

class Button : public Element {
public:
    std::string text;
    std::function<void()> onClick;
    bool enabled = true;

    math::Vec2 Measure(const Theme& theme) override;
    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
    bool MouseUp(const math::Vec2&, platform::MouseButton) override;
    bool MouseMove(const math::Vec2&) override;

private:
    bool hovered_ = false;
    bool pressed_ = false;
};

class TextField : public Element {
public:
    std::string text;
    std::function<void(const std::string&)> onChange;
    std::function<void()> onEnter;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
    void Key(platform::Key key, bool down) override;
    void TextInput(const std::string& utf8) override;
    void OnFocusChanged(bool focused) override;

private:
    size_t cursor_ = 0;
    bool focused_ = false;
};

class Slider : public Element {
public:
    float value = 0.0f;
    float min = 0.0f;
    float max = 1.0f;
    std::function<void(float)> onChange;

    math::Vec2 Measure(const Theme& theme) override;
    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
    bool MouseMove(const math::Vec2&) override;
    bool MouseUp(const math::Vec2&, platform::MouseButton) override;

private:
    void SetFromPos(const math::Vec2& p);
    bool dragging_ = false;
};

class CheckBox : public Element {
public:
    bool checked = false;
    std::string text;
    std::function<void(bool)> onChange;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
};

class VBox : public Element {
public:
    float spacing = 4.0f;
    float padding = 6.0f;
    void Layout(const Theme& theme, const math::Rect2& area) override;
};

class HBox : public Element {
public:
    float spacing = 4.0f;
    float padding = 6.0f;
    void Layout(const Theme& theme, const math::Rect2& area) override;
};

class ScrollArea : public Element {
public:
    float scrollOffset = 0.0f;
    float contentHeight = 0.0f;
    bool autoFit = true;

    void Layout(const Theme& theme, const math::Rect2& area) override;
    void DrawTree(gfx::Renderer& r, const Theme& theme) override;
    bool Scroll(float delta) override;
};

class Window : public Element {
public:
    std::string title;
    std::function<void()> onClose;
    bool closable = true;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
    bool MouseUp(const math::Vec2&, platform::MouseButton) override;
    bool MouseMove(const math::Vec2&) override;
    void Layout(const Theme& theme, const math::Rect2& area) override;

private:
    bool dragging_ = false;
    math::Vec2 dragOffset_;
    bool closeHovered_ = false;
};

class List : public Element {
public:
    std::vector<std::string> items;
    int selected = -1;
    std::function<void(int)> onSelect;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2&, platform::MouseButton) override;
    math::Vec2 Measure(const Theme& theme) override;

private:
    static constexpr float kItemHeight = 22.0f;
};

// ---------------------------------------------------------------------------
// Deep widgets: TreeView / ComboBox / TabBar / DockLayout
// ---------------------------------------------------------------------------

struct TreeNode {
    std::string text;
    bool expanded = true;
    std::vector<TreeNode> children;
    // Flat row path, e.g. "0/2/1". Empty = not part of a tree (root helper).
    std::string path;
};

class TreeView : public Element {
public:
    std::vector<TreeNode> nodes;
    std::string selectedPath;
    std::function<void(const std::string& path, const std::string& text)> onSelect;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2& p, platform::MouseButton b) override;
    math::Vec2 Measure(const Theme& theme) override;

private:
    static constexpr float kRowHeight = 20.0f;
    static constexpr float kIndent = 14.0f;
    void DrawNode(gfx::Renderer& r, const Theme& theme, const TreeNode& node,
                  const std::string& path, float depth, float& y) const;
    bool HitNode(const TreeNode& node, const std::string& path, float depth,
                 const math::Vec2& local, float& y, bool& onArrow,
                 std::string& hitPath) const;
};

class ComboBox : public Element {
public:
    std::vector<std::string> options;
    int selected = -1;
    std::function<void(int)> onChange;
    bool open = false;

    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2& p, platform::MouseButton b) override;
    bool MouseMove(const math::Vec2& p) override;
    bool MouseUp(const math::Vec2&, platform::MouseButton b) override;
    math::Vec2 Measure(const Theme& theme) override;

private:
    int hoveredRow_ = -1;
    static constexpr float kRowHeight = 20.0f;
};

// Tab container: children are the pages, only the active one is visible.
class TabBar : public Element {
public:
    std::vector<std::string> tabs;
    int active = 0;
    std::function<void(int)> onChange;
    float tabHeight = 26.0f;

    void Layout(const Theme& theme, const math::Rect2& area) override;
    void DrawTree(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2& p, platform::MouseButton b) override;
    math::Vec2 Measure(const Theme& theme) override;
};

// Splits children into left/center/right panes with draggable splitters.
// Supports 2 (left+center) or 3 (left+center+right) children.
class DockLayout : public Element {
public:
    float leftRatio = 0.25f;
    float rightRatio = 0.25f;
    float splitterW = 4.0f;

    void Layout(const Theme& theme, const math::Rect2& area) override;
    void Draw(gfx::Renderer& r, const Theme& theme) override;
    bool MouseDown(const math::Vec2& p, platform::MouseButton b) override;
    bool MouseMove(const math::Vec2& p) override;
    bool MouseUp(const math::Vec2&, platform::MouseButton b) override;
    math::Vec2 Measure(const Theme& theme) override;

private:
    int draggingSplit_ = 0; // 1 = left splitter, 2 = right splitter
    bool hoveredSplit_ = false;
};

} // namespace neon::ui
