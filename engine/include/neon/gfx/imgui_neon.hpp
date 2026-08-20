#pragma once

#include <string>

#include "neon/gfx/backend.hpp"
#include "imgui.h"

namespace neon::platform {
class IInput;
}

namespace neon::gfx {

class Renderer;

// Dear ImGui integration backend for NeonEngine. Renders through
// IRenderBackend (no raw GL calls) and consumes the engine input state, so
// the same code works on every platform backend once one is implemented.
//
// Typical frame:
//   ImGuiNeon_NewFrame(input, pendingText, dt);
//   ImGui::NewFrame();
//   ... build ImGui windows ...
//   ImGui::Render();
//   ImGuiNeon_RenderDrawData(ImGui::GetDrawData());
//
// Only the tool layer (editor) links this module; the runtime game does not
// depend on Dear ImGui.

// Creates the ImGui context, font atlas (optionally with a CJK system font)
// and the internal shader/textures. Call once after Renderer::Init.
bool ImGuiNeon_Init(Renderer* renderer, const char* cjkFontPath);
void ImGuiNeon_Shutdown();

// Feeds mouse/keys/wheel from the engine input state plus buffered UTF-8
// characters (platform TextInput events). Must be called every frame before
// ImGui::NewFrame().
void ImGuiNeon_NewFrame(platform::IInput& input, const std::string& pendingText, float dt);

// Renders ImDrawData with the engine render backend (screen-space, y-down).
void ImGuiNeon_RenderDrawData(ImDrawData* drawData);

// Convenience accessors for routing input between the two UI systems.
bool ImGuiNeon_WantCaptureMouse();
bool ImGuiNeon_WantCaptureKeyboard();

// First existing system CJK font path (Windows/macOS/Linux candidates).
const char* ImGuiNeon_SystemCJKPath();

// Debug helper: the uploaded font atlas texture (for engine-side inspection).
TextureHandle ImGuiNeon_FontTexture();

// Registers an engine texture so it can be referenced by ImGui draw commands
// (e.g. ImGui::Image asset previews). Returns the ImTextureID to pass to
// ImGui; unregister before destroying the texture.
ImTextureID ImGuiNeon_RegisterTexture(TextureHandle texture);
void ImGuiNeon_UnregisterTexture(TextureHandle texture);

} // namespace neon::gfx
