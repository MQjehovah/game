#include "neon/ui/system.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>

namespace neon::ui {

// ---------------------------------------------------------------------------
// Element / UIManager
// ---------------------------------------------------------------------------

void Element::Layout(const Theme& theme, const math::Rect2& area) {
    // Adopt the parent-provided area only when this element has no explicit
    // rect of its own. Rects are relative to the parent; AbsolutePos() sums
    // ancestors to recover screen-space coordinates.
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    math::Rect2 content{0.0f, 0.0f, rect.w, rect.h};
    for (auto& child : children) child->Layout(theme, content);
}

void Element::DrawTree(gfx::Renderer& renderer, const Theme& theme) {
    if (!visible) return;
    Draw(renderer, theme);
    for (auto& child : children) child->DrawTree(renderer, theme);
}

void UIManager::Init(gfx::Renderer* renderer, const gfx::Font& font) {
    renderer_ = renderer;
    theme_.font = font;
}

void UIManager::SetFocus(Element* element) {
    if (focus_ == element) return;
    if (focus_) focus_->OnFocusChanged(false);
    focus_ = element;
    if (focus_) focus_->OnFocusChanged(true);
}

Element* UIManager::HitTest(Element* node, const math::Vec2& p) {
    if (!node || !node->visible) return nullptr;
    if (!node->Contains(p)) return nullptr;
    // Descendants use rects relative to their parent.
    math::Vec2 local{p.x - node->rect.x, p.y - node->rect.y};
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
        if (Element* hit = HitTest(it->get(), local)) return hit;
    }
    return node;
}

Element* UIManager::FindImpl(Element* node, const std::string& name) {
    if (!node) return nullptr;
    if (node->name == name) return node;
    for (auto& child : node->children) {
        if (Element* e = FindImpl(child.get(), name)) return e;
    }
    return nullptr;
}

Element* UIManager::Find(const std::string& name) { return FindImpl(&root_, name); }

Element* UIManager::HitTestAt(const math::Vec2& p) { return HitTest(&root_, p); }

void UIManager::Update(platform::IInput& input) {
    if (!renderer_) return;
    math::Vec2 mp = renderer_->ScreenToUI(input.MousePos());

    hover_ = HitTest(&root_, mp);

    if (dragTarget_) {
        dragTarget_->MouseMove(mp);
        if (input.MouseReleased(dragButton_)) {
            dragTarget_->MouseUp(mp, dragButton_);
            dragTarget_ = nullptr;
        }
    }

    const platform::MouseButton buttons[3] = {platform::MouseButton::Left,
                                              platform::MouseButton::Right,
                                              platform::MouseButton::Middle};
    for (platform::MouseButton b : buttons) {
        if (input.MousePressed(b)) {
            Element* hit = HitTest(&root_, mp);
            if (hit && hit->MouseDown(mp, b)) {
                dragTarget_ = hit;
                dragButton_ = b;
                SetFocus(hit);
            } else {
                SetFocus(nullptr);
            }
        }
    }

    float wheel = input.WheelDelta();
    if (wheel != 0.0f && hover_ && !hover_->Scroll(wheel)) {
        for (Element* e = hover_->parent; e; e = e->parent) {
            if (e->Scroll(wheel)) break;
        }
    }

    root_.Layout(theme_, {0, 0, static_cast<float>(gfx::Renderer::kDesignWidth),
                          static_cast<float>(gfx::Renderer::kDesignHeight)});
}

void UIManager::Draw(gfx::Renderer& renderer) {
    DispatchDraw(&root_, renderer);
}

void UIManager::TextInput(const std::string& text) {
    if (focus_) focus_->TextInput(text);
}

void UIManager::Key(platform::Key key, bool down) {
    if (focus_) focus_->Key(key, down);
}

void UIManager::DispatchDraw(Element* node, gfx::Renderer& renderer) {
    if (!node->visible) return;
    node->DrawTree(renderer, theme_);
}

// ---------------------------------------------------------------------------
// Panel / Label / Button
// ---------------------------------------------------------------------------

void Panel::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    if (fill) {
        r.DrawRect(p, {rect.w, rect.h}, background.a > 0.0f ? background : theme.panelBg);
    }
    if (drawBorder) r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, theme.border);
}

math::Vec2 Label::Measure(const Theme& theme) {
    if (theme.font.Valid()) return theme.font.Measure(text, theme.fontSize);
    return {0, 0};
}

void Label::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawText(theme.font, text, {p.x, p.y + rect.h * 0.5f}, theme.fontSize,
               color.a > 0.0f ? color : theme.text, false, true);
}

math::Vec2 Button::Measure(const Theme& theme) {
    math::Vec2 m = theme.font.Measure(text, theme.fontSize);
    return {m.x + 20.0f, theme.fontSize + 12.0f};
}

void Button::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    gfx::Color bg = !enabled ? theme.inputBg
                    : pressed_ ? theme.pressed
                    : hovered_ ? theme.hover
                               : theme.panelBg;
    r.DrawRect(p, {rect.w, rect.h}, bg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f,
                      enabled ? theme.border : theme.dim);
    r.DrawText(theme.font, text, {p.x + rect.w * 0.5f, p.y + rect.h * 0.5f}, theme.fontSize,
               enabled ? theme.text : theme.dim, true, true);
}

bool Button::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (!enabled || b != platform::MouseButton::Left) return false;
    pressed_ = true;
    return true;
}

bool Button::MouseUp(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    bool wasPressed = pressed_;
    pressed_ = false;
    if (wasPressed && hovered_ && onClick) onClick();
    return true;
}

bool Button::MouseMove(const math::Vec2& p) {
    math::Vec2 a = AbsolutePos();
    hovered_ = p.x >= a.x && p.x <= a.x + rect.w && p.y >= a.y && p.y <= a.y + rect.h;
    return hovered_;
}

// ---------------------------------------------------------------------------
// TextField
// ---------------------------------------------------------------------------

void TextField::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, focused_ ? theme.accent : theme.border);
    float pad = 6.0f;
    r.DrawText(theme.font, text, {p.x + pad, p.y + rect.h * 0.5f}, theme.fontSize, theme.text,
               false, true);
    if (focused_) {
        float tw = theme.font.Measure(text, theme.fontSize).x;
        r.DrawRect({p.x + pad + tw + 2.0f, p.y + rect.h * 0.22f}, {1.5f, rect.h * 0.56f},
                   theme.accent);
    }
}

bool TextField::MouseDown(const math::Vec2&, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    return true;
}

void TextField::OnFocusChanged(bool focused) {
    focused_ = focused;
    if (!focused && onChange) onChange(text);
}

void TextField::TextInput(const std::string& utf8) {
    text += utf8;
    if (onChange) onChange(text);
}

void TextField::Key(platform::Key key, bool down) {
    if (!down) return;
    if (key == platform::Key::Backspace && !text.empty()) {
        // Remove the last UTF-8 codepoint.
        size_t n = text.size();
        size_t cut = n;
        while (cut > 0 && (static_cast<unsigned char>(text[cut - 1]) & 0xC0) == 0x80) --cut;
        if (cut > 0) --cut;
        text.erase(cut);
        if (onChange) onChange(text);
    } else if (key == platform::Key::Enter && onEnter) {
        onEnter();
    }
}

// ---------------------------------------------------------------------------
// Slider / CheckBox
// ---------------------------------------------------------------------------

math::Vec2 Slider::Measure(const Theme& theme) {
    return {100.0f, theme.fontSize + 10.0f};
}

void Slider::SetFromPos(const math::Vec2& p) {
    math::Vec2 a = AbsolutePos();
    float t = (p.x - a.x - 6.0f) / std::max(1.0f, rect.w - 12.0f);
    value = min + (max - min) * std::clamp(t, 0.0f, 1.0f);
    if (onChange) onChange(value);
}

void Slider::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    float frac = (value - min) / std::max(1e-4f, max - min);
    float trackY = p.y + rect.h * 0.5f - 2.0f;
    r.DrawRect({p.x, trackY}, {rect.w, 4.0f}, theme.inputBg);
    r.DrawRect({p.x, trackY}, {rect.w * frac, 4.0f}, theme.accent);
    float hx = p.x + rect.w * frac;
    r.DrawRect({hx - 4.0f, p.y + rect.h * 0.5f - 7.0f}, {8.0f, 14.0f}, theme.text);
}

bool Slider::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    dragging_ = true;
    SetFromPos(p);
    return true;
}

bool Slider::MouseMove(const math::Vec2& p) {
    if (dragging_) SetFromPos(p);
    return dragging_;
}

bool Slider::MouseUp(const math::Vec2&, platform::MouseButton b) {
    if (b == platform::MouseButton::Left) dragging_ = false;
    return false;
}

void CheckBox::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    float box = rect.h;
    r.DrawRect(p, {box, box}, theme.inputBg);
    r.DrawRectOutline({p.x, p.y, box, box}, 1.0f, checked ? theme.accent : theme.border);
    if (checked) {
        r.DrawText(theme.font, "x", {p.x + box * 0.5f, p.y + box * 0.5f}, theme.fontSize,
                   theme.accent, true, true);
    }
    r.DrawText(theme.font, text, {p.x + box + 6.0f, p.y + box * 0.5f}, theme.fontSize, theme.text,
               false, true);
}

bool CheckBox::MouseDown(const math::Vec2&, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    checked = !checked;
    if (onChange) onChange(checked);
    return true;
}

// ---------------------------------------------------------------------------
// Layout containers
// ---------------------------------------------------------------------------

void VBox::Layout(const Theme& theme, const math::Rect2& area) {
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    float y = padding;
    for (auto& child : children) {
        if (!child->visible) continue;
        math::Vec2 m = child->Measure(theme);
        math::Rect2 childArea{padding, y, std::max(0.0f, rect.w - padding * 2.0f),
                              m.y > 0.0f ? m.y : 22.0f};
        child->Layout(theme, childArea);
        y += childArea.h + spacing;
    }
}

void HBox::Layout(const Theme& theme, const math::Rect2& area) {
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    float x = padding;
    float contentW = std::max(0.0f, rect.w - padding * 2.0f);
    float total = 0.0f;
    for (auto& child : children) {
        if (!child->visible) continue;
        math::Vec2 m = child->Measure(theme);
        total += (m.x > 0.0f ? m.x : 60.0f);
    }
    total += spacing * std::max(0, static_cast<int>(children.size()) - 1);
    float stretch = children.empty() ? 0.0f : std::max(0.0f, contentW - total) /
                                                    static_cast<float>(children.size());
    for (auto& child : children) {
        if (!child->visible) continue;
        math::Vec2 m = child->Measure(theme);
        float w = (m.x > 0.0f ? m.x : 60.0f) + stretch;
        child->Layout(theme, {x, padding, w, std::max(0.0f, rect.h - padding * 2.0f)});
        x += w + spacing;
    }
}

// ---------------------------------------------------------------------------
// ScrollArea / Window / List
// ---------------------------------------------------------------------------

void ScrollArea::Layout(const Theme& theme, const math::Rect2& area) {
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    contentHeight = 0.0f;
    float y = 4.0f - scrollOffset;
    for (auto& child : children) {
        if (!child->visible) continue;
        math::Vec2 m = child->Measure(theme);
        float childH = m.y > 0.0f ? m.y : 22.0f;
        child->Layout(theme, {4.0f, y, std::max(0.0f, rect.w - 8.0f), childH});
        y += childH + 4.0f;
        contentHeight = y + scrollOffset;
    }
}

void ScrollArea::DrawTree(gfx::Renderer& r, const Theme& theme) {
    if (!visible) return;
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, theme.border);
    math::Vec2 abs = AbsolutePos();
    r.Backend()->SetScissor(
        static_cast<int>(abs.x * r.UIScale()),
        static_cast<int>(abs.y * r.UIScale()),
        static_cast<int>(rect.w * r.UIScale()),
        static_cast<int>(rect.h * r.UIScale()), true);
    for (auto& child : children) child->DrawTree(r, theme);
    r.Backend()->SetScissor(0, 0, 0, 0, false);
}

bool ScrollArea::Scroll(float delta) {
    scrollOffset = std::max(0.0f, scrollOffset - delta * 24.0f);
    float maxScroll = std::max(0.0f, contentHeight - rect.h);
    scrollOffset = std::min(maxScroll, scrollOffset);
    return true;
}

void Window::Layout(const Theme& theme, const math::Rect2& area) {
    (void)area;
    float titleH = 26.0f;
    for (auto& child : children) {
        child->Layout(theme, {4.0f, titleH, rect.w - 8.0f, rect.h - titleH - 4.0f});
    }
}

void Window::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.windowBg);
    r.DrawRect({p.x, p.y}, {rect.w, 26.0f}, theme.panelBg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, theme.border);
    r.DrawText(theme.font, title, {p.x + 8.0f, p.y + 13.0f}, theme.fontSize, theme.text, false,
               true);
    if (closable) {
        math::Rect2 closeRect{p.x + rect.w - 22.0f, p.y + 3.0f, 18.0f, 18.0f};
        r.DrawRect({closeRect.x, closeRect.y}, {closeRect.w, closeRect.h},
                   closeHovered_ ? theme.hover : gfx::Color::Transparent);
        r.DrawText(theme.font, "x", {closeRect.x + closeRect.w * 0.5f, closeRect.y + closeRect.h * 0.5f},
                   theme.fontSize, theme.dim, true, true);
    }
    // Children (content) drawn by UIManager::DispatchDraw (we don't clip).
}

bool Window::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    if (closable) {
        math::Rect2 closeRect{abs.x + rect.w - 22.0f, abs.y + 3.0f, 18.0f, 18.0f};
        if (closeRect.Contains(p)) {
            if (onClose) onClose();
            return true;
        }
    }
    if (p.y >= abs.y && p.y <= abs.y + 26.0f) {
        dragging_ = true;
        dragOffset_ = {p.x - abs.x, p.y - abs.y};
        return true;
    }
    return false;
}

bool Window::MouseMove(const math::Vec2& p) {
    math::Vec2 abs = AbsolutePos();
    math::Rect2 closeRect{abs.x + rect.w - 22.0f, abs.y + 3.0f, 18.0f, 18.0f};
    closeHovered_ = closeRect.Contains(p);
    if (dragging_) {
        rect.x = p.x - dragOffset_.x;
        rect.y = p.y - dragOffset_.y;
        return true;
    }
    return false;
}

bool Window::MouseUp(const math::Vec2&, platform::MouseButton b) {
    if (b == platform::MouseButton::Left) dragging_ = false;
    return false;
}

math::Vec2 List::Measure(const Theme& theme) {
    return {120.0f, static_cast<float>(items.size()) * kItemHeight + 4.0f};
}

void List::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    for (size_t i = 0; i < items.size(); ++i) {
        float y = p.y + static_cast<float>(i) * kItemHeight;
        if (static_cast<int>(i) == selected) {
            r.DrawRect({p.x, y}, {rect.w, kItemHeight}, theme.accent);
        }
        r.DrawText(theme.font, items[i], {p.x + 6.0f, y + kItemHeight * 0.5f},
                   std::min(theme.fontSize, 13.0f),
                   static_cast<int>(i) == selected ? gfx::Color{0.05f, 0.05f, 0.08f, 1.0f}
                                                   : theme.text,
                   false, true);
    }
}

bool List::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    int index = static_cast<int>((p.y - abs.y) / kItemHeight);
    if (index >= 0 && index < static_cast<int>(items.size())) {
        selected = index;
        if (onSelect) onSelect(index);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// TreeView
// ---------------------------------------------------------------------------

namespace {

std::string JoinPath(const std::string& prefix, int index) {
    return prefix.empty() ? std::to_string(index) : prefix + "/" + std::to_string(index);
}

std::vector<int> ParsePath(const std::string& path) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) out.push_back(std::atoi(part.c_str()));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

} // namespace

void TreeView::DrawNode(gfx::Renderer& r, const Theme& theme, const TreeNode& node,
                        const std::string& path, float depth, float& y) const {
    math::Vec2 p = AbsolutePos();
    float rowY = p.y + y;
    bool selected = selectedPath == path;
    if (selected) r.DrawRect({p.x, rowY}, {rect.w, kRowHeight}, theme.accent);
    float x = p.x + 6.0f + depth * kIndent;
    if (!node.children.empty()) {
        r.DrawText(theme.font, node.expanded ? "-" : "+",
                   {x + 5.0f, rowY + kRowHeight * 0.5f}, 13.0f, theme.text, true, true);
    }
    r.DrawText(theme.font, node.text, {x + 16.0f, rowY + kRowHeight * 0.5f},
               theme.fontSize,
               selected ? gfx::Color{0.05f, 0.05f, 0.08f, 1.0f} : theme.text, false, true);
    y += kRowHeight;
    if (node.expanded) {
        for (size_t i = 0; i < node.children.size(); ++i) {
            DrawNode(r, theme, node.children[i], JoinPath(path, static_cast<int>(i)),
                     depth + 1.0f, y);
        }
    }
}

void TreeView::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, theme.border);
    float y = 2.0f;
    for (size_t i = 0; i < nodes.size(); ++i) {
        DrawNode(r, theme, nodes[i], std::to_string(i), 0.0f, y);
    }
}

bool TreeView::HitNode(const TreeNode& node, const std::string& path, float depth,
                       const math::Vec2& local, float& y, bool& onArrow,
                       std::string& hitPath) const {
    if (local.y >= y && local.y < y + kRowHeight) {
        onArrow = !node.children.empty() && local.x >= 6.0f + depth * kIndent - 2.0f &&
                  local.x <= 6.0f + depth * kIndent + 16.0f;
        hitPath = path;
        return true;
    }
    y += kRowHeight;
    if (node.expanded) {
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (HitNode(node.children[i], JoinPath(path, static_cast<int>(i)), depth + 1.0f,
                        local, y, onArrow, hitPath)) {
                return true;
            }
        }
    }
    return false;
}

bool TreeView::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    math::Vec2 local{p.x - abs.x, p.y - abs.y};
    if (local.x < 0.0f || local.x > rect.w || local.y < 0.0f || local.y > rect.h) return false;

    float y = 2.0f;
    bool onArrow = false;
    std::string hitPath;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (HitNode(nodes[i], std::to_string(i), 0.0f, local, y, onArrow, hitPath)) {
            if (onArrow) {
                // Toggle the node at hitPath.
                std::vector<int> idx = ParsePath(hitPath);
                std::vector<TreeNode>* level = &nodes;
                TreeNode* target = nullptr;
                for (int i2 : idx) {
                    if (i2 < 0 || i2 >= static_cast<int>(level->size())) break;
                    target = &(*level)[static_cast<size_t>(i2)];
                    level = &target->children;
                }
                if (target) target->expanded = !target->expanded;
                return true;
            }
            selectedPath = hitPath;
            if (onSelect) {
                // Resolve text for the callback.
                std::vector<int> idx = ParsePath(hitPath);
                std::vector<TreeNode>* level = &nodes;
                const TreeNode* target = nullptr;
                for (int i2 : idx) {
                    if (i2 < 0 || i2 >= static_cast<int>(level->size())) break;
                    target = &(*level)[static_cast<size_t>(i2)];
                    level = const_cast<std::vector<TreeNode>*>(&target->children);
                }
                if (target) onSelect(hitPath, target->text);
            }
            return true;
        }
    }
    return false;
}

math::Vec2 TreeView::Measure(const Theme& theme) {
    (void)theme;
    int count = 0;
    std::function<void(const TreeNode&)> countNodes = [&](const TreeNode& node) {
        ++count;
        if (node.expanded) {
            for (const TreeNode& child : node.children) countNodes(child);
        }
    };
    for (const TreeNode& node : nodes) countNodes(node);
    return {200.0f, static_cast<float>(count) * kRowHeight + 4.0f};
}

// ---------------------------------------------------------------------------
// ComboBox
// ---------------------------------------------------------------------------

void ComboBox::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    r.DrawRectOutline({p.x, p.y, rect.w, rect.h}, 1.0f, theme.border);
    std::string label = selected >= 0 && selected < static_cast<int>(options.size())
                            ? options[static_cast<size_t>(selected)]
                            : "选择...";
    r.DrawText(theme.font, label, {p.x + 6.0f, p.y + rect.h * 0.5f}, theme.fontSize, theme.text,
               false, true);
    r.DrawText(theme.font, "v", {p.x + rect.w - 10.0f, p.y + rect.h * 0.5f}, theme.fontSize,
               theme.dim, true, true);
    if (open) {
        float listH = static_cast<float>(options.size()) * kRowHeight;
        math::Rect2 list{p.x, p.y + rect.h, rect.w, listH};
        r.DrawRect({list.x, list.y}, {list.w, list.h}, theme.windowBg);
        r.DrawRectOutline(list, 1.0f, theme.accent);
        for (size_t i = 0; i < options.size(); ++i) {
            float rowY = list.y + static_cast<float>(i) * kRowHeight;
            if (static_cast<int>(i) == hoveredRow_) {
                r.DrawRect({list.x, rowY}, {list.w, kRowHeight}, theme.hover);
            }
            r.DrawText(theme.font, options[i],
                       {list.x + 6.0f, rowY + kRowHeight * 0.5f}, theme.fontSize,
                       static_cast<int>(i) == selected ? theme.accent : theme.text, false, true);
        }
    }
}

bool ComboBox::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    if (p.x >= abs.x && p.x <= abs.x + rect.w && p.y >= abs.y && p.y <= abs.y + rect.h) {
        open = !open;
        hoveredRow_ = -1;
        return true;
    }
    if (open && p.y >= abs.y + rect.h &&
        p.y <= abs.y + rect.h + static_cast<float>(options.size()) * kRowHeight) {
        int index = static_cast<int>((p.y - (abs.y + rect.h)) / kRowHeight);
        if (index >= 0 && index < static_cast<int>(options.size())) {
            selected = index;
            if (onChange) onChange(index);
        }
        open = false;
        return true;
    }
    open = false;
    return false;
}

bool ComboBox::MouseMove(const math::Vec2& p) {
    if (!open) return false;
    math::Vec2 abs = AbsolutePos();
    hoveredRow_ = static_cast<int>((p.y - (abs.y + rect.h)) / kRowHeight);
    return true;
}

bool ComboBox::MouseUp(const math::Vec2&, platform::MouseButton) { return false; }

math::Vec2 ComboBox::Measure(const Theme& theme) {
    (void)theme;
    return {140.0f, 24.0f};
}

// ---------------------------------------------------------------------------
// TabBar
// ---------------------------------------------------------------------------

void TabBar::Layout(const Theme& theme, const math::Rect2& area) {
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    for (size_t i = 0; i < children.size(); ++i) {
        Element* page = children[i].get();
        page->visible = (static_cast<int>(i) == active);
        if (page->visible) {
            page->Layout(theme, {0.0f, tabHeight, rect.w, std::max(0.0f, rect.h - tabHeight)});
        }
    }
}

void TabBar::DrawTree(gfx::Renderer& r, const Theme& theme) {
    if (!visible) return;
    math::Vec2 p = AbsolutePos();
    r.DrawRect(p, {rect.w, rect.h}, theme.inputBg);
    float tabW = children.empty() ? rect.w : rect.w / static_cast<float>(children.size());
    for (size_t i = 0; i < tabs.size() && i < children.size(); ++i) {
        math::Rect2 tab{p.x + static_cast<float>(i) * tabW, p.y, tabW, tabHeight};
        bool isActive = static_cast<int>(i) == active;
        r.DrawRect({tab.x, tab.y}, {tab.w, tab.h}, isActive ? theme.accent : theme.panelBg);
        if (isActive) r.DrawRect({tab.x, tab.y + tab.h - 2.0f}, {tab.w, 2.0f}, theme.text);
        r.DrawText(theme.font, tabs[i], {tab.x + tab.w * 0.5f, tab.y + tab.h * 0.5f},
                   theme.fontSize,
                   isActive ? gfx::Color{0.05f, 0.05f, 0.08f, 1.0f} : theme.text, true, true);
    }
    if (active >= 0 && active < static_cast<int>(children.size())) {
        children[static_cast<size_t>(active)]->DrawTree(r, theme);
    }
}

bool TabBar::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    if (p.y < abs.y || p.y > abs.y + tabHeight) return false;
    float tabW = children.empty() ? rect.w : rect.w / static_cast<float>(children.size());
    int index = static_cast<int>((p.x - abs.x) / tabW);
    if (index >= 0 && index < static_cast<int>(tabs.size())) {
        active = index;
        if (onChange) onChange(index);
        return true;
    }
    return false;
}

math::Vec2 TabBar::Measure(const Theme& theme) {
    (void)theme;
    return {320.0f, 200.0f};
}

// ---------------------------------------------------------------------------
// DockLayout
// ---------------------------------------------------------------------------

void DockLayout::Layout(const Theme& theme, const math::Rect2& area) {
    if (rect.w <= 0.0f && rect.h <= 0.0f) rect = area;
    const size_t n = children.size();
    if (n == 0) return;

    float leftW = std::max(40.0f, std::min(rect.w * leftRatio, rect.w * 0.7f));
    float rightW = n >= 3 ? std::max(40.0f, std::min(rect.w * rightRatio, rect.w * 0.7f)) : 0.0f;
    float centerW = rect.w - leftW - rightW -
                    splitterW * static_cast<float>(n >= 3 ? 2 : 1);
    centerW = std::max(0.0f, centerW);

    children[0]->Layout(theme, {0.0f, 0.0f, leftW, rect.h});
    if (n >= 2) children[1]->Layout(theme, {leftW + splitterW, 0.0f, centerW, rect.h});
    if (n >= 3) {
        children[2]->Layout(theme,
                            {leftW + splitterW + centerW + splitterW, 0.0f, rightW, rect.h});
    }
}

void DockLayout::Draw(gfx::Renderer& r, const Theme& theme) {
    math::Vec2 p = AbsolutePos();
    const size_t n = children.size();
    float leftW = std::max(40.0f, std::min(rect.w * leftRatio, rect.w * 0.7f));
    float rightW = n >= 3 ? std::max(40.0f, std::min(rect.w * rightRatio, rect.w * 0.7f)) : 0.0f;
    float centerW = rect.w - leftW - rightW -
                    splitterW * static_cast<float>(n >= 3 ? 2 : 1);
    gfx::Color splitColor = hoveredSplit_ ? theme.accent : theme.border;
    r.DrawRect({p.x + leftW, p.y}, {splitterW, rect.h}, splitColor);
    if (n >= 3) {
        r.DrawRect({p.x + leftW + splitterW + std::max(0.0f, centerW), p.y},
                   {splitterW, rect.h}, splitColor);
    }
}

bool DockLayout::MouseDown(const math::Vec2& p, platform::MouseButton b) {
    if (b != platform::MouseButton::Left) return false;
    math::Vec2 abs = AbsolutePos();
    const size_t n = children.size();
    float leftW = std::max(40.0f, std::min(rect.w * leftRatio, rect.w * 0.7f));
    float rightW = n >= 3 ? std::max(40.0f, std::min(rect.w * rightRatio, rect.w * 0.7f)) : 0.0f;
    float centerW = rect.w - leftW - rightW -
                    splitterW * static_cast<float>(n >= 3 ? 2 : 1);
    if (p.x >= abs.x + leftW - 3.0f && p.x <= abs.x + leftW + splitterW + 3.0f) {
        draggingSplit_ = 1;
        return true;
    }
    if (n >= 3) {
        float sx = abs.x + leftW + splitterW + std::max(0.0f, centerW);
        if (p.x >= sx - 3.0f && p.x <= sx + splitterW + 3.0f) {
            draggingSplit_ = 2;
            return true;
        }
    }
    return false;
}

bool DockLayout::MouseMove(const math::Vec2& p) {
    math::Vec2 abs = AbsolutePos();
    const size_t n = children.size();
    float leftW = std::max(40.0f, std::min(rect.w * leftRatio, rect.w * 0.7f));
    float rightW = n >= 3 ? std::max(40.0f, std::min(rect.w * rightRatio, rect.w * 0.7f)) : 0.0f;
    float centerW = rect.w - leftW - rightW -
                    splitterW * static_cast<float>(n >= 3 ? 2 : 1);
    hoveredSplit_ = (p.x >= abs.x + leftW - 3.0f &&
                     p.x <= abs.x + leftW + splitterW + 3.0f) ||
                    (n >= 3 &&
                     p.x >= abs.x + leftW + splitterW + std::max(0.0f, centerW) - 3.0f &&
                     p.x <= abs.x + leftW + splitterW + std::max(0.0f, centerW) + splitterW +
                                3.0f);
    if (draggingSplit_ == 1) {
        leftRatio = math::Clamp((p.x - abs.x) / std::max(1.0f, rect.w), 0.1f, 0.8f);
    } else if (draggingSplit_ == 2) {
        rightRatio = math::Clamp((rect.w - (p.x - abs.x)) / std::max(1.0f, rect.w), 0.1f, 0.8f);
    }
    return draggingSplit_ != 0;
}

bool DockLayout::MouseUp(const math::Vec2&, platform::MouseButton b) {
    if (b == platform::MouseButton::Left) draggingSplit_ = 0;
    return false;
}

math::Vec2 DockLayout::Measure(const Theme& theme) {
    (void)theme;
    return {480.0f, 260.0f};
}

} // namespace neon::ui
