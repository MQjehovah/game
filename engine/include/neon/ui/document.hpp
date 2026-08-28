#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/ui/ui.hpp"

namespace neon::ui {

// Data-driven UI document node types (the UI editor's node palette).
enum class UiNodeType : uint8_t {
    Panel,  // free-form container: children position themselves (left/top)
    Row,    // flex container: children flow horizontally (gap/justify/align)
    Column, // flex container: children flow vertically
    Label,  // text
    Button, // clickable rectangle with centered text
    Bar,    // progress/fill bar
    Image,  // textured quad (sprite path)
};

const char* UiNodeTypeName(UiNodeType type);
UiNodeType UiNodeTypeFromName(const std::string& name);

// Modern box-layout length: px | percent-of-parent ("50%") | auto (fit
// content) | center (center along that axis). Unset = fall back to `rect`.
struct UiLength {
    enum class Unit : uint8_t { Unset, Px, Percent, Auto, Center };
    Unit unit = Unit::Unset;
    float value = 0.0f; // Px / Percent numerator (50% -> 0.5f)
    bool IsSet() const { return unit != Unit::Unset; }
};

enum class UiJustify : uint8_t { Start, Center, End, SpaceBetween };
enum class UiAlign : uint8_t { Start, Center, End, Stretch };

// A node in a UI document tree. Two authoring modes coexist:
//  - Legacy: absolute `rect` (LOCAL, relative to the parent). Old documents
//    render exactly as before.
//  - Box layout: optional left/top/right/bottom + width/height lengths and,
//    on Row/Column containers, gap/justify/align/padding. Children are then
//    positioned automatically - the whole tree adapts to any viewport size.
struct UiNode {
    UiNodeType type = UiNodeType::Panel;
    std::string name;
    math::Rect2 rect;              // LOCAL rect (relative to the parent)
    UiNode* parent = nullptr;      // tree link (set by AddChild)
    gfx::Color color{1.0f, 1.0f, 1.0f, 1.0f}; // panel bg / label tint / bar fill
    gfx::Color borderColor{0.25f, 0.55f, 1.0f, 1.0f};
    std::string text;              // label / button text
    std::string sprite;            // image node: sprite path ("" = plain quad)
    // G3-5 9-slice border (design px): when > 0 and a `sprite` + texture loader
    // are available, the node's background/image is drawn as 9 quads (fixed
    // corners, stretched edges) instead of one stretched quad.
    float slice = 0.0f;
    float fill = 0.0f;             // bar 0..1
    float fontSize = 16.0f;        // label / button text size
    bool visible = true;
    bool clipChildren = true;
    std::vector<std::unique_ptr<UiNode>> children;

    // ---- Box layout (all optional; Unset falls back to `rect`) ----
    UiLength left, top, right, bottom;   // relative to the parent's CONTENT box
    UiLength width, height;              // px / % / auto(fit content) / unset=rect
    float gap = 0.0f;                    // Row/Column child spacing
    float padding = 0.0f;                // content inset (all sides)
    UiJustify justify = UiJustify::Start; // main-axis distribution
    UiAlign align = UiAlign::Start;        // cross-axis placement

    // Extension seam: every UNRECOGNIZED JSON field is preserved here
    // verbatim and written back on save. Future layout strategies (and
    // editor plugins) can carry their own attributes without touching the
    // core node struct - the document format stays forward-compatible.
    core::Json extra;

    // Resolved ABSOLUTE design-space rect, filled by UiDocument::Layout.
    // Mutable frame-cache semantics: Layout runs inside const Draw/HitTest.
    // Draw/HitTest/selection read this; editing tools still write `rect`.
    mutable math::Rect2 resolved;
    mutable bool resolvedValid = false;

    UiNode* AddChild(UiNodeType type, const std::string& childName);
    UiNode* Find(const std::string& target);
    // Legacy absolute rect (walks the parent chain). Prefer ResolvedRect()
    // after a Layout pass - box-layout nodes only have a resolved rect.
    math::Rect2 AbsoluteRect() const {
        math::Rect2 r = rect;
        for (const UiNode* p = parent; p; p = p->parent) {
            r.x += p->rect.x;
            r.y += p->rect.y;
        }
        return r;
    }
    math::Rect2 ResolvedRect() const {
        return resolvedValid ? resolved : AbsoluteRect();
    }
    bool Contains(const math::Vec2& p) const {
        return visible && ResolvedRect().Contains(p);
    }
    // True when this node lays its children out itself (Row/Column).
    bool IsFlex() const { return type == UiNodeType::Row || type == UiNodeType::Column; }
};

// Loads a texture by asset path ("" / null loader = no textured nodes).
using UiTextureLoader = std::function<gfx::Texture(const std::string&)>;

// A serializable UI document: one root node (usually a full-screen panel)
// plus its subtree. Saved/loaded as JSON next to the project data
// (ui/*.ui.json). Rendered through the renderer's 2D overlay, so it works in
// the editor viewport and the packed player alike.
class UiDocument {
public:
    UiNode root; // full-design panel

    // Layout strategy (see layout_solver.hpp): "boxflex" (default) or
    // "absolute" (legacy). Anything registered in LayoutSolverRegistry works.
    std::string solver = "boxflex";

    // Loads a .ui.json file. Returns false (and logs) on parse/format errors.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    bool LoadJson(const std::string& text);
    std::string ToJson() const;

    // Renders the tree (top-down, children clipped to parents). `loadTexture`
    // enables sprite-backed image/panel nodes and 9-slice backgrounds. The
    // layout pass runs first with `viewportSize` (design units; width is
    // dynamic - the UI height base stays constant) as the root container, so
    // box-layout nodes adapt to any viewport. Legacy absolute-rect documents
    // are unaffected (their root spans the given viewport).
    void Draw(gfx::Renderer& renderer, const gfx::Font& font,
              const UiTextureLoader& loadTexture = {},
              const math::Vec2& viewportSize = {1280.0f, 720.0f}) const;
    // Deepest visible node containing the point (or nullptr). Runs a layout
    // pass with `viewportSize` first so box-layout nodes hit-test correctly.
    UiNode* HitTest(const math::Vec2& p, const math::Vec2& viewportSize = {1280.0f, 720.0f});
    UiNode* Find(const std::string& name) { return root.Find(name); }

    // Resolves the whole tree against a viewport (design units): fills every
    // node's `resolved` rect (absolute design space). Draw/HitTest call this
    // automatically; the editor preview calls it to show the adapted layout.
    // B12: the result is memoized on (dirty flag + viewport) so a frame that
    // draws AND hit-tests with the same viewport only solves the tree once,
    // and unchanged documents skip layout entirely.
    void Layout(const math::Vec2& viewportSize, const gfx::Font* font) const;
    // Invalidates the memoized layout. Called by load/add/setters; the editor
    // calls it when dragging/resizing nodes.
    void MarkLayoutDirty() const { layoutDirty_ = true; }

private:
    mutable bool layoutDirty_ = true;
    mutable math::Vec2 layoutViewport_;
    static bool SerializeNode(const UiNode& node, core::Json& out);
    static bool ParseNode(const core::Json& in, UiNode& out, std::string& error);
};

} // namespace neon::ui
