#pragma once

#include <map>
#include <string>

#include "neon/math/math.hpp"
#include "neon/ui/document.hpp"

namespace neon::ui {

// Text measuring service: decouples layout strategies from the concrete font
// type (and lets tests run without a GPU-backed font). Returns the design-space
// ink size of `text` at `fontSize`.
class ITextMeasure {
public:
    virtual ~ITextMeasure() = default;
    virtual math::Vec2 Measure(const std::string& text, float fontSize) const = 0;
};

// gfx::Font adapter (null font -> everything measures zero).
class FontTextMeasure final : public ITextMeasure {
public:
    explicit FontTextMeasure(const gfx::Font* font) : font_(font) {}
    math::Vec2 Measure(const std::string& text, float fontSize) const override;

private:
    const gfx::Font* font_;
};

// A pluggable layout strategy. Solvers resolve the whole node tree against a
// viewport (design units) and fill every node's `resolved` rect. Documents
// pick a solver by name ("solver" field, default "boxflex"); new layout
// models (grid, constraints, anchors, ...) register alongside the built-ins
// without touching the document or renderer.
class ILayoutSolver {
public:
    virtual ~ILayoutSolver() = default;
    virtual std::string Name() const = 0;
    virtual void Solve(UiNode& root, const math::Vec2& viewportSize,
                       const ITextMeasure& measure) const = 0;
};

// Built-in solvers:
//  "absolute" - legacy chain of local `rect`s (pre-box-layout behavior).
//  "boxflex"  - CSS-style box positioning (left/top/right/bottom, width/
//               height in px/%/auto/center) + Row/Column flex containers
//               (gap/justify/align/padding). Nodes without box fields fall
//               back to their local `rect`, so legacy documents render
//               identically under this solver too.
ILayoutSolver* AbsoluteLayoutSolver();
ILayoutSolver* BoxFlexLayoutSolver();

// Registry: name -> solver. Registration order defines lookup only.
std::map<std::string, ILayoutSolver*>& LayoutSolverRegistry();
ILayoutSolver* FindLayoutSolver(const std::string& name);

} // namespace neon::ui
