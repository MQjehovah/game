#include "neon/gfx/imgui_neon.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/gfx/backend.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/mat4.hpp"
#include "neon/platform/input.hpp"

namespace neon::gfx {
namespace {

constexpr uint32_t kImGuiVertexFloats = 8; // x y u v r g b a (matches backend UI VAO)

const char* kImGuiVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)";

const char* kImGuiFragmentShader = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    FragColor = vColor * texture(uTex, vUV);
}
)";

struct ImGuiNeonState {
    Renderer* renderer = nullptr;
    IRenderBackend* backend = nullptr;
    ShaderHandle shader;
    TextureHandle fontTexture;
    TextureHandle whiteTexture;
    std::unordered_map<ImTextureID, TextureHandle> textures;
    int displayW = 0;
    int displayH = 0;
};

ImGuiNeonState gState;

ImGuiKey MapKey(platform::Key key) {
    switch (key) {
        case platform::Key::A: return ImGuiKey_A;
        case platform::Key::B: return ImGuiKey_B;
        case platform::Key::C: return ImGuiKey_C;
        case platform::Key::D: return ImGuiKey_D;
        case platform::Key::E: return ImGuiKey_E;
        case platform::Key::F: return ImGuiKey_F;
        case platform::Key::G: return ImGuiKey_G;
        case platform::Key::H: return ImGuiKey_H;
        case platform::Key::I: return ImGuiKey_I;
        case platform::Key::J: return ImGuiKey_J;
        case platform::Key::K: return ImGuiKey_K;
        case platform::Key::L: return ImGuiKey_L;
        case platform::Key::M: return ImGuiKey_M;
        case platform::Key::N: return ImGuiKey_N;
        case platform::Key::O: return ImGuiKey_O;
        case platform::Key::P: return ImGuiKey_P;
        case platform::Key::Q: return ImGuiKey_Q;
        case platform::Key::R: return ImGuiKey_R;
        case platform::Key::S: return ImGuiKey_S;
        case platform::Key::T: return ImGuiKey_T;
        case platform::Key::U: return ImGuiKey_U;
        case platform::Key::V: return ImGuiKey_V;
        case platform::Key::W: return ImGuiKey_W;
        case platform::Key::X: return ImGuiKey_X;
        case platform::Key::Y: return ImGuiKey_Y;
        case platform::Key::Z: return ImGuiKey_Z;
        case platform::Key::D0: return ImGuiKey_0;
        case platform::Key::D1: return ImGuiKey_1;
        case platform::Key::D2: return ImGuiKey_2;
        case platform::Key::D3: return ImGuiKey_3;
        case platform::Key::D4: return ImGuiKey_4;
        case platform::Key::D5: return ImGuiKey_5;
        case platform::Key::D6: return ImGuiKey_6;
        case platform::Key::D7: return ImGuiKey_7;
        case platform::Key::D8: return ImGuiKey_8;
        case platform::Key::D9: return ImGuiKey_9;
        case platform::Key::Space: return ImGuiKey_Space;
        case platform::Key::Enter: return ImGuiKey_Enter;
        case platform::Key::Escape: return ImGuiKey_Escape;
        case platform::Key::Tab: return ImGuiKey_Tab;
        case platform::Key::Backspace: return ImGuiKey_Backspace;
        case platform::Key::Shift: return ImGuiKey_LeftShift;
        case platform::Key::Control: return ImGuiKey_LeftCtrl;
        case platform::Key::Alt: return ImGuiKey_LeftAlt;
        case platform::Key::ArrowUp: return ImGuiKey_UpArrow;
        case platform::Key::ArrowDown: return ImGuiKey_DownArrow;
        case platform::Key::ArrowLeft: return ImGuiKey_LeftArrow;
        case platform::Key::ArrowRight: return ImGuiKey_RightArrow;
        case platform::Key::F1: return ImGuiKey_F1;
        case platform::Key::F2: return ImGuiKey_F2;
        case platform::Key::F3: return ImGuiKey_F3;
        case platform::Key::F4: return ImGuiKey_F4;
        case platform::Key::F5: return ImGuiKey_F5;
        case platform::Key::F6: return ImGuiKey_F6;
        case platform::Key::F7: return ImGuiKey_F7;
        case platform::Key::F8: return ImGuiKey_F8;
        case platform::Key::F9: return ImGuiKey_F9;
        case platform::Key::F10: return ImGuiKey_F10;
        case platform::Key::F11: return ImGuiKey_F11;
        case platform::Key::F12: return ImGuiKey_F12;
        default: return ImGuiKey_None;
    }
}

} // namespace

const char* ImGuiNeon_SystemCJKPath() {
#if defined(_WIN32)
    static const char* kCandidates[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        nullptr};
#elif defined(__APPLE__)
    static const char* kCandidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        nullptr};
#else
    static const char* kCandidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        nullptr};
#endif
    for (int i = 0; kCandidates[i]; ++i) {
        std::FILE* f = std::fopen(kCandidates[i], "rb");
        if (f) {
            std::fclose(f);
            return kCandidates[i];
        }
    }
    return nullptr;
}

bool ImGuiNeon_Init(Renderer* renderer, const char* cjkFontPath) {
    if (!renderer) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "neon_editor_imgui.ini";

    // Fonts: default Latin + (optionally) full CJK coverage from a system font.
    if (cjkFontPath) {
        static const ImWchar kCJKRanges[] = {
            0x20, 0x7E,     // Basic Latin
            0x00A0, 0x00FF, // Latin-1
            0x2000, 0x206F, // General punctuation
            0x3000, 0x303F, // CJK punctuation
            0x4E00, 0x9FFF, // CJK Unified Ideographs
            0xFF00, 0xFFEF, // Fullwidth forms
            0,
        };
        ImFontConfig cfg;
        cfg.FontNo = 0; // first face of a .ttc collection
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        if (!io.Fonts->AddFontFromFileTTF(cjkFontPath, 18.0f, &cfg, kCJKRanges)) {
            NEON_LOG_WARN("ImGui: failed to load CJK font '%s'", cjkFontPath);
        } else {
            NEON_LOG_INFO("ImGui: CJK font '%s'", cjkFontPath);
        }
    }
    // Legacy atlas path: bake the full glyph range up-front. This avoids the
    // 1.92+ dynamic-atlas (WantUpdates) flow, which is still settling in WIP
    // builds and interacts poorly with custom renderers. A 4096x4096 CJK
    // atlas costs ~1-2s at startup, which is fine for a desktop editor.
    unsigned char* pixels = nullptr;
    int atlasW = 0, atlasH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasW, &atlasH);

    gState.renderer = renderer;
    gState.backend = renderer->Backend();
    gState.displayW = renderer->ScreenWidth();
    gState.displayH = renderer->ScreenHeight();

    gState.shader = gState.backend->CreateShader(kImGuiVertexShader, kImGuiFragmentShader, "imgui");
    if (!gState.shader.Valid()) {
        NEON_LOG_ERROR("ImGui: shader creation failed");
        return false;
    }

    // 1x1 white texture for untextured commands.
    const uint8_t white[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.rgba = white;
    whiteDesc.filter = Filter::Nearest;
    gState.whiteTexture = gState.backend->CreateTexture(whiteDesc);
    gState.textures[static_cast<ImTextureID>(gState.whiteTexture.id)] = gState.whiteTexture;

    TextureDesc atlasDesc;
    atlasDesc.width = atlasW;
    atlasDesc.height = atlasH;
    atlasDesc.rgba = pixels;
    atlasDesc.filter = Filter::Linear;
    gState.fontTexture = gState.backend->CreateTexture(atlasDesc);
    if (!gState.fontTexture.Valid()) {
        NEON_LOG_ERROR("ImGui: font atlas texture creation failed");
        return false;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(gState.fontTexture.id));
    gState.textures[static_cast<ImTextureID>(gState.fontTexture.id)] = gState.fontTexture;
    io.Fonts->ClearTexData();

    NEON_LOG_INFO("ImGui: backend ready (atlas %dx%d, %d fonts)", atlasW, atlasH,
                  io.Fonts->Fonts.Size);
    return true;
}

void ImGuiNeon_Shutdown() {
    if (!gState.backend) return;
    if (gState.shader.Valid()) gState.backend->DestroyShader(gState.shader);
    if (gState.whiteTexture.Valid()) gState.backend->DestroyTexture(gState.whiteTexture);
    if (gState.fontTexture.Valid()) gState.backend->DestroyTexture(gState.fontTexture);
    for (auto& kv : gState.textures) {
        if (kv.second.Valid() && kv.second.id != gState.whiteTexture.id) {
            gState.backend->DestroyTexture(kv.second);
        }
    }
    gState.textures.clear();
    gState = ImGuiNeonState{};
    ImGui::DestroyContext();
}

void ImGuiNeon_NewFrame(platform::IInput& input, const std::string& pendingText, float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (!gState.renderer) return;

    gState.displayW = gState.renderer->ScreenWidth();
    gState.displayH = gState.renderer->ScreenHeight();
    io.DisplaySize = ImVec2(static_cast<float>(gState.displayW),
                            static_cast<float>(gState.displayH));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    // 1.92+ moved the global font scale from io to style.
    ImGui::GetStyle().FontScaleMain = gState.renderer->UIScale();
    io.DeltaTime = std::max(1.0f / 120.0f, std::min(dt, 1.0f / 20.0f));

    // Mouse.
    math::Vec2 mp = input.MousePos();
    io.AddMousePosEvent(mp.x, mp.y);
    io.AddMouseButtonEvent(0, input.MouseDown(platform::MouseButton::Left));
    io.AddMouseButtonEvent(1, input.MouseDown(platform::MouseButton::Right));
    io.AddMouseButtonEvent(2, input.MouseDown(platform::MouseButton::Middle));
    float wheel = input.WheelDelta();
    if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);

    // Keyboard edges (engine state machine tracks pressed/released per frame).
    for (int k = static_cast<int>(platform::Key::A); k <= static_cast<int>(platform::Key::F12);
         ++k) {
        platform::Key key = static_cast<platform::Key>(k);
        ImGuiKey ik = MapKey(key);
        if (ik == ImGuiKey_None) continue;
        if (input.Pressed(key)) io.AddKeyEvent(ik, true);
        if (input.Released(key)) io.AddKeyEvent(ik, false);
    }

    if (!pendingText.empty()) io.AddInputCharactersUTF8(pendingText.c_str());
}

bool ImGuiNeon_WantCaptureMouse() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiNeon_WantCaptureKeyboard() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

TextureHandle ImGuiNeon_FontTexture() {
    return gState.fontTexture;
}

ImTextureID ImGuiNeon_RegisterTexture(TextureHandle texture) {
    if (!texture.Valid() || !gState.backend) return ImTextureID_Invalid;
    ImTextureID id = static_cast<ImTextureID>(texture.id);
    gState.textures[id] = texture;
    return id;
}

void ImGuiNeon_UnregisterTexture(TextureHandle texture) {
    if (!texture.Valid()) return;
    gState.textures.erase(static_cast<ImTextureID>(texture.id));
}

void ImGuiNeon_RenderDrawData(ImDrawData* drawData) {
    if (!drawData || drawData->CmdListsCount == 0 || !gState.backend) return;

    IRenderBackend* backend = gState.backend;
    const int w = gState.displayW;
    const int h = gState.displayH;
    if (w <= 0 || h <= 0) return;

    backend->SetViewport(w, h);
    // Our shader outputs straight (non-premultiplied) color = vColor * tex,
    // and the font atlas stores RGB=1 with coverage in alpha, so standard
    // alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) is correct. Using
    // premultiplied blending here would make every glyph quad solid white.
    backend->SetBlendMode(BlendMode::Alpha);
    backend->SetDepthTest(false, false);
    backend->SetCullMode(CullMode::None);
    backend->UseShader(gState.shader);
    backend->SetUniformMat4("uMVP",
                            math::Mat4::Ortho(0.0f, static_cast<float>(w),
                                              static_cast<float>(h), 0.0f, -1.0f, 1.0f));

    std::vector<float> verts;
    std::vector<uint16_t> indices;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList* list = drawData->CmdLists[listIndex];
        verts.resize(static_cast<size_t>(list->VtxBuffer.Size) * kImGuiVertexFloats);
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = list->VtxBuffer[i];
            float* out = verts.data() + static_cast<size_t>(i) * kImGuiVertexFloats;
            out[0] = v.pos.x;
            out[1] = v.pos.y;
            out[2] = v.uv.x;
            out[3] = v.uv.y;
            out[4] = static_cast<float>((v.col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
            out[5] = static_cast<float>((v.col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
            out[6] = static_cast<float>((v.col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
            out[7] = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        }

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                cmd.UserCallback(list, &cmd);
                continue;
            }
            // Clip rect is in display coordinates (y-down); clamp to the window.
            float clipX = std::max(0.0f, cmd.ClipRect.x);
            float clipY = std::max(0.0f, cmd.ClipRect.y);
            float clipX2 = std::min(static_cast<float>(w), cmd.ClipRect.z);
            float clipY2 = std::min(static_cast<float>(h), cmd.ClipRect.w);
            if (clipX2 <= clipX || clipY2 <= clipY) continue;
            backend->SetScissor(static_cast<int>(clipX), static_cast<int>(clipY),
                                static_cast<int>(clipX2 - clipX),
                                static_cast<int>(clipY2 - clipY), true);

            auto it = gState.textures.find(cmd.GetTexID());
            TextureHandle tex = it != gState.textures.end() ? it->second : gState.whiteTexture;
            backend->BindTexture(0, tex.Valid() ? tex : gState.whiteTexture);

            // 1.93 removed ImDrawCmd::VtxCount. This backend does not set
            // ImGuiBackendFlags_RendererHasVtxOffset, so every ImDrawCmd in a
            // list has VtxOffset == 0 and all commands draw from the list's own
            // vertex buffer; the drawable vertex range is the whole buffer, not
            // the gap to the next command (that "next command" heuristic broke
            // multi-command lists into zero-size vertex ranges, crashing the
            // GL driver with a 0-byte VBO + live indices).
            uint32_t vtxCount =
                static_cast<uint32_t>(list->VtxBuffer.Size) - cmd.VtxOffset;
            indices.resize(static_cast<size_t>(cmd.ElemCount));
            const ImDrawIdx* src = list->IdxBuffer.Data + cmd.IdxOffset;
            for (int i = 0; i < static_cast<int>(cmd.ElemCount); ++i) {
                indices[static_cast<size_t>(i)] =
                    static_cast<uint16_t>(src[i] - static_cast<ImDrawIdx>(cmd.VtxOffset));
            }
            backend->DrawPrimitives(
                verts.data() + static_cast<size_t>(cmd.VtxOffset) * kImGuiVertexFloats,
                vtxCount, kImGuiVertexFloats * 4, indices.data(),
                static_cast<uint32_t>(cmd.ElemCount), PrimitiveTopology::Triangles);
        }
    }
    backend->SetScissor(0, 0, 0, 0, false);
    backend->SetBlendMode(BlendMode::Opaque);
}

} // namespace neon::gfx
