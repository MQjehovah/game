#include "editor.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "editor_history.hpp"
#include "font_data.hpp"

#include "imgui_internal.h"
#include "neon/core/json.hpp"
#include "neon/gfx/imgui_neon.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace neon::editor {
namespace {

std::string DirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos + 1);
}

// Map an editor mesh key to the runtime-loadable key written into an exported
// scene. File-backed built-ins resolve to their asset paths; procedural
// primitives ("terrain", "cube") and already-prefixed keys ("obj:", "gltf:")
// pass through verbatim.
std::string ExportMeshKey(const std::string& key) {
    if (key == "helmet") return "gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf";
    if (key == "tree") return "obj:assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj";
    return key;
}

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

// Create every missing path component (accepts '/' and '\' separators).
bool EnsureDirs(const std::string& path) {
    std::string acc;
    size_t i = 0;
    while (i < path.size()) {
        size_t next = path.find_first_of("/\\", i);
        if (next == std::string::npos) next = path.size();
        std::string comp = path.substr(i, next - i);
        i = next + 1;
        if (!acc.empty() && !comp.empty()) {
            acc += "/" + comp;
        } else if (acc.empty()) {
            acc = comp;
        } else {
            continue; // duplicate separator; keep going
        }
        if (acc.empty() || acc == ".") continue;
        // A Windows drive root like "C:" already exists; skip creation.
        if (acc.size() == 2 && acc[1] == ':') continue;
        if (!MakeDir(acc)) return false;
    }
    return true;
}

std::string GetTempDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string dir(buf);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir.empty() ? "." : dir;
#else
    const char* t = std::getenv("TMPDIR");
    if (t && *t) return t;
    return "/tmp";
#endif
}

gfx::Mesh MakeTerrain(gfx::Renderer& renderer) {
    const int segments = 48;
    const float size = 60.0f;
    std::vector<float> heights(static_cast<size_t>(segments + 1) * (segments + 1), 0.0f);
    float half = size * 0.5f;
    for (int row = 0; row <= segments; ++row) {
        for (int col = 0; col <= segments; ++col) {
            float x = -half + col * (size / segments);
            float z = -half + row * (size / segments);
            float h = std::sin(x * 0.11f) * std::cos(z * 0.13f) * 0.8f +
                      std::sin(x * 0.31f + z * 0.27f) * 0.35f;
            float d = std::sqrt(x * x + z * z);
            h *= math::Saturate((d - 6.0f) / 10.0f); // flatten the centre
            heights[static_cast<size_t>(row) * (segments + 1) + col] = h;
        }
    }
    return gfx::Mesh::CreateTerrain(renderer, segments, size, heights, 1.0f, "terrain");
}

math::Ray ScreenRay(const gfx::Camera& cam, float aspect, const math::Vec2& designPos) {
    float ndcX = designPos.x / static_cast<float>(gfx::Renderer::kDesignWidth) * 2.0f - 1.0f;
    float ndcY = 1.0f - designPos.y / static_cast<float>(gfx::Renderer::kDesignHeight) * 2.0f;
    math::Vec3 fwd = (cam.target - cam.position).Normalized();
    math::Vec3 right = math::Cross(fwd, cam.up).Normalized();
    math::Vec3 upv = math::Cross(right, fwd);
    float tanF = std::tan(cam.fovY * 0.5f);
    math::Vec3 dir = (fwd + right * ndcX * tanF * aspect + upv * ndcY * tanF).Normalized();
    return {cam.position, dir};
}

// ---------------------------------------------------------------------------
// ImGuizmo matrix boundary.
//
// The engine's math::Mat4 is row-major storage: element (row, col) lives at
// m[row * 4 + col] and translation at m[3]/m[7]/m[11]. ImGuizmo expects the
// classic column-major float[16] layout used by OpenGL/glm (right/up/forward
// basis in columns 0/1/2, translation at m[12]/m[13]/m[14]; see ImGuizmo.cpp's
// matrix_t where v.right = m16[0..3], v.position = m16[12..15]). Converting
// is therefore a transpose: element (r, c) -> gizmo index c*4 + r.
void Mat4ToGizmo(const math::Mat4& m, float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = m.m[r * 4 + c];
    }
}

void GizmoToMat4(const float in[16], math::Mat4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m.m[r * 4 + c] = in[c * 4 + r];
    }
}

// Rebuild a SceneEntity's TRS from a decomposed row-major matrix. The engine
// composes model matrices as T*R*S (column-vector convention: v' = M*v), so
// scale is carried by the COLUMNS of the 3x3 block: column j = scale_j * R_j.
// Translation is m[3]/m[7]/m[11]. This is the inverse of
// Translation(pos) * rot.ToMat4() * Scale(scale).
void DecomposeModel(const math::Mat4& m, math::Vec3& pos, math::Vec3& scale, math::Quat& rot) {
    pos = {m.m[3], m.m[7], m.m[11]};
    math::Vec3 col0{m.m[0], m.m[4], m.m[8]};
    math::Vec3 col1{m.m[1], m.m[5], m.m[9]};
    math::Vec3 col2{m.m[2], m.m[6], m.m[10]};
    scale = {col0.Length(), col1.Length(), col2.Length()};
    math::Vec3 r0 = col0.Normalized();
    math::Vec3 r1 = col1.Normalized();
    math::Vec3 r2 = col2.Normalized();
    // A mirror (det < 0, e.g. a negative scale axis) must be folded into the
    // scale so the extracted rotation stays proper (det +1) and recomposing
    // T*R*S reproduces the source matrix exactly.
    if (math::Dot(r0, math::Cross(r1, r2)) < 0.0f) {
        r0 = -r0;
        scale.x = -scale.x;
    }
    // Feed Mat4ToQuat a pure rotation matrix built from the normalized columns;
    // Mat4ToQuat's row-based Shepperd extraction is exact on orthonormal rows.
    math::Mat4 rotM;
    rotM.m[0] = r0.x;  rotM.m[4] = r0.y;  rotM.m[8] = r0.z;
    rotM.m[1] = r1.x;  rotM.m[5] = r1.y;  rotM.m[9] = r1.z;
    rotM.m[2] = r2.x;  rotM.m[6] = r2.y;  rotM.m[10] = r2.z;
    rot = math::Mat4ToQuat(rotM);
}

} // namespace

bool EditorApp::OnCreate() {
    if (disableShadows_) renderer_.SetShadowsEnabled(false);
    renderer_.SetBloomEnabled(bloomEnabled_);
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Editor: renderer init failed");
        return false;
    }
    assetMgr_.Init(&renderer_);

    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
    const std::vector<std::string> cjkSamples = {
        "霓虹编辑器场景层级属性名称位置旋转缩放颜色保存加载添加删除播放停止网格头盔立方体树木地面",
        "工具栏视口实体选择变换材质渲染确定取消打开文件写入成功失败无法松树",
        "块测试引擎演示场景树表单滚动列表地形添加节点自研控件树拖动分隔条右侧面板第行",
        "文件视图退出图层属性菜单帮助关于版本相机旋转中键平移滚轮拾取"};
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24, cjkSamples);
    ui_.Init(&renderer_, cjkFont_.Valid() ? cjkFont_ : pixelFont_);

    if (!gfx::ImGuiNeon_Init(&renderer_, gfx::ImGuiNeon_SystemCJKPath())) {
        NEON_LOG_ERROR("Editor: Dear ImGui init failed");
        return false;
    }

    SetupScene();
    BuildCustomUIDemo();
    InitToolPanels();

    LoadEditorConfig();
    NEON_LOG_INFO("NeonEditor ready (%zu entities), project dir '%s'", entities_.size(),
                  projectDir_.c_str());
    return true;
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    gfx::ImGuiNeon_Shutdown();
    renderer_.Shutdown();
}

void EditorApp::SetupScene() {
    auto add = [&](const std::string& key, const std::string& name, const math::Vec3& pos,
                   const math::Vec3& scale, const gfx::Color& tint) {
        SceneEntity e;
        e.name = name;
        e.meshKey = key;
        e.pos = pos;
        e.scale = scale;
        e.tint = tint;
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            entities_.push_back(std::move(e));
        }
    };

    add("terrain", "地面", {0, 0, 0}, {1, 1, 1}, gfx::Color::White);
    add("helmet", "头盔", {3, 0.9f, -2}, {1, 1, 1}, gfx::Color::White);
    add("cube", "测试方块", {-3, 0.6f, 1}, {1, 1, 1}, gfx::Color{0.9f, 0.4f, 0.2f, 1.0f});
    add("tree", "松树A", {-5, 0, -3}, {1.6f, 1.6f, 1.6f}, gfx::Color::White);
    add("tree", "松树B", {5, 0, 4}, {1.2f, 1.2f, 1.2f}, gfx::Color::White);
    LoadScene("editor_scene.json");
    selected_ = entities_.empty() ? -1 : 0;
}

void EditorApp::OnUpdate(float dt) {
    // The gizmo drag-sim (frame 30) needs the real mouse to hover the viewport
    // so ImGui's hover hit-test yields the dock host window (the window
    // SetAlternativeWindow points at); headless starts at (0,0) over the menu
    // bar, so park it on the viewport center for the smoke frame.
    if (smokeMode_ && TimeRef().frameIndex == 30) {
        platform::InputEvent e;
        e.type = platform::InputEvent::Type::MouseMove;
        e.x = renderer_.ScreenWidth() / 2;
        e.y = renderer_.ScreenHeight() / 2;
        Input()->HandleEvent(e);
    }
    UpdateViewport(dt);
    if (playtestActive_ && playtest_) playtest_->Tick(dt);
    gfx::ImGuiNeon_NewFrame(*Input(), pendingText_, dt);
    pendingText_.clear();
    ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();

    // The custom UI demo only processes input when ImGui does not capture it.
    if (!ImGui::GetIO().WantCaptureMouse) ui_.Update(*Input());

    // Open every tool panel right before the UI smoke test.
    if (smokeMode_ && TimeRef().frameIndex == 29) {
        showCustomUIDemo_ = true;
        showHierarchy_ = true;
        showInspector_ = true;
        showAssets_ = true;
        showResources_ = true;
        showLog_ = true;
        showBt_ = true;
        // Seed a small tree so the BT canvas renders real nodes on the smoke
        // frame (frame 30) and the smoke can assert the canvas drew geometry.
        btGraph_ = btgraph::BtGraph{};
        const std::string r = btGraph_.AddNode("sequence", math::Vec2{20.f, 20.f});
        const std::string c = btGraph_.AddNode("in_range", math::Vec2{20.f, 180.f});
        const std::string a = btGraph_.AddNode("move_to", math::Vec2{240.f, 180.f});
        core::Json dist;
        dist.type_ = core::Json::Type::Number;
        dist.number_ = 8.0;
        core::Json speed;
        speed.type_ = core::Json::Type::Number;
        speed.number_ = 3.0;
        btGraph_.SetArg(c, "distance", dist);
        btGraph_.SetArg(a, "speed", speed);
        btGraph_.SetParent(c, r);
        btGraph_.SetParent(a, r);
        btSelected_ = r;
    }
    if (smokeMode_ && TimeRef().frameIndex == 30) RunUISmokeTest();

    // Play/Stop smoke: start a playtest at frame 60, verify it ticks, stop at
    // the last frame (119; OnUpdate never runs at 120). Kept at "Play/Stop
    // doesn't crash the editor" level; the real script/BT verification lives
    // in tests/test_game_runtime.cpp.
    if (smokeMode_ && TimeRef().frameIndex == 60) StartPlaytest();
    if (smokeMode_ && TimeRef().frameIndex == 90) {
        const bool ok = playtestActive_ && playtest_ && playtest_->Running();
        NEON_LOG_INFO("EDITOR-PLAYTEST-SMOKE: [%s] playtest active (entities=%zu)",
                      ok ? "PASS" : "FAIL", ok ? playtest_->EntityCount() : 0u);
        if (!ok) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 119) { // last OnUpdate before exit
        const bool wasActive = playtestActive_ && playtest_;
        StopPlaytest();
        const bool clean = !playtest_ && !playtestActive_;
        NEON_LOG_INFO("EDITOR-PLAYTEST-SMOKE: [%s] playtest stopped cleanly (was %s)",
                      clean ? "PASS" : "FAIL", wasActive ? "active" : "inactive");
        if (!wasActive || !clean) smokeFailed_ = true;
    }
}

void EditorApp::OnRender() {
    renderer_.BeginFrame({0.06f, 0.08f, 0.13f, 1.0f});
    renderer_.SetSky({0.05f, 0.08f, 0.16f, 1.0f}, {0.35f, 0.45f, 0.6f, 1.0f});
    renderer_.SetFog({0.3f, 0.38f, 0.5f, 1.0f}, 60.0f, 140.0f);
    renderer_.DrawSky();

    float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
    gfx::Camera cam;
    cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                           std::sin(pitch_),
                                           std::cos(yaw_) * std::cos(pitch_)} *
                                        camDist_;
    cam.target = camTarget_;
    renderer_.SetCamera(cam, aspect);
    renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.3f);

    if (playtestActive_ && playtest_) {
        // Play mode: the viewport renders the runtime's world (a snapshot of
        // the scene taken at Play). The editor scene is untouched.
        playtest_->Draw(renderer_, cam);
    } else {
        for (const SceneEntity& e : entities_) {
            math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                               math::Mat4::Scale(e.scale);
            renderer_.DrawMesh(e.mesh, e.material, model);
        }
        static bool dbg = false;
        if (!dbg && smokeMode_) {
            dbg = true;
            NEON_LOG_INFO("EDITOR-DRAW: entities=%zu drawCalls=%u", entities_.size(),
                          renderer_.Stats().drawCalls);
        }
        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
            math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                               math::Mat4::Scale(e.scale);
            math::AABB world = math::TransformAABB(e.mesh.Bounds(), model);
            renderer_.DrawBox(world, gfx::Color{0.3f, 0.8f, 1.0f, 1.0f});
        }
    }

    // End the 3D scene phase: composite the HDR frame to the backbuffer and
    // bind the backbuffer so the tool UI (engine UI demo + ImGui) below renders
    // crisp and unbloomed on top.
    renderer_.EndScene();

    if (showCustomUIDemo_) ui_.Draw(renderer_);
    renderer_.Flush2D();
    gfx::ImGuiNeon_RenderDrawData(ImGui::GetDrawData());

    if (!screenshotPath_.empty() && TimeRef().frameIndex >= screenshotFrame_) {
        std::vector<uint8_t> pixels;
        if (renderer_.CaptureFrame(pixels)) {
            stbi_write_png(screenshotPath_.c_str(), renderer_.ScreenWidth(),
                           renderer_.ScreenHeight(), 4, pixels.data(),
                           renderer_.ScreenWidth() * 4);
            NEON_LOG_INFO("Editor screenshot: %s", screenshotPath_.c_str());
        }
        screenshotPath_.clear();
    }
    renderer_.EndFrame();
}

void EditorApp::OnEvent(const platform::InputEvent& event) {
    // Ctrl+Z (undo) / Ctrl+Y or Ctrl+Shift+Z (redo) on the KeyDown edge only,
    // and never while ImGui owns the keyboard (e.g. typing in the name field)
    // -- same gating as the F5 playtest shortcut below. When the 行为树 panel
    // has focus AND its graph history has steps, undo/redo drive the BT graph;
    // otherwise they drive the scene history (an empty BT history never
    // swallows the scene shortcuts).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (Input()->IsDown(platform::Key::Control)) {
            if (event.key == platform::Key::Z) {
                if (Input()->IsDown(platform::Key::Shift)) {
                    if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                    else history_.Redo();
                } else {
                    if (btPanelFocused_ && btHistory_.CanUndo()) btHistory_.Undo();
                    else history_.Undo();
                }
                ClampSelection();
                return;
            }
            if (event.key == platform::Key::Y) {
                if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                else history_.Redo();
                ClampSelection();
                return;
            }
        }
    }
    // F5 toggles playtest on the KeyDown edge only (Win32 auto-repeats KeyDown
    // while held, which would otherwise oscillate Play/Stop), and never while
    // ImGui owns the keyboard (e.g. typing in a text field).
    if (event.key == platform::Key::F5) {
        if (event.type == platform::InputEvent::Type::KeyDown) {
            if (!f5Pressed_ && !gfx::ImGuiNeon_WantCaptureKeyboard()) TogglePlaytest();
            f5Pressed_ = true;
        } else if (event.type == platform::InputEvent::Type::KeyUp) {
            f5Pressed_ = false;
        }
    }
    if (event.type == platform::InputEvent::Type::TextInput) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.TextInput(event.text);
        pendingText_ += event.text;
    } else if (event.type == platform::InputEvent::Type::KeyDown) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.Key(event.key, true);
    } else if (event.type == platform::InputEvent::Type::KeyUp) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.Key(event.key, false);
    }
}

gfx::Camera EditorApp::OrbitCamera() const {
    gfx::Camera cam;
    cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                           std::sin(pitch_),
                                           std::cos(yaw_) * std::cos(pitch_)} *
                                    camDist_;
    cam.target = camTarget_;
    return cam;
}

void EditorApp::UpdateViewport(float dt) {
    platform::IInput* input = Input();
    math::Vec2 mp = renderer_.ScreenToUI(input->MousePos());
    // ImGui tool windows capture mouse when hovered/active; the 3D viewport
    // area itself has no ImGui window, so camera controls stay responsive.
    bool overPanel = ImGui::GetIO().WantCaptureMouse;
    bool inViewport = mp.x >= viewportRect_.x && mp.x <= viewportRect_.x + viewportRect_.w &&
                      mp.y >= viewportRect_.y && mp.y <= viewportRect_.y + viewportRect_.h;

    if (!overPanel && inViewport) {
        // While the transform gizmo is hovered or being dragged the mouse
        // belongs to it: camera orbit/pan and left-click picking must not run.
        // UpdateViewport runs before the gizmo's Manipulate() each frame, so
        // gizmoDragActive_/IsOver() report the previous frame's gizmo state.
        const bool gizmoBusy =
            selected_ >= 0 && (gizmoDragActive_ || ImGuizmo::IsOver());
        if (!gizmoBusy) {
            if (input->MouseDown(platform::MouseButton::Right)) {
                yaw_ += -input->MouseDelta().x * 0.005f;
                pitch_ = math::Clamp(pitch_ + -input->MouseDelta().y * 0.005f, 0.05f, 1.4f);
            }
            if (input->MouseDown(platform::MouseButton::Middle)) {
                math::Vec3 fwd = (camTarget_ + math::Vec3{0, 0, 0} -
                                  (camTarget_ +
                                   math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                              std::sin(pitch_),
                                              std::cos(yaw_) * std::cos(pitch_)} *
                                       camDist_))
                                     .Normalized();
                math::Vec3 right = math::Cross(fwd, {0, 1, 0}).Normalized();
                math::Vec3 upv = math::Cross(right, fwd);
                camTarget_ -= right * input->MouseDelta().x * 0.02f;
                camTarget_ += upv * input->MouseDelta().y * 0.02f;
            }
            float wheel = input->WheelDelta();
            if (std::fabs(wheel) > 0.01f)
                camDist_ = math::Clamp(camDist_ - wheel * 1.2f, 3.0f, 60.0f);
        }
        // Play mode keeps camera navigation but not scene editing: left-click
        // picking would mutate the editor scene selection mid-playtest.
        if (input->MousePressed(platform::MouseButton::Left) && !playtestActive_ &&
            !gizmoBusy) {
            float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
            gfx::Camera cam;
            cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                                   std::sin(pitch_),
                                                   std::cos(yaw_) * std::cos(pitch_)} *
                                            camDist_;
            cam.target = camTarget_;
            math::Ray ray = ScreenRay(cam, aspect, mp);
            float best = 1e30f;
            int picked = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& e = entities_[i];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                math::AABB world = math::TransformAABB(e.mesh.Bounds(), model);
                float t = 0.0f;
                if (math::IntersectRayAABB(ray, world, t) && t < best) {
                    best = t;
                    picked = static_cast<int>(i);
                }
            }
            selected_ = picked;
        }
    }
    (void)dt;
}

void EditorApp::DrawTransformGizmo() {
    // ImGuizmo::BeginFrame() must run every frame before Manipulate(): it
    // resets mbOverGizmoHotspot (ImGuizmo.cpp:1084) so a handle can re-arm for
    // hover/activation each frame, and snapshots the last frame's hover for
    // IsOver(). Without it the activation check `CanActivate() && type !=
    // MT_NONE` can never fire again after the first hover.
    ImGuizmo::BeginFrame();
    gizmoBeginFrame_ = true;

    if (playtestActive_ || selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
        gizmoDragActive_ = false;
        return;
    }
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];

    // Draw the gizmo into the viewport window's draw list. Over a docked window
    // the mouse is treated as hovering the DOCK HOST, not the 视口 window (the
    // viewport is NoInputs, so ImGui's hover hit-test skips it and g.HoveredWindow
    // becomes the host). For a docked leaf window ParentWindow IS the host
    // (imgui.cpp:8009), so point ImGuizmo's hover check at it via
    // SetAlternativeWindow; otherwise GetMoveType/GetRotateType/GetScaleType
    // all bail on `!mbMouseOver` and the gizmo can never be grabbed.
    ImGuiWindow* viewportWindow = ImGui::GetCurrentWindow();
    ImGuiWindow* hoverWindow =
        (viewportWindow && viewportWindow->ParentWindow) ? viewportWindow->ParentWindow
                                                         : viewportWindow;
    ImGuizmo::SetAlternativeWindow(hoverWindow);
    gizmoAltWindowSet_ = hoverWindow != nullptr;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

    // The 3D scene renders FULL-WINDOW (OnRender sets the camera with the full
    // screen aspect; the viewport window is an overlay), so the gizmo rect must
    // be the full window: this makes the gizmo sit on the entity exactly where
    // the renderer drew it and where the picker looks. The viewport window's
    // draw list still clips the gizmo to the visible viewport region.
    const float rx = 0.0f;
    const float ry = 0.0f;
    const float rw = static_cast<float>(renderer_.ScreenWidth());
    const float rh = static_cast<float>(renderer_.ScreenHeight());
    ImGuizmo::SetRect(rx, ry, rw, rh);
    gizmoRect_[0] = rx;
    gizmoRect_[1] = ry;
    gizmoRect_[2] = rw;
    gizmoRect_[3] = rh;

    float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
    gfx::Camera cam = OrbitCamera();
    float view[16], proj[16];
    Mat4ToGizmo(cam.View(), view);
    Mat4ToGizmo(cam.Projection(aspect), proj);

    math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                       math::Mat4::Scale(e.scale);
    float gizmoModel[16];
    Mat4ToGizmo(model, gizmoModel);

    // Smoke instrumentation: the gizmo must emit geometry into the viewport's
    // draw list (not just run without crashing).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int cmdsBefore = dl->CmdBuffer.Size;
    const int vtxBefore = dl->VtxBuffer.Size;

    // Manipulate reads ImGui's mouse state directly and mutates gizmoModel on
    // drag; when it returns true the transform changed. The write-back is
    // routed through the history command stack: each changed frame pushes an
    // EditTransformCommand whose ORIGINAL is the last applied value (a
    // continuous merge chain), so frames within one drag collapse into a
    // single undo step that reverts to the pre-drag transform.
    if (ImGuizmo::Manipulate(view, proj, gizmoOp_, gizmoMode_, gizmoModel)) {
        math::Mat4 m;
        GizmoToMat4(gizmoModel, m);
        math::Vec3 pos, scale;
        math::Quat rot;
        DecomposeModel(m, pos, scale, rot);
        if (!(Vec3Eq(pos, e.pos) && Vec3Eq(scale, e.scale) && QuatEq(rot, e.rot))) {
            gizmoDragOriginValid_ = true;
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, e.rot, e.scale, pos, rot, scale,
                EditTransformCommand::kAll));
        }
    }
    gizmoDragActive_ = ImGuizmo::IsUsing();
    if (!gizmoDragActive_) {
        // The drag just ended: seal the command it produced so a FUTURE drag of
        // the same entity starts its own undo step (one drag = one undo step).
        if (gizmoDragOriginValid_) {
            if (EditTransformCommand* top =
                    dynamic_cast<EditTransformCommand*>(history_.TopUndo())) {
                if (top->Matches(selected_, EditTransformCommand::kAll)) top->Seal();
            }
        }
        gizmoDragOriginValid_ = false;
    }

    if (smokeMode_ && !gizmoDrawn_) {
        gizmoDrawn_ = true;
        const bool drewGeometry = dl->CmdBuffer.Size > cmdsBefore &&
                                  dl->VtxBuffer.Size > vtxBefore;
        NEON_LOG_INFO("EDITOR-GIZMO-SMOKE: [%s] gizmo drawn (op=%d mode=%d entity='%s' cmds+%d vtx+%d)",
                      drewGeometry ? "PASS" : "FAIL", static_cast<int>(gizmoOp_),
                      static_cast<int>(gizmoMode_), e.name.c_str(),
                      dl->CmdBuffer.Size - cmdsBefore, dl->VtxBuffer.Size - vtxBefore);
        if (!drewGeometry) smokeFailed_ = true;
    }

    // On the smoke frame, synthesize the full ImGuizmo input path (hover the
    // dock host, press, drag, release) to verify the gizmo is actually
    // grabbable. Runs here, inside the viewport window scope, because
    // ImGuizmo::Manipulate needs a current window to draw into.
    if (smokeMode_ && TimeRef().frameIndex == 30 && !gizmoDragSimulated_) {
        gizmoDragSimulated_ = true;
        RunGizmoDragSim();
    }
}

void EditorApp::RunGizmoDragSim() {
    auto report = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-GIZMO-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
        report(false, "drag sim needs a selected entity");
        return;
    }
    SceneEntity& sel = entities_[static_cast<size_t>(selected_)];
    math::Mat4 modelBefore = math::Mat4::Translation(sel.pos) * sel.rot.ToMat4() *
                             math::Mat4::Scale(sel.scale);

    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    ImGuiIO& io = ctx.IO;

    // Preserve the real frame's input state; restored at the end.
    const bool savedDown = io.MouseDown[0];
    const float savedDur = io.MouseDownDuration[0];
    const ImVec2 savedPos = io.MousePos;
    const ImGuiID savedActive = ctx.ActiveId;
    const ImGuiID savedHovered = ctx.HoveredId;
    const ImGuiID savedHoveredPrev = ctx.HoveredIdPreviousFrame;
    ImGuiWindow* savedHoveredWin = ctx.HoveredWindow;

    float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
    gfx::Camera cam = OrbitCamera();
    float view[16], proj[16];
    Mat4ToGizmo(cam.View(), view);
    Mat4ToGizmo(cam.Projection(aspect), proj);

    // Screen position of the entity origin under the full-window rect (the
    // same rect the gizmo now uses), in y-down ImGui pixels.
    math::Mat4 vp = cam.ViewProjection(aspect);
    math::Vec4 clip = vp.TransformVec4({sel.pos.x, sel.pos.y, sel.pos.z, 1.0f});
    const float gx = (clip.x / clip.w * 0.5f + 0.5f) *
                     static_cast<float>(renderer_.ScreenWidth());
    const float gy = (0.5f - clip.y / clip.w * 0.5f) *
                     static_cast<float>(renderer_.ScreenHeight());

    // The docked leaf's parent IS the dock host ImGui reports as hovered; the
    // same window SetAlternativeWindow points the gizmo at.
    ImGuiWindow* vpWin = ImGui::FindWindowByName("视口");
    ImGuiWindow* hostWin = (vpWin && vpWin->ParentWindow) ? vpWin->ParentWindow : vpWin;
    report(hostWin != nullptr, "drag sim resolves the dock host window");
    if (!hostWin) return;

    // The real hover path relies on ImGui reporting the dock host as the
    // hovered window when the mouse is over the viewport (OnUpdate parked the
    // mouse on the viewport center for this smoke frame). If it doesn't match
    // the host the gizmo is bound to, SetAlternativeWindow is wrong/removed
    // and the gizmo would be undraggable - fail the smoke here.
    report(ctx.HoveredWindow == hostWin,
           "real hover over the viewport resolves to the dock host window");

    // Clear hover/active so CanActivate() sees no other ImGui item.
    ctx.HoveredWindow = hostWin;
    ctx.ActiveId = 0;
    ctx.HoveredId = 0;
    ctx.HoveredIdPreviousFrame = 0;

    float gizmoModel[16];
    Mat4ToGizmo(modelBefore, gizmoModel);

    // Press over the entity origin -> the screen-space translate handle.
    io.MousePos = ImVec2(gx, gy);
    io.MouseDown[0] = true;
    io.MouseDownDuration[0] = 0.0f; // pressed this frame: IsMouseClicked fires
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, static_cast<float>(renderer_.ScreenWidth()),
                      static_cast<float>(renderer_.ScreenHeight()));
    // NOTE: deliberately NOT re-arming SetAlternativeWindow here. The real
    // DrawTransformGizmo set it this frame; the activation below only succeeds
    // if that setting matches ctx.HoveredWindow (the dock host), so a removed
    // or retargeted SetAlternativeWindow is caught by this smoke.
    ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, gizmoModel);
    report(ImGuizmo::IsUsing(), "gizmo activates on click over a handle");

    // Drag: hold the button and move the mouse off the origin.
    io.MouseDownDuration[0] = 0.1f;
    io.MousePos = ImVec2(gx + 40.0f, gy);
    const bool dragged = ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE,
                                              ImGuizmo::WORLD, gizmoModel);
    report(ImGuizmo::IsUsing(), "gizmo stays active while dragging");
    math::Mat4 m;
    GizmoToMat4(gizmoModel, m);
    report(dragged && (m.m[3] != modelBefore.m[3] || m.m[7] != modelBefore.m[7] ||
                       m.m[11] != modelBefore.m[11]),
           "gizmo drag moves the model matrix");

    // Release.
    io.MouseDown[0] = false;
    ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, gizmoModel);
    report(!ImGuizmo::IsUsing(), "gizmo deactivates on release");

    // Restore the frame's input state so the rest of the frame sees it as-is.
    io.MouseDown[0] = savedDown;
    io.MouseDownDuration[0] = savedDur;
    io.MousePos = savedPos;
    ctx.ActiveId = savedActive;
    ctx.HoveredId = savedHovered;
    ctx.HoveredIdPreviousFrame = savedHoveredPrev;
    ctx.HoveredWindow = savedHoveredWin;
}

void EditorApp::BuildCustomUIDemo() {
    ui::Element* root = ui_.Root();

    auto win = std::make_unique<ui::Window>();
    win->name = "ui_demo";
    win->title = "引擎 UI 演示";
    win->rect = {280, 480, 720, 236};
    auto* dock = new ui::DockLayout();
    dock->name = "demo_dock";
    win->Add(std::unique_ptr<ui::Element>(dock));

    // Left pane: TreeView.
    auto left = std::make_unique<ui::Panel>();
    left->fill = false;
    auto tree = std::make_unique<ui::TreeView>();
    tree->name = "demo_tree";
    demoTree_ = tree.get();
    ui::TreeNode sceneNode;
    sceneNode.text = "场景";
    sceneNode.expanded = true;
    ui::TreeNode entitiesNode;
    entitiesNode.text = "实体";
    entitiesNode.expanded = true;
    ui::TreeNode groundNode;
    groundNode.text = "地面";
    ui::TreeNode helmetNode;
    helmetNode.text = "头盔";
    entitiesNode.children.push_back(groundNode);
    entitiesNode.children.push_back(helmetNode);
    ui::TreeNode lightsNode;
    lightsNode.text = "光照";
    sceneNode.children.push_back(entitiesNode);
    sceneNode.children.push_back(lightsNode);
    tree->nodes.push_back(sceneNode);
    left->Add(std::move(tree));
    dock->Add(std::move(left));

    // Center pane: TabBar with form / scroll-list pages.
    auto center = std::make_unique<ui::Panel>();
    center->fill = false;
    auto tabs = std::make_unique<ui::TabBar>();
    tabs->name = "demo_tabs";
    tabs->tabs = {"表单", "滚动列表"};
    auto form = std::make_unique<ui::VBox>();
    form->name = "page_form";
    auto combo = std::make_unique<ui::ComboBox>();
    combo->name = "demo_combo";
    combo->options = {"地形", "头盔", "方块", "松树"};
    combo->selected = 0;
    combo->onChange = [this](int i) { demoComboChanged_ = i; };
    demoCombo_ = combo.get();
    form->Add(std::move(combo));
    auto addBtn = std::make_unique<ui::Button>();
    addBtn->name = "add_tree_node";
    addBtn->text = "+添加节点";
    addBtn->onClick = [this] {
        if (!demoTree_) return;
        ui::TreeNode n;
        n.text = "节点" + std::to_string(++demoAddClicks_);
        demoTree_->nodes[0].children[0].children.push_back(std::move(n));
    };
    demoAddButton_ = addBtn.get();
    form->Add(std::move(addBtn));
    auto info = std::make_unique<ui::Label>();
    info->text = "自研控件树：DockLayout + TabBar + ComboBox + TreeView";
    form->Add(std::move(info));
    tabs->Add(std::move(form));
    auto scrollPage = std::make_unique<ui::Panel>();
    scrollPage->fill = false;
    auto scroll = std::make_unique<ui::ScrollArea>();
    scroll->name = "demo_scroll";
    auto list = std::make_unique<ui::List>();
    for (int i = 1; i <= 20; ++i) list->items.push_back("第 " + std::to_string(i) + " 行");
    scroll->Add(std::move(list));
    scrollPage->Add(std::move(scroll));
    tabs->Add(std::move(scrollPage));
    center->Add(std::move(tabs));
    dock->Add(std::move(center));

    // Right pane: info labels.
    auto right = std::make_unique<ui::Panel>();
    right->fill = false;
    auto vbox = std::make_unique<ui::VBox>();
    auto lbl1 = std::make_unique<ui::Label>();
    lbl1->text = "右侧面板";
    auto lbl2 = std::make_unique<ui::Label>();
    lbl2->text = "拖动分隔条调整布局";
    vbox->Add(std::move(lbl1));
    vbox->Add(std::move(lbl2));
    right->Add(std::move(vbox));
    dock->Add(std::move(right));

    root->Add(std::move(win));
}

void EditorApp::BuildImGuiUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存场景", "Ctrl+S")) SaveScene();
            if (ImGui::MenuItem("加载场景", "Ctrl+L")) LoadScene("editor_scene.json");
            ImGui::Separator();
            if (ImGui::MenuItem("退出")) {
                if (Window()) Window()->RequestClose();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景", nullptr, &showHierarchy_);
            ImGui::MenuItem("属性", nullptr, &showInspector_);
            ImGui::MenuItem("资产", nullptr, &showAssets_);
            ImGui::MenuItem("资源", nullptr, &showResources_);
            ImGui::MenuItem("日志", nullptr, &showLog_);
            ImGui::MenuItem("行为树", nullptr, &showBt_);
            ImGui::Separator();
            ImGui::MenuItem("引擎 UI 演示", nullptr, &showCustomUIDemo_);
            ImGui::MenuItem("ImGui Demo", nullptr, &showImGuiDemo_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("项目")) {
            ImGui::TextUnformatted("项目目录");
            if (ImGui::InputText("##project_dir", projectDirBuf_, sizeof(projectDirBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                projectDir_ = projectDirBuf_;
                if (projectDir_.empty()) projectDir_ = ".";
                SaveEditorConfig();
            }
            ImGui::TextDisabled("导出场景写入 %s/scenes/exported_scene.json",
                                projectDir_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("导出游戏场景")) ExportScene();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::MenuItem("关于", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Transform-gizmo shortcuts: W/E/R switch the operation while an entity is
    // selected (ignored while the user is typing text, e.g. the name field).
    if (selected_ >= 0 && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = ImGuizmo::SCALE;
    }

    // Docking layout: full-workspace dock space below the menu bar.
    const float menuH = ImGui::GetFrameHeight();
    const float toolH = 36.0f;
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH + toolH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(mainVp->WorkSize.x, mainVp->WorkSize.y - menuH - toolH),
                             ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGuiWindowFlags dsFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##NeonDockSpace", nullptr, dsFlags);
    ImGui::PopStyleVar(3);
    ImGuiID dockId = ImGui::GetID("NeonDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // First-run default docking layout (skipped when the ini already restores one).
    static bool layoutAttempted = false;
    if (!layoutAttempted) {
        layoutAttempted = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (node == nullptr || !node->IsSplitNode()) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId,
                                          ImVec2(mainVp->WorkSize.x,
                                                 mainVp->WorkSize.y - menuH - toolH));
            ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.24f,
                                                        nullptr, &dockId);
            ImGuiID left = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.22f,
                                                       nullptr, &dockId);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.30f,
                                                         nullptr, &dockId);
            ImGui::DockBuilderDockWindow("场景", left);
            ImGui::DockBuilderDockWindow("资产", left);
            ImGui::DockBuilderDockWindow("资源", left);
            ImGui::DockBuilderDockWindow("属性", right);
            ImGui::DockBuilderDockWindow("日志", right);
            ImGui::DockBuilderDockWindow("视口", dockId);
            ImGui::DockBuilderFinish(dockId);
        }
    }

    // Toolbar row below the menu bar.
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 34.0f), ImGuiCond_Always);
    ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toolbar", nullptr, tbFlags)) {
        if (ImGui::Button("保存")) SaveScene();
        ImGui::SameLine();
        if (ImGui::Button("加载")) LoadScene("editor_scene.json");
        ImGui::SameLine();
        if (ImGui::Button("+头盔")) AddEntity("helmet");
        ImGui::SameLine();
        if (ImGui::Button("+方块")) AddEntity("cube");
        ImGui::SameLine();
        if (ImGui::Button("+松树")) AddEntity("tree");
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
                history_.Push(std::make_unique<DeleteEntityCommand>(
                    &entities_, static_cast<size_t>(selected_)));
                selected_ = -1;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(playtestActive_ ? "■ 停止试玩" : "▶ 试玩")) TogglePlaytest();
        ImGui::SameLine();
        if (ImGui::Button("导出场景")) ExportScene();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // Gizmo operation (W/E/R) and mode toggle for the selected entity.
        if (ImGui::Button(gizmoOp_ == ImGuizmo::TRANSLATE ? "[移动] W" : "移动 W"))
            gizmoOp_ = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoOp_ == ImGuizmo::ROTATE ? "[旋转] E" : "旋转 E"))
            gizmoOp_ = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoOp_ == ImGuizmo::SCALE ? "[缩放] R" : "缩放 R"))
            gizmoOp_ = ImGuizmo::SCALE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoMode_ == ImGuizmo::LOCAL ? "[本地]" : "本地"))
            gizmoMode_ = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::Button(gizmoMode_ == ImGuizmo::WORLD ? "[世界]" : "世界"))
            gizmoMode_ = ImGuizmo::WORLD;
        ImGui::SameLine();
        ImGui::Text("实体 %zu", entities_.size());
    }
    ImGui::End();

    BuildScenePanel();
    BuildAssetPanel();
    BuildResourcePanel();
    BuildInspectorPanel();
    BuildLogPanel();
    BuildBtPanel();
    BuildViewportPanel();

    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
}

void EditorApp::RunUISmokeTest() {
    auto check = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-UI-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };

    // --- Dear ImGui tool layer ---
    check(ImGui::GetCurrentContext() != nullptr, "ImGui context created");
    check(ImGui::GetIO().Fonts->IsBuilt(), "ImGui font atlas built");
    check(ImGui::GetIO().Fonts->Fonts.Size >= 1, "ImGui has at least one font");
    ImDrawData* dd = ImGui::GetDrawData();
    check(dd != nullptr && dd->CmdListsCount > 0, "ImGui produced draw data");

    // --- Custom (engine widget) UI demo ---
    check(demoTree_ != nullptr && demoCombo_ != nullptr && demoAddButton_ != nullptr,
          "custom UI demo widgets exist");
    if (!demoTree_ || !demoCombo_ || !demoAddButton_) return;

    size_t leavesBefore = demoTree_->nodes[0].children[0].children.size();
    math::Vec2 addAbs = demoAddButton_->AbsolutePos();
    math::Vec2 addCenter{addAbs.x + demoAddButton_->rect.w * 0.5f,
                         addAbs.y + demoAddButton_->rect.h * 0.5f};
    check(ui_.HitTestAt(addCenter) == demoAddButton_, "hit test finds add button");
    demoAddButton_->MouseMove(addCenter);
    demoAddButton_->MouseDown(addCenter, platform::MouseButton::Left);
    demoAddButton_->MouseUp(addCenter, platform::MouseButton::Left);
    check(demoTree_->nodes[0].children[0].children.size() == leavesBefore + 1,
          "add button adds a tree node");

    math::Vec2 treeAbs = demoTree_->AbsolutePos();
    math::Vec2 arrow{treeAbs.x + 10.0f, treeAbs.y + 12.0f};
    check(ui_.HitTestAt(arrow) == demoTree_, "hit test finds tree view");
    demoTree_->MouseDown(arrow, platform::MouseButton::Left);
    check(!demoTree_->nodes[0].expanded, "tree arrow collapses root");
    demoTree_->MouseDown(arrow, platform::MouseButton::Left);
    check(demoTree_->nodes[0].expanded, "tree arrow expands root again");

    math::Vec2 comboAbs = demoCombo_->AbsolutePos();
    math::Vec2 boxCenter{comboAbs.x + 40.0f, comboAbs.y + 10.0f};
    check(ui_.HitTestAt(boxCenter) == demoCombo_, "hit test finds combo box");
    demoCombo_->MouseDown(boxCenter, platform::MouseButton::Left);
    check(demoCombo_->open, "combo opens on click");
    math::Vec2 row1{comboAbs.x + 40.0f, comboAbs.y + demoCombo_->rect.h + 30.0f};
    demoCombo_->MouseDown(row1, platform::MouseButton::Left);
    check(demoCombo_->selected == 1 && demoComboChanged_ == 1,
          "combo selects row and fires onChange");
    check(!demoCombo_->open, "combo closes after selection");

    // --- Tool panels ---
    check(!core::GetRecentLogs(16).empty(), "log panel has engine log entries");
    check(!assetEntries_.empty(), "asset panel enumerated files");

    // --- Transform gizmo ---
    // The gizmo renders every frame while an entity is selected; verify the
    // setup path ran and the matrix boundary (engine row-major Mat4 <-> ImGuizmo
    // column-major float[16]) round-trips a synthetic TRS without drift.
    check(gizmoDrawn_, "transform gizmo drawn in the viewport");
    check(gizmoBeginFrame_, "ImGuizmo::BeginFrame called every frame");
    check(gizmoAltWindowSet_, "gizmo hover bound to the dock host window");
    check(gizmoRect_[0] == 0.0f && gizmoRect_[1] == 0.0f &&
              gizmoRect_[2] == static_cast<float>(renderer_.ScreenWidth()) &&
              gizmoRect_[3] == static_cast<float>(renderer_.ScreenHeight()),
          "gizmo rect is the full window (matches scene render + picker)");
    auto nearVec = [](const math::Vec3& a, const math::Vec3& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f &&
               std::fabs(a.z - b.z) < 1e-4f;
    };
    {
        math::Vec3 pos{1.25f, -2.5f, 3.75f};
        math::Vec3 scale{2.0f, 0.5f, 1.5f};
        math::Quat rot = math::Quat::FromEuler(0.4f, -0.7f, 0.2f);
        math::Mat4 model = math::Mat4::Translation(pos) * rot.ToMat4() *
                           math::Mat4::Scale(scale);
        float gizmo[16];
        Mat4ToGizmo(model, gizmo);
        math::Mat4 back;
        GizmoToMat4(gizmo, back);
        math::Vec3 p, s;
        math::Quat q;
        DecomposeModel(back, p, s, q);
        check(nearVec(p, pos), "gizmo round-trip preserves translation");
        check(nearVec(s, scale), "gizmo round-trip preserves scale");
        check(math::Distance(rot.Rotate({0, 0, -1}), q.Rotate({0, 0, -1})) < 1e-3f,
              "gizmo round-trip preserves rotation");
    }
    // --- Undo/redo: scene edits route through the history command stack ---
    // Do -> undo -> redo on the real editor scene: push transform edits,
    // verify the merge policy (consecutive same-field edits = one undo step)
    // and the drag-end seal (the next drag = a new undo step), then drive
    // Ctrl+Z / Ctrl+Y through the real keyboard event path.
    {
        const size_t idx = 0; // deterministic: the first scene entity
        check(idx < entities_.size(), "undo/redo: smoke has an entity to edit");
        if (idx < entities_.size()) {
            SceneEntity& sel = entities_[idx];
            const math::Vec3 orig = sel.pos;
            const math::Vec3 step1 = orig + math::Vec3{0.5f, -0.25f, 0.125f};
            const math::Vec3 step2 = step1 + math::Vec3{0.1f, 0.2f, 0.3f};
            const math::Vec3 step3 = step2 + math::Vec3{0.2f, -0.3f, 0.4f};
            const math::Vec3 step4 = step3 + math::Vec3{0.3f, 0.1f, -0.2f};
            const size_t depthBefore = history_.UndoDepth();

            auto editPos = [&](const math::Vec3& from, const math::Vec3& to) {
                history_.Push(std::make_unique<EditTransformCommand>(
                    &entities_, static_cast<int>(idx), from, sel.rot, sel.scale, to, sel.rot,
                    sel.scale, EditTransformCommand::kPos));
            };

            editPos(orig, step1);
            check(nearVec(sel.pos, step1),
                  "undo/redo: transform edit applies through the command stack");
            editPos(step1, step2); // continuous chain -> coalesces
            check(history_.UndoDepth() == depthBefore + 1,
                  "undo/redo: consecutive same-field edits merge into one undo step");
            check(nearVec(sel.pos, step2),
                  "undo/redo: merged command holds the final value");

            // Value-chain guard: an edit whose ORIGINAL does not equal the last
            // applied value (e.g. a programmatic set between two separate
            // inspector drags) must NOT merge into the (unsealed) top step.
            editPos(step1, step3);
            check(history_.UndoDepth() == depthBefore + 2,
                  "undo/redo: discontinuous chain opens its own undo step");
            check(nearVec(sel.pos, step3), "undo/redo: discontinuous edit applies");

            // Seal the top command (what the gizmo does when a drag ends): the
            // next edit must open a fresh undo step too.
            if (EditTransformCommand* top =
                    dynamic_cast<EditTransformCommand*>(history_.TopUndo())) {
                top->Seal();
            }
            editPos(step3, step4);
            check(history_.UndoDepth() == depthBefore + 3,
                  "undo/redo: sealed command opens a new undo step");
            check(nearVec(sel.pos, step4), "undo/redo: post-seal edit applies");

            // Ctrl+Z / Ctrl+Y through the real keyboard event path.
            auto shortcut = [this](platform::Key key, bool withCtrl) {
                if (withCtrl) {
                    platform::InputEvent ctrlDown;
                    ctrlDown.type = platform::InputEvent::Type::KeyDown;
                    ctrlDown.key = platform::Key::Control;
                    Input()->HandleEvent(ctrlDown);
                    OnEvent(ctrlDown);
                }
                platform::InputEvent press;
                press.type = platform::InputEvent::Type::KeyDown;
                press.key = key;
                Input()->HandleEvent(press);
                OnEvent(press);
                if (withCtrl) {
                    platform::InputEvent ctrlUp;
                    ctrlUp.type = platform::InputEvent::Type::KeyUp;
                    ctrlUp.key = platform::Key::Control;
                    Input()->HandleEvent(ctrlUp);
                    OnEvent(ctrlUp);
                }
            };
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, step3), "undo/redo: Ctrl+Z undoes the post-seal edit");
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, step1),
                  "undo/redo: Ctrl+Z undoes the discontinuous edit");
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, orig), "undo/redo: Ctrl+Z undoes the merged drag");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step2), "undo/redo: Ctrl+Y redoes the merged drag");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step3),
                  "undo/redo: Ctrl+Y redoes the discontinuous edit");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step4), "undo/redo: Ctrl+Y redoes the post-seal edit");
            // Leave the scene as it was: undo everything we just did.
            shortcut(platform::Key::Z, true);
            shortcut(platform::Key::Z, true);
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, orig),
                  "undo/redo: restores the original transform");
        }
    }

    // --- Add/delete index stability through the command stack ---
    // add -> delete -> undo (restore) -> redo (delete again) must keep every
    // other entity index valid: commands record the index + an entity copy and
    // rely on LIFO undo / FIFO redo to execute against the exact layout they
    // captured.
    {
        const size_t baseCount = entities_.size();
        check(baseCount > 1, "undo/redo: index-stability smoke needs entities");
        if (baseCount > 1) {
            const size_t mid = 1;
            const std::string nameAtMid = entities_[mid].name;
            const SceneEntity sample = entities_[mid]; // valid-mesh stand-in
            history_.Push(std::make_unique<AddEntityCommand>(&entities_, sample, mid));
            check(entities_.size() == baseCount + 1 && entities_[mid].name == sample.name,
                  "undo/redo: add inserts at the recorded index");
            history_.Push(std::make_unique<DeleteEntityCommand>(&entities_, mid));
            check(entities_.size() == baseCount && entities_[mid].name == nameAtMid,
                  "undo/redo: delete removes the inserted entity (indices stable)");
            history_.Undo();
            check(entities_.size() == baseCount + 1 && entities_[mid].name == sample.name,
                  "undo/redo: undo delete restores the entity at its recorded index");
            history_.Redo();
            check(entities_.size() == baseCount && entities_[mid].name == nameAtMid,
                  "undo/redo: redo delete removes it again (indices stable)");
        }
    }

    // --- Gizmo activation/drag (deterministic, drives ImGuizmo's input path) ---
    // A real pointer drag can't be automated headlessly, but the activation
    // path is: RunGizmoDragSim() (called inside the viewport window scope on
    // the smoke frame) synthesizes a hover over the dock host, a press on the
    // entity's screen position, a drag, and a release, and verifies IsUsing()
    // follows and the model matrix moves. Assert here that it ran.
    check(gizmoDragSimulated_, "gizmo drag simulation ran");

    assets::AssetStats stats = assetMgr_.Stats();
    check(stats.textures >= 4, "resource panel: PBR textures cached");
    check(stats.meshes >= 1, "resource panel: meshes cached");

    size_t beforeImport = entities_.size();
    ImportAssetPath("assets/models/DamagedHelmet/DamagedHelmet.gltf");
    check(entities_.size() == beforeImport + 1, "asset import adds glTF entity");
    if (entities_.size() > beforeImport) {
        const SceneEntity& last = entities_.back();
        check(last.meshKey.rfind("gltf:", 0) == 0 && last.mesh.Valid(),
              "imported entity resolves glTF mesh");
    }

    // --- Editor config round-trip: save then load the project dir ---
    {
        const std::string cfgDir = GetTempDir() + "/cfg_proj";
        const std::string cfgPrev = projectDir_;
        projectDir_ = cfgDir;
        SaveEditorConfig();
        LoadEditorConfig();
        check(projectDir_ == cfgDir, "editor config project dir round-trips");
        projectDir_ = cfgPrev;
    }

    // --- Material editor: metallic / AO / texture-slot edits via undo ---
    // Set a texture path + metallic + AO on a selected entity through the
    // command stack, verify undo/redo restores, then leave the edits applied so
    // the export round-trip below asserts the material JSON + restored
    // SceneMesh carry them.
    const std::string kAlbedoTex = "assets/models/DamagedHelmet/Default_albedo.jpg";
    const float kMetallic = 0.45f;
    const float kAO = 0.7f;
    {
        const size_t idx = 0;
        check(idx < entities_.size(), "material: smoke has an entity to edit");
        if (idx < entities_.size()) {
            SceneEntity& sel = entities_[idx];
            const float origMetallic = sel.metallic;
            const float origAO = sel.ao;
            const std::string origAlbedo = sel.albedoTex;

            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, static_cast<int>(idx), ApplyMetallicProp, origMetallic, kMetallic));
            check(sel.metallic == kMetallic && sel.material.metallic == kMetallic,
                  "material: metallic edit applies through the command stack");
            history_.Undo();
            check(sel.metallic == origMetallic && sel.material.metallic == origMetallic,
                  "material: metallic undo restores the original value");
            history_.Redo();
            check(sel.metallic == kMetallic && sel.material.metallic == kMetallic,
                  "material: metallic redo reapplies the edit");

            gfx::Texture tex = assetMgr_.LoadTexture(kAlbedoTex);
            check(tex.Valid(), "material: albedo texture loads through the AssetManager");
            if (tex.Valid()) {
                const TextureSlotValue oldVal{origAlbedo, sel.material.albedo};
                const TextureSlotValue newVal{kAlbedoTex, tex.Handle()};
                history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                    &entities_, static_cast<int>(idx), ApplyAlbedoTexSlot, oldVal, newVal));
                check(sel.albedoTex == kAlbedoTex &&
                          sel.material.albedo.id == tex.Handle().id,
                      "material: albedo texture edit applies through the command stack");
                history_.Undo();
                check(sel.albedoTex == origAlbedo && !sel.material.albedo.Valid(),
                      "material: albedo undo restores the empty slot");
                history_.Redo();
                check(sel.albedoTex == kAlbedoTex && sel.material.albedo.Valid(),
                      "material: albedo redo reapplies the path + handle");
            }

            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, static_cast<int>(idx), ApplyAOProp, origAO, kAO));
            check(sel.ao == kAO && sel.material.aoStrength == kAO,
                  "material: AO edit applies through the command stack");
            history_.Undo();
            check(sel.ao == origAO, "material: AO undo restores the original value");
            history_.Redo();
            check(sel.ao == kAO && sel.material.aoStrength == kAO,
                  "material: AO redo reapplies the edit");
        }
    }

    // --- Export → load round-trip (temp project dir; no repo pollution) ---
    const size_t exportCount = entities_.size();
    const std::string oldProjectDir = projectDir_;
    projectDir_ = GetTempDir();
    core::Status exportStatus = ExportScene();
    projectDir_ = oldProjectDir;
    check(exportStatus.Ok(), "export scene writes componentized JSON");
    if (exportStatus.Ok()) {
        std::string exportedPath = GetTempDir() + "/scenes/exported_scene.json";
        std::ifstream fin(exportedPath);
        std::stringstream fss;
        fss << fin.rdbuf();
        auto parsed = scene::SceneFile::Parse(fss.str());
        check(parsed.Ok(), "exported scene parses with SceneFile::Parse");
        if (parsed.Ok()) {
            check(parsed.Value().entities.size() == exportCount,
                  "exported scene contains every editor entity");
            if (!parsed.Value().entities.empty()) {
                check(parsed.Value().entities[0].name == entities_[0].name,
                      "exported entity name matches editor entity");
                // The material edit round-trips into the exported material JSON.
                const scene::ComponentDef* meshComp = nullptr;
                for (const auto& c : parsed.Value().entities[0].components) {
                    if (c.name == "mesh") {
                        meshComp = &c;
                        break;
                    }
                }
                const core::Json* matJson =
                    meshComp ? meshComp->data.Get("material") : nullptr;
                const core::Json* alb = matJson ? matJson->Get("albedoTex") : nullptr;
                const core::Json* met = matJson ? matJson->Get("metallic") : nullptr;
                const core::Json* ao = matJson ? matJson->Get("ao") : nullptr;
                check(meshComp != nullptr && matJson != nullptr && alb != nullptr &&
                          met != nullptr && ao != nullptr &&
                          alb->GetString() == kAlbedoTex &&
                          std::fabs(met->GetNumber() - kMetallic) < 1e-6f &&
                          std::fabs(ao->GetNumber() - kAO) < 1e-6f,
                      "exported material JSON carries the texture path + metallic + AO");
            }
            // Import back: Instantiate the exported scene and verify SceneMesh
            // restores the material texture path + scalar edits.
            scene::ComponentRegistry reg;
            scene::RegisterBuiltinComponents(reg);
            ecs::World world;
            scene::PrefabLibrary prefs;
            auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
            check(inst.Ok(), "imported exported scene instantiates");
            auto view = world.ViewAll<scene::SceneMesh>();
            check(view.Size() > 0, "imported scene has mesh components");
            if (view.Size() > 0) {
                ecs::Entity e0 = world.EntityAt<scene::SceneMesh>(0);
                const scene::SceneMesh* m = world.Get<scene::SceneMesh>(e0);
                check(m != nullptr && m->albedoTex == kAlbedoTex &&
                          std::fabs(m->metallic - kMetallic) < 1e-6f &&
                          std::fabs(m->ao - kAO) < 1e-6f,
                      "imported SceneMesh carries the texture path + metallic + AO");
            }
        }
    }

    // --- Behavior tree editor: canvas + model + save/load + undo ---
    {
        check(btCanvasDrawn_, "bt canvas renders the seeded tree");
        check(btGraph_.NodeCount() == 3u && btGraph_.LinkCount() == 2u,
              "bt panel seeded a 3-node linked tree");

        // Model-level create/link/serialize -> save to a temp .bt.json ->
        // load back -> assert identical (the graph->JSON->graph round-trip).
        btgraph::BtGraph g;
        const std::string r = g.AddNode("sequence", math::Vec2{});
        const std::string c = g.AddNode("in_range", math::Vec2{});
        const std::string a = g.AddNode("move_to", math::Vec2{});
        core::Json d, s;
        d.type_ = core::Json::Type::Number;
        d.number_ = 8.0;
        s.type_ = core::Json::Type::Number;
        s.number_ = 3.0;
        g.SetArg(c, "distance", d);
        g.SetArg(a, "speed", s);
        g.SetParent(c, r);
        g.SetParent(a, r);
        const std::string json = g.Serialize();

        const std::string btPath = GetTempDir() + "/bt_smoke.bt.json";
        {
            std::ofstream out(btPath, std::ios::binary);
            out << json;
        }
        check(!json.empty(), "bt smoke: tree serialized");
        std::string loadedText;
        std::ifstream in(btPath, std::ios::binary);
        loadedText.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
        btgraph::BtGraph loaded;
        check(loaded.FromTreeJson(core::Json::Parse(loadedText, nullptr)),
              "bt smoke: saved .bt.json reloads");
        check(loaded.Serialize() == json, "bt smoke: save/load round-trip identical");

        // Editor graph edits route through the undo stack: add -> undo -> redo.
        const size_t nodesBefore = btGraph_.NodeCount();
        const btgraph::BtGraph before = btGraph_;
        const std::string nid = btGraph_.AddNode("wait", math::Vec2{0.f, 0.f});
        BtPushSnapshot(before);
        check(!nid.empty() && btGraph_.NodeCount() == nodesBefore + 1,
              "bt smoke: canvas add node");
        btHistory_.Undo();
        check(btGraph_.NodeCount() == nodesBefore, "bt smoke: undo restores the graph");
        btHistory_.Redo();
        check(btGraph_.NodeCount() == nodesBefore + 1, "bt smoke: redo reapplies the add");
        btHistory_.Undo();
        check(btGraph_.NodeCount() == nodesBefore, "bt smoke: graph left clean after undo");
    }

    NEON_LOG_INFO("EDITOR-UI-SMOKE: all checks done");
}

void EditorApp::AddEntity(const std::string& meshKey) {
    static int counter = 1;
    math::Vec3 pos = camTarget_ + math::Vec3{0, 1.0f, -3.0f};
    std::string name;
    if (meshKey.rfind("obj:", 0) == 0 || meshKey.rfind("gltf:", 0) == 0) {
        std::string path = meshKey.substr(meshKey.find(':') + 1);
        size_t slash = path.find_last_of("/\\");
        size_t dot = path.find_last_of('.');
        size_t begin = slash == std::string::npos ? 0 : slash + 1;
        size_t len = (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = pos;
    if (meshKey == "tree") {
        e.scale = {1.6f, 1.6f, 1.6f};
    }
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        selected_ = static_cast<int>(entities_.size()) - 1;
    }
}

core::Result<core::Json> EditorApp::BuildPlaySceneJson() {
    if (entities_.empty())
        return core::Result<core::Json>::Err("editor: scene is empty");
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale,
                                                ExportMeshKey(e.meshKey), e.metallic,
                                                e.roughness, e.tint, e.albedoTex, e.mrTex,
                                                e.aoTex, e.emissiveTex, e.ao,
                                                e.emissiveIntensity);
        if (!res.Ok()) {
            return core::Result<core::Json>::Err("editor: " + res.Error());
        }
        arr.array_.push_back(res.Value());
    }
    root.object_["entities"] = std::move(arr);
    return core::Result<core::Json>::Ok(std::move(root));
}

void EditorApp::StartPlaytest() {
    StopPlaytest(); // restart semantics: a fresh snapshot each time
    if (entities_.empty()) {
        NEON_LOG_WARN("Editor: nothing to play (scene is empty)");
        return;
    }
    auto root = BuildPlaySceneJson();
    if (!root.Ok()) {
        NEON_LOG_ERROR("Editor: cannot build play scene: %s", root.Error().c_str());
        return;
    }
    std::string json = core::JsonWriter::Write(root.Value());

    scene::GameRuntimeConfig cfg;
    cfg.assets = &assetMgr_;
    cfg.scriptBaseDir = projectDir_.empty() ? "." : projectDir_;

    playtest_ = std::make_unique<scene::GameRuntime>();
    core::Status st = playtest_->Start(json, cfg);
    if (!st.Ok()) {
        NEON_LOG_ERROR("Editor: playtest failed to start: %s", st.Error().c_str());
        playtest_.reset();
        return;
    }
    playtestActive_ = true;
    NEON_LOG_INFO("Editor: playtest started (%zu entities, %zu scripts, %zu trees)",
                  playtest_->EntityCount(), playtest_->ScriptCount(),
                  playtest_->BehaviorTreeCount());
}

void EditorApp::StopPlaytest() {
    if (!playtest_) return;
    playtest_->Stop();
    playtest_.reset();
    playtestActive_ = false;
    NEON_LOG_INFO("Editor: playtest stopped");
}

void EditorApp::TogglePlaytest() {
    if (playtestActive_) {
        StopPlaytest();
    } else {
        StartPlaytest();
    }
}

core::Status EditorApp::ExportScene() {
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Editor: export aborted: %s", rootRes.Error().c_str());
        return core::Status::Err(rootRes.Error());
    }
    core::Json root = rootRes.Value();

    std::string base = projectDir_.empty() ? "." : projectDir_;
    std::string scenesDir = base + "/scenes";
    if (!EnsureDirs(scenesDir)) {
        NEON_LOG_ERROR("Editor: cannot create export directory '%s'", scenesDir.c_str());
        return core::Status::Err("editor: cannot create export directory '" + scenesDir + "'");
    }
    std::string path = scenesDir + "/exported_scene.json";
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out(path); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: exported scene (%zu entities) -> %s", entities_.size(),
                      path.c_str());
        return core::Status::Ok(true);
    }
    NEON_LOG_ERROR("Editor: cannot write '%s'", path.c_str());
    return core::Status::Err("editor: cannot write '" + path + "'");
}

void EditorApp::LoadEditorConfig() {
    projectDir_ = ".";
    std::ifstream in("neon_editor_config.json");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        core::Json root = core::Json::Parse(ss.str(), &err);
        if (root.IsObject()) {
            if (const core::Json* p = root.Get("projectDir")) projectDir_ = p->GetString();
        }
    }
    if (projectDir_.empty()) projectDir_ = ".";
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
}

void EditorApp::SaveEditorConfig() {
    if (projectDir_.empty()) projectDir_ = ".";
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json p;
    p.type_ = core::Json::Type::String;
    p.string_ = projectDir_;
    root.object_["projectDir"] = p;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("neon_editor_config.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: config saved (project dir '%s')", projectDir_.c_str());
    } else {
        NEON_LOG_WARN("Editor: cannot write editor config");
    }
}

bool EditorApp::ResolveMesh(SceneEntity& e) {
    const std::string& key = e.meshKey;
    if (key == "terrain") {
        e.mesh = MakeTerrain(renderer_);
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "helmet") {
        assets::GltfAsset gltf =
            assetMgr_.LoadGLTF("assets/models/DamagedHelmet/DamagedHelmet.gltf");
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    } else if (key == "cube") {
        e.mesh = gfx::Mesh::CreateCube(renderer_, 1, 1, 1, "cube");
        e.material = gfx::Material::Lit({}, e.tint, 12.0f);
    } else if (key == "tree") {
        e.mesh =
            assetMgr_.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj");
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("obj:", 0) == 0) {
        e.mesh = assetMgr_.LoadMeshOBJ(key.substr(4));
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("gltf:", 0) == 0) {
        assets::GltfAsset gltf = assetMgr_.LoadGLTF(key.substr(5));
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    }
    return e.mesh.Valid();
}

void EditorApp::ApplyMaterialParams(SceneEntity& e) {
    e.material.tint = e.tint;
    e.material.metallic = e.metallic;
    e.material.roughness = e.roughness;
    e.material.aoStrength = e.ao;
    e.material.emissiveIntensity = e.emissiveIntensity;
    // Texture slots: load any non-empty path through the cached AssetManager.
    // Empty paths leave the existing handle untouched (e.g. a glTF material's
    // baked PBR textures survive until the user explicitly overrides/clears).
    if (!e.albedoTex.empty()) e.material.albedo = assetMgr_.LoadTexture(e.albedoTex).Handle();
    if (!e.mrTex.empty()) e.material.metallicRoughness = assetMgr_.LoadTexture(e.mrTex).Handle();
    if (!e.aoTex.empty()) e.material.occlusion = assetMgr_.LoadTexture(e.aoTex).Handle();
    if (!e.emissiveTex.empty()) e.material.emissive = assetMgr_.LoadTexture(e.emissiveTex).Handle();
}

void EditorApp::SaveScene() {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        obj.type_ = core::Json::Type::Object;
        core::Json name;
        name.type_ = core::Json::Type::String;
        name.string_ = e.name;
        obj.object_["name"] = name;
        auto str = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto num = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto vec3 = [&](const math::Vec3& v) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            j.array_ = {num(v.x), num(v.y), num(v.z)};
            return j;
        };
        obj.object_["mesh"] = str(e.meshKey);
        obj.object_["pos"] = vec3(e.pos);
        obj.object_["scale"] = vec3(e.scale);
        core::Json tint;
        tint.type_ = core::Json::Type::Array;
        tint.array_ = {num(e.tint.r), num(e.tint.g), num(e.tint.b)};
        obj.object_["tint"] = tint;
        obj.object_["metallic"] = num(e.metallic);
        obj.object_["roughness"] = num(e.roughness);
        obj.object_["ao"] = num(e.ao);
        obj.object_["emissiveIntensity"] = num(e.emissiveIntensity);
        obj.object_["albedoTex"] = str(e.albedoTex);
        obj.object_["mrTex"] = str(e.mrTex);
        obj.object_["aoTex"] = str(e.aoTex);
        obj.object_["emissiveTex"] = str(e.emissiveTex);
        arr.array_.push_back(obj);
    }
    root.object_["entities"] = arr;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("editor_scene.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Scene saved (%zu entities)", entities_.size());
    }
}

void EditorApp::LoadScene(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    core::Json root = core::Json::Parse(ss.str(), &err);
    const core::Json* arr = root.Get("entities");
    if (!arr) return;
    // Replace entity list, re-resolve meshes.
    std::vector<SceneEntity> loaded;
    for (size_t i = 0; i < arr->Size(); ++i) {
        const core::Json* j = arr->At(i);
        if (!j) continue;
        SceneEntity e;
        e.name = j->Get("name")->GetString("entity");
        e.meshKey = j->Get("mesh")->GetString("cube");
        if (const core::Json* p = j->Get("pos")) {
            e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                     static_cast<float>(p->At(1)->GetNumber()),
                     static_cast<float>(p->At(2)->GetNumber())};
        }
        if (const core::Json* s = j->Get("scale")) {
            e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                       static_cast<float>(s->At(1)->GetNumber()),
                       static_cast<float>(s->At(2)->GetNumber())};
        }
        if (const core::Json* t = j->Get("tint")) {
            e.tint = {static_cast<float>(t->At(0)->GetNumber()),
                      static_cast<float>(t->At(1)->GetNumber()),
                      static_cast<float>(t->At(2)->GetNumber()), 1.0f};
        }
        if (const core::Json* m = j->Get("metallic")) e.metallic = static_cast<float>(m->GetNumber());
        if (const core::Json* r = j->Get("roughness")) e.roughness = static_cast<float>(r->GetNumber());
        if (const core::Json* a = j->Get("ao")) e.ao = static_cast<float>(a->GetNumber());
        if (const core::Json* ei = j->Get("emissiveIntensity")) e.emissiveIntensity = static_cast<float>(ei->GetNumber());
        if (const core::Json* at = j->Get("albedoTex")) e.albedoTex = at->GetString();
        if (const core::Json* mt = j->Get("mrTex")) e.mrTex = mt->GetString();
        if (const core::Json* aot = j->Get("aoTex")) e.aoTex = aot->GetString();
        if (const core::Json* et = j->Get("emissiveTex")) e.emissiveTex = et->GetString();
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            loaded.push_back(std::move(e));
        }
    }
    if (!loaded.empty()) {
        entities_ = std::move(loaded);
        selected_ = -1;
        history_.Clear(); // undo history from the previous scene is invalid
        NEON_LOG_INFO("Scene loaded (%zu entities)", entities_.size());
    }
}

void EditorApp::ClampSelection() {
    if (entities_.empty()) {
        selected_ = -1;
    } else if (selected_ >= static_cast<int>(entities_.size())) {
        selected_ = static_cast<int>(entities_.size()) - 1;
    }
}

} // namespace neon::editor
