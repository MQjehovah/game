#pragma once

#include <functional>
#include <memory>
#include <string>

#include "neon/gfx/font.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/ui/document.hpp"

namespace neon::ui {

// ---------------------------------------------------------------------------
// IUiSystem: the WHOLE game-facing UI system behind one replaceable seam.
//
// The engine (GameRuntime, script bindings, editor play preview) depends only
// on this interface - never on UiDocument, a specific layout solver or a
// rendering path. Swapping the entire UI stack (new canvas system, a retained
// GPU-batched tree, a third-party UI lib...) means implementing this and
// injecting it through GameRuntimeConfig::uiSystem; scripts, scenes and the
// editor keep working untouched.
//
// Coordinate model: all layout/point math happens in UI DESIGN units whose
// meaning is defined by the host's current 2D mapping; the host passes the
// live viewport size (design units) to Draw/Update so box-layout systems can
// adapt. Clicks are edge-triggered: Update() with clickEdge=true records the
// hit node; Clicked() reports it; the next Update() clears it.
// ---------------------------------------------------------------------------
class IUiSystem {
public:
    virtual ~IUiSystem() = default;

    // Document lifecycle: path is project-relative (ui/*.ui.json for the
    // default system; other systems may interpret it however they like).
    virtual bool Show(const std::string& path) = 0;
    virtual void Hide() = 0;
    virtual bool Active() const = 0;

    // Input: pointer in design units + the frame's left-click edge. Runs the
    // system's hit-testing and refreshes the Clicked() state.
    virtual void Update(const math::Vec2& pointer, bool clickEdge) = 0;
    // Edge-triggered click query for the named clickable node.
    virtual bool Clicked(const std::string& name) const = 0;

    // Script mutation surface (node lookup by name).
    virtual void SetText(const std::string& node, const std::string& text) = 0;
    virtual void SetFill(const std::string& node, float fill) = 0;
    virtual void SetVisible(const std::string& node, bool visible) = 0;
    virtual void SetColor(const std::string& node, float r, float g, float b, float a) = 0;
    // Optional (default no-op): resolve a node's resolved rect for anchors/
    // overlay math. Returns false when unsupported or not found.
    virtual bool NodeRect(const std::string& node, math::Rect2& out) const {
        (void)node; (void)out;
        return false;
    }

    // Renders the UI on top of the composited frame (call AFTER
    // Renderer::EndScene). `viewportSize` is the live design-space viewport.
    virtual void Draw(gfx::Renderer& renderer, const gfx::Font& font,
                      const UiTextureLoader& loadTexture,
                      const math::Vec2& viewportSize) = 0;
};

// Dependencies of the default document-backed system.
struct DocumentUiConfig {
    // path -> JSON text (project dir / VFS). Required.
    std::function<std::string(const std::string&)> readFile;
};

// Default IUiSystem: JSON documents (ui/*.ui.json) rendered through the
// engine 2D overlay, layout via the pluggable solver registry
// ("solver" per document; "boxflex" default / "absolute" legacy).
std::unique_ptr<IUiSystem> CreateDocumentUiSystem(const DocumentUiConfig& cfg);

} // namespace neon::ui
