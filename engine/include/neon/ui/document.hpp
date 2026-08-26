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
    Panel, // background rectangle (+ optional border)
    Label, // text
    Button, // clickable rectangle with centered text
    Bar,   // progress/fill bar
    Image, // textured quad (sprite path)
};

const char* UiNodeTypeName(UiNodeType type);
UiNodeType UiNodeTypeFromName(const std::string& name);

// A node in a UI document tree. Rects are ABSOLUTE in the design space
// (1280x720, top-left origin); children are clipped to their parent's rect.
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

    UiNode* AddChild(UiNodeType type, const std::string& childName);
    UiNode* Find(const std::string& target);
    // Absolute design-space rect (walks the parent chain).
    math::Rect2 AbsoluteRect() const {
        math::Rect2 r = rect;
        for (const UiNode* p = parent; p; p = p->parent) {
            r.x += p->rect.x;
            r.y += p->rect.y;
        }
        return r;
    }
    bool Contains(const math::Vec2& p) const {
        return visible && AbsoluteRect().Contains(p);
    }
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

    // Loads a .ui.json file. Returns false (and logs) on parse/format errors.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    bool LoadJson(const std::string& text);
    std::string ToJson() const;

    // Renders the tree (top-down, children clipped to parents). `loadTexture`
    // enables sprite-backed image/panel nodes and 9-slice backgrounds.
    void Draw(gfx::Renderer& renderer, const gfx::Font& font,
              const UiTextureLoader& loadTexture = {}) const;
    // Deepest visible node containing the point (or nullptr).
    UiNode* HitTest(const math::Vec2& p);
    UiNode* Find(const std::string& name) { return root.Find(name); }

private:
    static bool SerializeNode(const UiNode& node, core::Json& out);
    static bool ParseNode(const core::Json& in, UiNode& out, std::string& error);
};

} // namespace neon::ui
