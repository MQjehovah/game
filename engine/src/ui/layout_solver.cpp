#include "neon/ui/layout_solver.hpp"

#include <algorithm>

namespace neon::ui {

// ---------------------------------------------------------------------------
// Text measure adapter
// ---------------------------------------------------------------------------

math::Vec2 FontTextMeasure::Measure(const std::string& text, float fontSize) const {
    if (!font_ || !font_->Valid() || text.empty()) return {0.0f, fontSize};
    return font_->Measure(text, fontSize);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

std::map<std::string, ILayoutSolver*>& LayoutSolverRegistry() {
    static std::map<std::string, ILayoutSolver*> registry = [] {
        std::map<std::string, ILayoutSolver*> r;
        r[AbsoluteLayoutSolver()->Name()] = AbsoluteLayoutSolver();
        r[BoxFlexLayoutSolver()->Name()] = BoxFlexLayoutSolver();
        return r;
    }();
    return registry;
}

ILayoutSolver* FindLayoutSolver(const std::string& name) {
    const auto& r = LayoutSolverRegistry();
    const auto it = r.find(name);
    return it == r.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// "absolute" - legacy chain of local rects (pre-box-layout behavior)
// ---------------------------------------------------------------------------

namespace {

class AbsoluteSolver final : public ILayoutSolver {
public:
    std::string Name() const override { return "absolute"; }
    void Solve(UiNode& root, const math::Vec2& viewportSize,
               const ITextMeasure&) const override {
        SolveNode(root, {0.0f, 0.0f, viewportSize.x, viewportSize.y});
    }

private:
    static void SolveNode(UiNode& n, const math::Rect2& parentAbs) {
        n.resolved = {parentAbs.x + n.rect.x, parentAbs.y + n.rect.y, n.rect.w, n.rect.h};
        n.resolvedValid = true;
        for (auto& c : n.children) SolveNode(*c, n.resolved);
    }
};

} // namespace

ILayoutSolver* AbsoluteLayoutSolver() {
    static AbsoluteSolver solver;
    return &solver;
}

// ---------------------------------------------------------------------------
// "boxflex" - CSS-style boxes + Row/Column flex containers
// ---------------------------------------------------------------------------

namespace {

// Size resolution against a known parent length. Auto/Unset fall back to
// `fallback` here; the Auto case is resolved by the caller via MeasureNode.
float ResolveSize(const UiLength& l, float parentLen, float fallback) {
    switch (l.unit) {
        case UiLength::Unit::Px: return l.value;
        case UiLength::Unit::Percent: return l.value * parentLen;
        default: return fallback;
    }
}

// One axis of the box position: near (left/top), far (right/bottom), center,
// or the legacy local rect offset. All relative to the parent CONTENT box.
float ResolveAxis(const UiLength& near, const UiLength& far, float base, float parentLen,
                  float selfLen, float fallback) {
    if (near.unit == UiLength::Unit::Center)
        return base + (parentLen - selfLen) * 0.5f;
    if (near.IsSet())
        return base + (near.unit == UiLength::Unit::Percent ? near.value * parentLen
                                                            : near.value);
    if (far.IsSet())
        return base + parentLen -
               (far.unit == UiLength::Unit::Percent ? far.value * parentLen : far.value) -
               selfLen;
    return base + fallback; // legacy: local rect offset inside the parent
}

// Natural size of a subtree (used by `auto` sizes and flex measuring).
// Percent lengths have no parent here yet, so they fall back to `rect`.
math::Vec2 MeasureNode(const UiNode& n, const ITextMeasure& m) {
    switch (n.type) {
        case UiNodeType::Label:
        case UiNodeType::Button:
            return m.Measure(n.text, n.fontSize);
        case UiNodeType::Row: {
            float main = 0.0f, cross = 0.0f;
            bool first = true;
            for (const auto& c : n.children) {
                if (!c->visible) continue;
                math::Vec2 cs = MeasureNode(*c, m);
                cs.x = c->width.IsSet() ? ResolveSize(c->width, 0.0f, cs.x) : cs.x;
                cs.y = c->height.IsSet() ? ResolveSize(c->height, 0.0f, cs.y) : cs.y;
                main += cs.x + (first ? 0.0f : n.gap);
                cross = std::max(cross, cs.y);
                first = false;
            }
            return {main + n.padding * 2.0f, cross + n.padding * 2.0f};
        }
        case UiNodeType::Column: {
            float main = 0.0f, cross = 0.0f;
            bool first = true;
            for (const auto& c : n.children) {
                if (!c->visible) continue;
                math::Vec2 cs = MeasureNode(*c, m);
                cs.x = c->width.IsSet() ? ResolveSize(c->width, 0.0f, cs.x) : cs.x;
                cs.y = c->height.IsSet() ? ResolveSize(c->height, 0.0f, cs.y) : cs.y;
                main += cs.y + (first ? 0.0f : n.gap);
                cross = std::max(cross, cs.x);
                first = false;
            }
            return {cross + n.padding * 2.0f, main + n.padding * 2.0f};
        }
        default:
            return {n.rect.w, n.rect.h};
    }
}

class BoxFlexSolver final : public ILayoutSolver {
public:
    std::string Name() const override { return "boxflex"; }
    void Solve(UiNode& root, const math::Vec2& viewportSize,
               const ITextMeasure& measure) const override {
        // The root IS the viewport (its own rect/box fields are ignored).
        root.resolved = {0.0f, 0.0f, viewportSize.x, viewportSize.y};
        root.resolvedValid = true;
        LayoutChildrenOf(root, measure);
    }

private:
    // Position + size one node inside its parent's content box, then lay out
    // its own children.
    static void LayoutNode(UiNode& n, const math::Rect2& content,
                           const ITextMeasure& measure) {
        const math::Vec2 natural = MeasureNode(n, measure);
        const float w = n.width.unit == UiLength::Unit::Auto
                            ? natural.x
                            : ResolveSize(n.width, content.w, n.rect.w);
        const float h = n.height.unit == UiLength::Unit::Auto
                            ? natural.y
                            : ResolveSize(n.height, content.h, n.rect.h);
        const float x = ResolveAxis(n.left, n.right, content.x, content.w, w, n.rect.x);
        const float y = ResolveAxis(n.top, n.bottom, content.y, content.h, h, n.rect.y);
        n.resolved = {x, y, w, h};
        n.resolvedValid = true;
        LayoutChildrenOf(n, measure);
    }

    static math::Rect2 ContentBox(const UiNode& n) {
        const float p = n.padding;
        return {n.resolved.x + p, n.resolved.y + p, std::max(0.0f, n.resolved.w - p * 2.0f),
                std::max(0.0f, n.resolved.h - p * 2.0f)};
    }

    // Dispatch the children of an already-resolved node.
    static void LayoutChildrenOf(UiNode& n, const ITextMeasure& measure) {
        const math::Rect2 inner = ContentBox(n);
        if (n.IsFlex()) {
            LayoutFlex(n, inner, measure);
            return;
        }
        for (auto& c : n.children)
            if (c->visible) LayoutNode(*c, inner, measure);
    }

    // Row/Column: flow the children along the main axis (justify), place them
    // on the cross axis (align), then recurse into each subtree.
    static void LayoutFlex(UiNode& n, const math::Rect2& inner,
                           const ITextMeasure& measure) {
        const bool row = n.type == UiNodeType::Row;
        const float mainLen = row ? inner.w : inner.h;
        const float crossLen = row ? inner.h : inner.w;

        // Pass 1: main-axis natural sizes (auto sizes measured).
        struct Item {
            UiNode* node;
            float main;
            float cross;
        };
        std::vector<Item> items;
        items.reserve(n.children.size());
        float total = 0.0f;
        for (auto& c : n.children) {
            if (!c->visible) continue;
            const math::Vec2 nat = MeasureNode(*c, measure);
            Item it;
            it.node = c.get();
            it.main = c->width.unit == UiLength::Unit::Auto
                          ? (row ? nat.x : nat.y)
                          : ResolveSize(row ? c->width : c->height, mainLen,
                                        row ? c->rect.w : c->rect.h);
            it.cross = c->height.unit == UiLength::Unit::Auto
                           ? (row ? nat.y : nat.x)
                           : ResolveSize(row ? c->height : c->width, crossLen,
                                         row ? c->rect.h : c->rect.w);
            items.push_back(it);
            total += it.main;
        }
        if (items.empty()) return;
        total += n.gap * static_cast<float>(items.size() - 1);
        const float free = mainLen - total;

        // Pass 2: distribute along the main axis.
        float cursor = inner.x; // row; for column this is inner.y
        float gap = n.gap;
        if (free > 0.0f) {
            switch (n.justify) {
                case UiJustify::Center: cursor += free * 0.5f; break;
                case UiJustify::End: cursor += free; break;
                case UiJustify::SpaceBetween:
                    gap += items.size() > 1 ? free / static_cast<float>(items.size() - 1)
                                            : 0.0f;
                    break;
                case UiJustify::Start: break;
            }
        }
        for (const Item& it : items) {
            UiNode& c = *it.node;
            // Cross-axis placement (stretch overrides the child's cross size).
            float cross = it.cross;
            float crossPos;
            switch (n.align) {
                case UiAlign::Center: crossPos = (row ? inner.y : inner.x) + (crossLen - cross) * 0.5f; break;
                case UiAlign::End: crossPos = (row ? inner.y : inner.x) + crossLen - cross; break;
                case UiAlign::Stretch:
                    cross = crossLen;
                    crossPos = row ? inner.y : inner.x;
                    break;
                default: crossPos = row ? inner.y : inner.x; break;
            }
            if (row) {
                c.resolved = {cursor, crossPos, it.main, cross};
            } else {
                c.resolved = {crossPos, cursor, cross, it.main};
            }
            c.resolvedValid = true;
            cursor += it.main + gap;
            LayoutChildrenOf(c, measure);
        }
    }
};

} // namespace

ILayoutSolver* BoxFlexLayoutSolver() {
    static BoxFlexSolver solver;
    return &solver;
}

} // namespace neon::ui
