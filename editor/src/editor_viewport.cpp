#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "font_data.hpp"
#include "imgui_internal.h"
#include "neon/gfx/imgui_neon.hpp"
#include "neon/gfx/scene_props.hpp"

// stbi_write_png declaration only: the implementation is compiled once in
// editor.cpp (STB_IMAGE_WRITE_IMPLEMENTATION), so OnRender's screenshot capture
// can call it here without a duplicate definition.
#include "stb_image_write.h"

namespace neon::editor {

void EditorApp::BindDock2DMapping(bool designFit, float aspect) {
    const math::Rect2& vp = viewportScreenRect_;
    if (designFit)
        renderer_.Set2DViewport(vp.x, vp.y, vp.w, vp.h, canvasZoom_, canvasPan_, aspect);
    else
        renderer_.Set2DViewportPixels(vp.x, vp.y);
}

EditorApp::DockViewportScope::DockViewportScope(EditorApp& app, bool designFit, bool sceneVp,
                                                float aspect)
    : app_(app), sceneVp_(sceneVp) {
    const math::Rect2& vp = app_.viewportScreenRect_;
    if (!(vp.w > 0.0f && vp.h > 0.0f) || !app_.renderer_.Backend()) return;
    active_ = true;
    app_.renderer_.Backend()->SetScissor(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                         static_cast<int>(vp.w), static_cast<int>(vp.h), true);
    app_.BindDock2DMapping(designFit, aspect);
    // The scene rasterizes into the SAME rect the game-area design space maps
    // to (letterboxed, centered), so 3D geometry, the 2D HUD and world-
    // anchored UI all share one framing. Without this, the 3D projection and
    // the 2D anchor space disagree and every plate/float text drifts away
    // from its entity as it moves off-centre.
    const math::Rect2 sceneVpRect = designFit ? app_.renderer_.DesignSpaceRect() : vp;
    app_.sceneRect_ = sceneVpRect;
    if (sceneVp_)
        app_.renderer_.SetSceneViewport(sceneVpRect.x, sceneVpRect.y, sceneVpRect.w,
                                        sceneVpRect.h);
}

EditorApp::DockViewportScope::~DockViewportScope() {
    if (!active_) return;
    if (sceneVp_) {
        app_.renderer_.ResetSceneViewport();
        // Letterbox bars drawn LAST (after the sky/3D), so they cover any
        // full-window 2D pass that bled into the dock margins — the scene
        // boundary reads as pure black against the editor background.
        const math::Rect2& vp = app_.viewportScreenRect_;
        const math::Rect2& sr = app_.sceneRect_;
        if (sr.w > 0.0f && sr.h > 0.0f) {
            auto fill = [&](float x, float y, float w, float h) {
                if (w <= 0.0f || h <= 0.0f) return;
                const math::Vec2 a = app_.renderer_.ScreenToUI({x, y});
                const math::Vec2 b = app_.renderer_.ScreenToUI({x + w, y + h});
                app_.renderer_.DrawRect(a, b - a, gfx::Color{0.0f, 0.0f, 0.0f, 1.0f});
            };
            fill(vp.x, vp.y, sr.x - vp.x, vp.h);      // left bar
            fill(sr.x + sr.w, vp.y,                   // right bar
                 vp.x + vp.w - (sr.x + sr.w), vp.h);
            fill(vp.x, vp.y, vp.w, sr.y - vp.y);      // top bar
            fill(vp.x, sr.y + sr.h, vp.w,             // bottom bar
                 vp.y + vp.h - (sr.y + sr.h));
        }
    }
    app_.renderer_.Flush2D();
    app_.renderer_.Backend()->SetScissor(0, 0, 0, 0, false);
    app_.renderer_.Reset2DViewport();
}

void EditorApp::OnRender() {
    renderer_.BeginFrame({0.06f, 0.08f, 0.13f, 1.0f});
    // Play locks the canvas for BOTH modes: every play viewport/overlay below
    // derives from viewportScreenRect_ with zoom=1/pan=0, so the whole design
    // space is always framed (a stale editor zoom/pan must not crop the game).
    // UpdateViewport keeps wheel/middle-drag disabled while playing.
    if (playActive_) {
        canvasZoom_ = 1.0f;
        canvasPan_ = {0.0f, 0.0f};
    }
    gfx::Camera cam = ActiveCamera(); // the scene branch re-reads after SetCamera
    if (showUIEditor_ && uiDocOpen_) {
        static bool uiEdLogged = false;
        if (!uiEdLogged) {
            uiEdLogged = true;
            NEON_LOG_INFO("UI-EDITOR-PREVIEW: active (doc='%s')", uiDocPath_.c_str());
        }        // UI editor preview: an EDITING CANVAS, not the runtime view - the
        // whole 1280x720 document fits inside the dock (Set2DViewport
        // fit-within, canvasZoom_/canvasPan_ for wheel zoom / middle-drag),
        // so every element stays visible and selectable. The document lays
        // out against the standard 1280x720 viewport; the RUNTIME adaptive
        // behavior is what the play mode shows.
        {
            DockViewportScope dock(*this, /*designFit=*/true, /*sceneVp=*/false);
            if (dock.Active()) {
                if (cjkFont_.Valid())
                    uiDoc_.Draw(renderer_, cjkFont_,
                                [this](const std::string& p) {
                                    if (p.empty()) return gfx::Texture{};
                                    return assetMgr_.LoadTexture(p);
                                },
                                {static_cast<float>(gfx::Renderer::kDesignWidth),
                                 static_cast<float>(gfx::Renderer::kDesignHeight)});
                // P5-editor UX: outline every selected node; the active one
                // gets resize handles. Box-layout nodes only have a resolved
                // rect, so read ResolvedRect (falls back to the rect chain).
                for (ui::UiNode* n : uiSelection_) {
                    if (!n) continue;
                    const math::Rect2 sel = n->ResolvedRect();
                    renderer_.DrawRectOutline(sel, 2.0f,
                                              {0.4f, 0.9f, 1.0f, 0.9f});
                    if (n != uiSelected_) continue;
                    const float hs = 8.0f;
                    const math::Vec2 corners[4] = {
                        {sel.x - hs, sel.y - hs},
                        {sel.x + sel.w, sel.y - hs},
                        {sel.x - hs, sel.y + sel.h},
                        {sel.x + sel.w, sel.y + sel.h}};
                    for (const math::Vec2& c : corners)
                        renderer_.DrawRect(c, {hs * 2, hs * 2}, {0.4f, 0.9f, 1.0f, 1.0f});
                }
            }
        }
        renderer_.EndScene();
    } else if (playActive_ && play_ && projectMode_ == "2d") {
        // 2D game play: fit the 1280x720 design space into the viewport
        // dock so the runtime's on_render (the actual game) draws where the
        // player sees it. Entities render through a FIXED design-space camera
        // (1 world unit = 1 design pixel) so the whole view - sprites, UI and
        // the camera frame - zooms together via canvasZoom_; the editor's own
        // camera never leaks into the play.
        // The scene's Camera3D object is the runtime view; build the fallback
        // camera from it too so the game renders the SCENE camera's framing
        // (ortho 720-height) instead of the tiny preview default.
        const gfx::Camera gameCam = PlayCamera();
        {
            // THE GAME AREA = the camera's view (its configured aspect,
            // default 16:9), letterboxed inside the dock: world pass AND UI
            // pass share this one mapping - world, HUD and input are one
            // space, clicks line up by construction. The modern box UI
            // adapts WITHIN the game area (whose width follows the aspect).
            const float gameAspect = PlayCameraAspect();
            DockViewportScope dock(*this, /*designFit=*/true, /*sceneVp=*/true, gameAspect);
            ApplySceneEnvironment();
            if (dock.Active()) {
                play_->Draw(renderer_, gameCam, 1.0f);
                renderer_.EndScene();
                // G5-4-4: composite the scene, then flush the on_render HUD
                // canvas on top (authored colors, not tone-mapped) through
                // the SAME mapping.
                play_->FlushCanvas(renderer_);
            } else {
                // No viewport rect yet (first frame): full-window fallback.
                play_->Draw(renderer_, gameCam, 1.0f);
                renderer_.EndScene();
                play_->FlushCanvas(renderer_);
            }
        }
    } else {
        // Edit mode: the scene FILLS the whole dock (free camera - orbit in
        // 3D, pan/zoom in 2D); the camera's view is only a preview frame.
        // Play (2D or 3D): the GAME AREA = the scene camera's view (its
        // configured aspect), letterboxed inside the dock.
        const bool gameArea = playActive_;
        DockViewportScope dock(*this, /*designFit=*/gameArea, /*sceneVp=*/true,
                               PlayCameraAspect());
        // Day sky + scene lights: shared with the 2D play so edit and
        // Play render the same environment (see ApplySceneEnvironment).
        ApplySceneEnvironment();

        const float aspect = ViewportAspect();
        cam = ActiveCamera();
        renderer_.SetCamera(cam, aspect);
        // SetCamera's shadow pass restores the scene viewport itself (see
        // Renderer::SetCamera); DockViewportScope set it before, so nothing
        // to re-apply here.
        // P2-editor UX: world-space grid overlay (toggle in the viewport
        // toolbar). Drawn into the scene so it matches the active camera.
        if (showViewportGrid_) {
            const float half = 30.0f;
            const gfx::Color gridC{0.42f, 0.48f, 0.58f, 0.35f};
            std::vector<gfx::Renderer::LineVertex> gv;
            for (float x = -half; x <= half; x += 1.0f) {
                gv.push_back({{x, 0.0f, -half}, gridC});
                gv.push_back({{x, 0.0f, half}, gridC});
            }
            for (float z = -half; z <= half; z += 1.0f) {
                gv.push_back({{-half, 0.0f, z}, gridC});
                gv.push_back({{half, 0.0f, z}, gridC});
            }
            renderer_.DrawLines(gv.data(), static_cast<uint32_t>(gv.size()),
                                math::Mat4::Identity());
        }
        // The scene's DirectionalLight object drives the world light (Unity-style).
        // (Handled inside ApplySceneEnvironment, together with the sky/fog.)
        // G-camera: the camera object's frustum gizmo already shows the real
        // view (and follows the camera), so the fixed design/camera border is
        // no longer needed - the game is driven by the camera's actual view.

        if (playActive_ && play_) {
            // Play mode: the viewport renders the runtime's world (a snapshot
            // of the scene taken at Play). The editor scene is untouched.
            // For a 2D project the runtime content must be framed by the same
            // 1280x720 design space the edit-mode camera frame draws, NOT the
            // editor's free pan/zoom camera (which would land the game off the
            // frame). Lock the play camera to the design rect instead.
            gfx::Camera playCam = cam;
            if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) {
                // Unity-style: the scene's Camera3D object defines the runtime
                // camera (position/orientation/ortho size). Fall back to a
                // locked design-space rect only when the scene has no camera.
                playCam = PlayCamera();
            }
            play_->Draw(renderer_, playCam);
            // Physics debug view: wireframe every collider so the collision
            // shapes are visible while playing (dynamic = cyan, static =
            // gray). Uses the physics world's live positions, so resting and
            // bouncing bodies show exactly where the simulation puts them.
            // G8-3: physics wireframe is one debug-overlay layer (on by default).
            if (debugColliders_) {
                for (const physics::World::DebugBody& db :
                     play_->PhysicsWorld().DebugBodies()) {
                    const gfx::Color c = db.dynamic ? gfx::Color{0.2f, 0.9f, 1.0f, 0.9f}
                                                    : gfx::Color{0.55f, 0.62f, 0.7f, 0.85f};
                    if (db.kind == physics::World::ShapeKind::Sphere)
                        renderer_.DrawSphere(db.pos, db.radius, c);
                    else
                        renderer_.DrawBox({db.pos - db.halfExtents, db.pos + db.halfExtents}, c);
                }
            }
        } else {
            // P2-3: sprites draw back-to-front by zOrder (stable sort keeps the
            // scene-tree order for everything else).
            std::vector<size_t> drawOrder(entities_.size());
            for (size_t i = 0; i < drawOrder.size(); ++i) drawOrder[i] = i;
            std::stable_sort(drawOrder.begin(), drawOrder.end(),
                             [&](size_t a, size_t b) {
                                 return entities_[a].zOrder < entities_[b].zOrder;
                             });
            for (size_t di : drawOrder) {
                SceneEntity& e = entities_[di];
                if (e.meshKey == "tilemap") {
                    // P1-1: draw every non-empty cell as a sprite quad; the
                    // entity's scale sets the cell size.
                    const size_t cellCount =
                        static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_;
                    if (!e.tilemapTiles_.empty() && e.tilemapTiles_.size() == cellCount &&
                        e.spriteMesh.Valid()) {
                        for (int r = 0; r < e.tilemapRows_; ++r) {
                            for (int c = 0; c < e.tilemapCols_; ++c) {
                                const std::string& tex =
                                    e.tilemapTiles_[static_cast<size_t>(r) * e.tilemapCols_ + c];
                                if (tex.empty()) continue;
                                gfx::Material mat = e.spriteMaterial;
                                mat.albedo = assetMgr_.LoadTexture(tex).Handle();
                                mat.tint = gfx::Color::White;
                                const math::Vec3 offset{
                                    (static_cast<float>(c) + 0.5f) * e.scale.x,
                                    (static_cast<float>(r) + 0.5f) * e.scale.y, 0.0f};
                                math::Mat4 cellModel =
                                    math::Mat4::Translation(e.pos + e.rot.Rotate(offset)) *
                                    e.rot.ToMat4() *
                                    math::Mat4::Scale(e.scale);
                                renderer_.DrawMesh(e.spriteMesh, mat, cellModel);
                            }
                        }
                    }
                    continue;
                }
                if (!e.decalTex.empty()) {
                    gfx::Material mat = e.spriteMaterial;
                    mat.albedo = assetMgr_.LoadTexture(e.decalTex).Handle();
                    mat.transparent = true;
                    mat.tint = {1, 1, 1, e.decalAlpha};
                    if (!e.decalMesh.Valid())
                        e.decalMesh =
                            gfx::Mesh::CreatePlane(renderer_, e.decalSize, e.decalSize, 1, 1,
                                                   "decal");
                    math::Mat4 decalModel =
                        math::Mat4::Translation(e.pos + math::Vec3{0, 0.02f, 0}) *
                        e.rot.ToMat4();
                    renderer_.DrawMesh(e.decalMesh, mat, decalModel);
                    continue;
                }
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (!e.spriteTex.empty() && e.spriteMesh.Valid()) {
                    // 2D sprite: image quad; flips mirror it around its center
                    // via a negative local scale (keeps the texture upright).
                    if (e.spriteFlipX || e.spriteFlipY)
                        model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                           e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                    renderer_.DrawMesh(e.spriteMesh, e.spriteMaterial, model);
                } else if (e.skinned && e.skinned->Valid()) {
                    const std::vector<math::Mat4> bones = e.skinned->BoneMatrices();
                    for (const auto& part : e.skinned->parts)
                        renderer_.DrawSkinnedMesh(part.mesh, part.material,
                                                  model * part.localTransform, bones,
                                                  static_cast<int>(bones.size()));
                } else {
                    renderer_.DrawMesh(e.mesh, e.material, model);
                }
            }
            static bool dbg = false;
            if (!dbg && smokeMode_) {
                dbg = true;
                NEON_LOG_INFO("EDITOR-DRAW: entities=%zu drawCalls=%u", entities_.size(),
                              renderer_.Stats().drawCalls);
            }
            // P2-editor UX: terrain brush footprint preview at the hover point.
            if (terrainPaintMode_ && terrainHoverValid_) {
                const gfx::Color ringC{0.95f, 0.65f, 0.2f, 0.9f};
                const int segs = 36;
                const float y = entities_[static_cast<size_t>(selected_)].pos.y + 0.05f;
                std::vector<gfx::Renderer::LineVertex> rv;
                rv.reserve(static_cast<size_t>(segs) * 2);
                for (int i = 0; i < segs; ++i) {
                    const float a0 = static_cast<float>(i) / segs * 6.2831853f;
                    const float a1 = static_cast<float>(i + 1) / segs * 6.2831853f;
                    rv.push_back({{terrainHoverPos_.x + std::cos(a0) * terrainBrushRadius_, y,
                                   terrainHoverPos_.z + std::sin(a0) * terrainBrushRadius_},
                                  ringC});
                    rv.push_back({{terrainHoverPos_.x + std::cos(a1) * terrainBrushRadius_, y,
                                   terrainHoverPos_.z + std::sin(a1) * terrainBrushRadius_},
                                  ringC});
                }
                renderer_.DrawLines(rv.data(), static_cast<uint32_t>(rv.size()),
                                    math::Mat4::Identity());
            }
            if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
                const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (e.spriteFlipX || e.spriteFlipY)
                    model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                       e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                const gfx::Mesh& pickMesh = e.spriteMesh.Valid() ? e.spriteMesh : e.mesh;
                math::AABB world = math::TransformAABB(pickMesh.Bounds(), model);
                renderer_.DrawBox(world, gfx::Color{0.3f, 0.8f, 1.0f, 1.0f});
            }
        }

        // Edit mode: the camera-frame preview (2D: the flat z=0 view rect -
        // exactly the play game area; 3D front/top: the ortho view rect).
        if (!playActive_ && (projectMode_ == "2d" || editMode_ == EditMode::Scene2D ||
                             viewCam_ != ViewCam::Perspective))
            DrawCameraFrame();

        // G8-3: debug overlay layers (nav walkable area, light probes, audio).
        // Drawn while the scene viewport is STILL active so screen-space lines
        // (DrawLines) rasterise inside the dock rect and line up with the ImGui
        // transform gizmo. Previously this ran after ResetSceneViewport() and
        // therefore drew at full-window viewport -> a vertical offset.
        DrawDebugOverlay(cam);

        // DockViewportScope (declared at the top of this branch) flushes the
        // scene's 2D overlay under the scissor, then resets the scene
        // viewport / scissor / 2D mapping for the composite + tool UI.
    }

    // End the 3D scene phase: composite the HDR frame to the backbuffer and
    // bind the backbuffer so the tool UI (engine UI demo + ImGui) below renders
    // crisp and unbloomed on top.
    renderer_.EndScene();

    // Play overlays (script on_render HUD / UI document / fallback HUD) all
    // draw in DESIGN space on top of the composite, clipped to the dock.
    // ONE mapping for every mode: the world layer above picked the raster
    // shape (2D: 16:9 fit-within, 3D: fill the dock), but every overlay is
    // the same 1280x720 canvas, so it always runs through the same
    // design-fit mapping - no full-window or pixel-passthrough side paths.
    if (playActive_ && play_) {
        DockViewportScope dock(*this, /*designFit=*/true, /*sceneVp=*/false,
                               PlayCameraAspect());
        // The 2D play branch already flushed its canvas inside its own scope
        // (same mapping); flushing twice would double every HUD element.
        if (projectMode_ != "2d") play_->FlushCanvas(renderer_);
        play_->DrawUI(renderer_);
        // Built-in HUD only as a fallback: a data-driven game that defines
        // on_render draws its own HUD on the 2D canvas above.
        if (!play_->HasScriptFunction("on_render")) DrawPlayHUD();
        if (!dock.Active()) renderer_.Flush2D(); // full-window fallback
    }

    // Scene pass draw calls (before the thumbnail pass adds its own counts).
    if (smokeMode_) {
        lastRenderTick_ = TimeRef().frameIndex;
        smokeDrawCalls_ = renderer_.Stats().drawCalls;
        lastRenderCamOrtho_ = cam.ortho;
    }

    // Asset thumbnails: meshes selected in the asset panel render into small
    // offscreen targets here, after the scene is composited, so the ImGui pass
    // below can sample them on the next frame. Flush any pending 2D first: on a
    // non-HDR driver EndScene is a no-op and the sky's quads must reach the
    // backbuffer, not the thumbnail target.
    renderer_.Flush2D();
    GenerateMeshThumbnails();
    GenerateMaterialThumbnails();

    renderer_.Flush2D();
    gfx::ImGuiNeon_RenderDrawData(ImGui::GetDrawData());

    // Model viewer renders into its own docked panel (not the main viewport),
    // so it coexists with the edit/play scene.
    RenderModelPreviewPanel();

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

bool EditorApp::MouseOverToolPanel() {
    // Position-based (this runs before ImGui::NewFrame, so hover flags are
    // stale): the click belongs to a panel - and never to a viewport picker -
    // when the mouse is inside ANY visible docked leaf window except the
    // viewport itself.
    ImGuiContext& ictx = *ImGui::GetCurrentContext();
    const math::Vec2 mousePx = Input()->MousePos();
    for (int wi = 0; wi < ictx.Windows.Size; ++wi) {
        ImGuiWindow* w = ictx.Windows[wi];
        if (!w || w->Hidden) continue;
        // A CLOSED panel keeps its stale rect in g.Windows (ImGui never
        // destroys it), so without this check the old area would keep
        // hijacking the viewport camera even after the panel is closed.
        // Active is reset every NewFrame for windows not submitted that frame.
        if (!w->Active) continue;
        if (w->DockNodeAsHost != nullptr) continue; // dock host / tab bar
        if (w->ParentWindow != nullptr) continue;   // child windows
        if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue; // overlays (gizmo)
        if (std::strcmp(w->Name, "视口") == 0) continue; // the 3D viewport
        if (std::strncmp(w->Name, "##", 2) == 0) continue; // internal windows
        if (mousePx.x >= w->Pos.x && mousePx.x <= w->Pos.x + w->Size.x &&
            mousePx.y >= w->Pos.y && mousePx.y <= w->Pos.y + w->Size.y)
            return true;
    }
    return false;
}

void EditorApp::UpdateUIEditorViewport() {
    const math::Rect2& vp = viewportScreenRect_;
    if (vp.w <= 0.0f || vp.h <= 0.0f) return;
    platform::IInput* input = Input();
    if (!input) return;
    // Clicks/wheel over a tool panel (tree, inspector fields) belong to the
    // panel, never to the canvas. Without this gate, clicking a DragFloat in
    // the inspector ALSO ran the canvas hit-test below, which usually missed
    // and cleared the selection - the "edit a field -> selection lost" bug.
    const bool overPanel = MouseOverToolPanel();
    const math::Vec2 mousePx0 = input->MousePos();
    const bool inViewport =
        mousePx0.x >= vp.x && mousePx0.x <= vp.x + vp.w &&
        mousePx0.y >= vp.y && mousePx0.y <= vp.y + vp.h;
    if (overPanel || !inViewport) return;
    // Same design-space mapping as the render path (fit + canvas zoom/pan):
    // ScreenToUI below then reports document coordinates for both picking
    // and the drag delta, and the two can never disagree again.
    BindDock2DMapping(/*designFit=*/true);
    const math::Vec2 mouse = renderer_.ScreenToUI(input->MousePos());

    // The UI document is a design-space canvas like the 2D view: wheel zooms
    // the whole document, middle-drag pans it (shared canvas zoom/pan state).
    const float wheel = input->WheelDelta();
    if (std::fabs(wheel) > 0.01f) {
        canvasZoom_ = math::Clamp(canvasZoom_ * std::pow(1.15f, -wheel), 0.2f, 8.0f);
        input->ConsumeWheel();
    }
    if (input->MouseDown(platform::MouseButton::Middle)) {
        const float scale = renderer_.UIScale() > 0.0f ? renderer_.UIScale() : 1.0f;
        canvasPan_ -= input->MouseDelta() / scale;
    }

    auto cornerAt = [](const math::Rect2& r, const math::Vec2& p) {
        const float k = 10.0f;
        const math::Vec2 corners[4] = {
            {r.x, r.y}, {r.x + r.w, r.y}, {r.x, r.y + r.h}, {r.x + r.w, r.y + r.h}};
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(corners[i].x - p.x) <= k && std::fabs(corners[i].y - p.y) <= k)
                return i;
        }
        return -1;
    };

    if (input->MousePressed(platform::MouseButton::Left)) {
        uiDragging_ = false;
        uiResizeHandle_ = -1;
        const math::Rect2 selRect =
            uiSelected_ ? uiSelected_->AbsoluteRect() : math::Rect2{};
        if (uiSelected_ && cornerAt(selRect, mouse) >= 0) {
            uiResizeHandle_ = cornerAt(selRect, mouse);
            uiDragging_ = true;
            return;
        }
        ui::UiNode* hit = uiDoc_.HitTest(mouse);
        // P5-editor UX: Ctrl+click toggles multi-selection.
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (hit && hit != &uiDoc_.root) {
            if (ctrl)
                UIToggleSelectNode(hit);
            else
                UISelectNode(hit);
        } else if (!ctrl) {
            UISelectNode(nullptr);
        }
        uiDragging_ = uiSelected_ != nullptr;
        return;
    }

    if (input->MouseDown(platform::MouseButton::Left) && uiDragging_ && uiSelected_) {
        const math::Vec2 delta = input->MouseDelta() / renderer_.UIScale();
        math::Rect2& r = uiSelected_->rect;
        if (uiResizeHandle_ >= 0) {
            switch (uiResizeHandle_) {
                case 0: r.x += delta.x; r.y += delta.y; r.w -= delta.x; r.h -= delta.y; break;
                case 1: r.w += delta.x; r.y += delta.y; r.h -= delta.y; break;
                case 2: r.x += delta.x; r.w -= delta.x; r.h += delta.y; break;
                default: r.w += delta.x; r.h += delta.y; break;
            }
            // P5-editor UX: grid snap while resizing.
            r.x = UISnap(r.x);
            r.y = UISnap(r.y);
            r.w = std::max(UISnap(r.w), 4.0f);
            r.h = std::max(UISnap(r.h), 4.0f);
            r.w = std::max(r.w, 8.0f);
            r.h = std::max(r.h, 8.0f);
        } else {
            r.x = UISnap(r.x + delta.x);
            r.y = UISnap(r.y + delta.y);
        }
        MarkUIDirty();
        return;
    }

    if (input->MouseReleased(platform::MouseButton::Left)) {
        uiDragging_ = false;
        uiResizeHandle_ = -1;
    }

    // P5-editor UX shortcuts: Delete 删除选中, Ctrl+D 复制, 方向键 微调.
    if (!gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (input->Pressed(platform::Key::Delete)) UIDeleteSelectedNodes();
        if (input->Pressed(platform::Key::D) && ImGui::GetIO().KeyCtrl)
            UIDuplicateSelectedNodes();
        const float nudge = uiGridSize_;
        math::Vec2 dir{};
        if (input->Pressed(platform::Key::ArrowLeft)) dir.x = -nudge;
        if (input->Pressed(platform::Key::ArrowRight)) dir.x = nudge;
        if (input->Pressed(platform::Key::ArrowUp)) dir.y = -nudge;
        if (input->Pressed(platform::Key::ArrowDown)) dir.y = nudge;
        if (dir.x != 0.0f || dir.y != 0.0f) {
            for (ui::UiNode* n : uiSelection_) {
                if (!n) continue;
                n->rect.x += dir.x;
                n->rect.y += dir.y;
            }
            MarkUIDirty();
        }
    }
}

void EditorApp::MarkUIDirty() {
    uiDirty_ = true;
    if (!uiDocOpen_ || uiDocPath_.empty()) return;
    // "untitled" documents have no real path yet; the explicit 保存 button
    // assigns one. Everything else auto-saves on every edit so closing the
    // panel or restarting the editor never loses changes.
    const bool isUntitled =
        uiDocPath_.size() >= 15 &&
        uiDocPath_.compare(uiDocPath_.size() - 15, 15, "untitled.ui.json") == 0;
    if (isUntitled) return;
    if (uiDoc_.Save(uiDocPath_)) uiDirty_ = false;
}

gfx::Camera EditorApp::ActiveCamera() const {
    // P1-1 camera entity preview: when enabled and a Camera3D entity is
    // selected, the viewport shows through it (transform + fov/ortho).
    if (cameraFollowSelected_ && selected_ >= 0 &&
        selected_ < static_cast<int>(entities_.size())) {
        const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        if (e.nodeType == "Camera3D") {
            gfx::Camera cam;
            cam.position = e.pos;
            cam.target = e.pos + e.rot.Rotate({0, 0, -1});
            cam.up = {0, 1, 0};
            cam.ortho = e.cameraOrtho;
            cam.orthoSize = e.cameraOrthoSize;
            cam.fovY = e.cameraFov * math::kDegToRad;
            return cam;
        }
    }
    gfx::Camera cam;
    switch (viewCam_) {
        case ViewCam::Top: // 顶视: orthographic looking down -Y
            cam.position = camTarget_ + math::Vec3{0, camDist_, 0};
            cam.target = camTarget_;
            cam.up = {0, 0, -1};
            cam.ortho = true;
            cam.orthoSize = orthoSize_;
            break;
        case ViewCam::Front: // 前视: orthographic looking down -Z
            cam.position = camTarget_ + math::Vec3{0, 0, camDist_};
            cam.target = camTarget_;
            cam.up = {0, 1, 0};
            cam.ortho = true;
            cam.orthoSize = orthoSize_;
            break;
        case ViewCam::Perspective:
        default:
            cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                                   std::sin(pitch_),
                                                   std::cos(yaw_) * std::cos(pitch_)} *
                                            camDist_;
            cam.target = camTarget_;
            break;
    }
    return cam;
}

void EditorApp::ApplySceneEnvironment() {
    // Day sky: the old near-black zenith made the IBL ambient ~0, so
    // backfaces and shadowed areas were crushed to black (roofs looked
    // incomplete, shadows harsh). A bright sky keeps shadows readable.
    renderer_.SetSky({0.28f, 0.38f, 0.58f, 1.0f}, {0.55f, 0.65f, 0.8f, 1.0f});
    const bool is2DFog = projectMode_ == "2d" || editMode_ == EditMode::Scene2D;
    if (is2DFog) {
        // 2D has no depth: the ortho camera sits at z=100 so a distance
        // fog would paint a radial gradient centred on the camera axis
        // (bright/dim circle at the camera position). Disable it by pushing
        // the fog range far beyond any visible object.
        renderer_.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 1e9f, 1e10f);
    } else {
        renderer_.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 60.0f, 140.0f);
    }
    renderer_.DrawSky();
    // The scene's DirectionalLight object drives the world light (Unity-style).
    const scene::SceneLight* sl = nullptr;
    const scene::SceneLight* amb = nullptr;
    for (const SceneEntity& se : entities_) {
        if (se.hasLight && se.light.type == "directional" && !sl) sl = &se.light;
        if (se.hasLight && se.light.type == "ambient" && !amb) amb = &se.light;
    }
    if (sl) {
        const gfx::Color sun{sl->color.r * sl->intensity, sl->color.g * sl->intensity,
                             sl->color.b * sl->intensity, sl->color.a};
        renderer_.SetDirectionalLight(sl->sunDir, sun, 0.0f);
    } else {
        // No directional light object: a dim default so lighting visibly
        // responds to the scene's light objects (no light -> dark-ish).
        renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {0.8f, 0.8f, 0.8f}, 0.0f);
    }
    // An explicit ambient light object is authoritative for the flat ambient
    // term (color * strength) and turns off the sky-IBL fill so the object is
    // actually visible. Without one, keep the bright day-sky IBL ambient + a
    // neutral fill for the default look.
    if (amb) {
        renderer_.SetAmbientLight(amb->color, amb->ambientStrength);
        renderer_.SetIblStrength(0.0f);
    } else {
        renderer_.SetAmbientLight({1.0f, 1.0f, 1.0f, 1.0f}, 0.1f);
        renderer_.SetIblStrength(1.0f);
    }
    // Scene PointLight objects (Unity-style) drive the renderer's point lights.
    int plIndex = 0;
    for (const SceneEntity& se : entities_) {
        if (!se.hasLight || se.light.type != "point") continue;
        if (plIndex >= gfx::Renderer::kMaxPointLights) break;
        const gfx::Color pc{se.light.color.r * se.light.intensity,
                            se.light.color.g * se.light.intensity,
                            se.light.color.b * se.light.intensity, se.light.color.a};
        renderer_.SetPointLight(plIndex++, se.pos, pc, se.light.radius);
    }
    for (; plIndex < gfx::Renderer::kMaxPointLights; ++plIndex)
        renderer_.SetPointLight(plIndex, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
}

void EditorApp::DrawCameraFrame() {
    // The border marks the ACTUAL runtime view - the scene camera's framing
    // (its orthoSize + configured aspect, on the camera's position). What the
    // frame encloses is exactly what running the game shows.
    const float z = 0.0f; // sprite content plane
    if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) {
        const SceneEntity* sc = nullptr;
        for (const SceneEntity& se : entities_) {
            if (se.nodeType == "Camera3D") { sc = &se; break; }
        }
        const float halfH = (sc && sc->cameraOrthoSize > 0.0f) ? sc->cameraOrthoSize : 360.0f;
        const float aspect = PlayCameraAspect();
        const float halfW = halfH * aspect;
        const float cx = sc ? sc->pos.x : 640.0f;
        const float cy = sc ? sc->pos.y : 360.0f;
        const gfx::Renderer::LineVertex verts[8] = {
            {{cx - halfW, cy - halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx + halfW, cy - halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx + halfW, cy - halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx + halfW, cy + halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx + halfW, cy + halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx - halfW, cy + halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx - halfW, cy + halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{cx - halfW, cy - halfH, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        };
        renderer_.DrawLines(verts, 8, math::Mat4::Identity());
        return;
    }
    const float halfH = orthoSize_;
    const float halfW = halfH * ViewportAspect();
    const math::Vec3 c = camTarget_;
    const gfx::Renderer::LineVertex verts[8] = {
        {{c.x - halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
    };
    renderer_.DrawLines(verts, 8, math::Mat4::Identity());
}

void EditorApp::DrawSceneGizmos() {
    if (playActive_) return; // gizmos are an edit-mode planning aid
    // Draw through ImGui (inside the frame) using the SAME view/projection/rect
    // as the transform gizmo, so the frusta/icons line up with the gizmo.
    // The camera FRUSTUM preview uses the PLAY aspect (16:9 game area): what
    // the gizmo shows is exactly what running the game will frame.
    const gfx::Camera cam = ActiveCamera();
    const float aspect = static_cast<float>(gfx::Renderer::kDesignWidth) /
                         static_cast<float>(gfx::Renderer::kDesignHeight);
    const math::Mat4 view = cam.View();
    const math::Mat4 proj = cam.Projection(aspect);
    const math::Rect2 vp = SceneRect();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl || vp.w <= 0.0f || vp.h <= 0.0f) return;

    auto screen = [&](const math::Vec3& w, ImVec2& out) -> bool {
        const math::Vec4 clip =
            proj.TransformVec4(view.TransformVec4(math::Vec4(w.x, w.y, w.z, 1.0f)));
        if (clip.w <= 0.0f) return false;
        const float nx = clip.x / clip.w, ny = clip.y / clip.w;
        out.x = vp.x + (nx * 0.5f + 0.5f) * vp.w;
        out.y = vp.y + (0.5f - ny * 0.5f) * vp.h;
        return true;
    };
    auto line = [&](const math::Vec3& a, const math::Vec3& b, const gfx::Color& c) {
        ImVec2 pa, pb;
        if (screen(a, pa) && screen(b, pb))
            dl->AddLine(pa, pb, IM_COL32(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                                        static_cast<int>(c.b * 255), 235),
                        1.5f);
    };

    const gfx::Color camCol{0.4f, 0.9f, 1.0f, 0.95f};
    const gfx::Color dirCol{1.0f, 0.9f, 0.4f, 0.95f};
    const gfx::Color ptCol{1.0f, 0.65f, 0.25f, 0.95f};
    const gfx::Color ambCol{0.7f, 0.9f, 1.0f, 0.95f};

    for (const SceneEntity& e : entities_) {
        const math::Mat4 model =
            math::Mat4::Translation(e.pos) * e.rot.ToMat4() * math::Mat4::Scale(e.scale);
        if (e.nodeType == "Camera3D") {
            // 2D edit shows the flat camera-frame rect instead (see
            // DrawCameraFrame) - the 3D frustum box's depth lines are noise
            // on a flat canvas.
            if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) continue;
            const float dAspect = static_cast<float>(gfx::Renderer::kDesignWidth) /
                                  static_cast<float>(gfx::Renderer::kDesignHeight);
            const float nearP = 0.1f, farP = 60.0f;
            math::Vec3 c[8];
            if (e.cameraOrtho) {
                const float hh = e.cameraOrthoSize, hw = hh * dAspect;
                c[0] = {-hw, hh, -nearP}; c[1] = {hw, hh, -nearP};
                c[2] = {hw, -hh, -nearP}; c[3] = {-hw, -hh, -nearP};
                c[4] = {-hw, hh, -farP};  c[5] = {hw, hh, -farP};
                c[6] = {hw, -hh, -farP};  c[7] = {-hw, -hh, -farP};
            } else {
                const float t = std::tan(e.cameraFov * 0.5f * math::kDegToRad);
                const float nh = t * nearP, nw = nh * dAspect;
                const float fh = t * farP, fw = fh * dAspect;
                c[0] = {-nw, nh, -nearP}; c[1] = {nw, nh, -nearP};
                c[2] = {nw, -nh, -nearP}; c[3] = {-nw, -nh, -nearP};
                c[4] = {-fw, fh, -farP};  c[5] = {fw, fh, -farP};
                c[6] = {fw, -fh, -farP};  c[7] = {-fw, -fh, -farP};
            }
            const int q0[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
            const int q1[4][2] = {{4, 5}, {5, 6}, {6, 7}, {7, 4}};
            for (auto& ed : q0) line(model.TransformPoint(c[ed[0]]), model.TransformPoint(c[ed[1]]), camCol);
            for (auto& ed : q1) line(model.TransformPoint(c[ed[0]]), model.TransformPoint(c[ed[1]]), camCol);
            for (int i = 0; i < 4; ++i) line(model.TransformPoint(c[i]), model.TransformPoint(c[4 + i]), camCol);
            line(model.TransformPoint({-0.3f, 0, 0}), model.TransformPoint({0.3f, 0, 0}), camCol);
            line(model.TransformPoint({0, -0.3f, 0}), model.TransformPoint({0, 0.3f, 0}), camCol);
            line(model.TransformPoint({0, 0, -0.3f}), model.TransformPoint({0, 0, 0.3f}), camCol);
        } else if (e.hasLight) {
            const gfx::Color col = e.light.type == "point"
                                       ? ptCol
                                       : (e.light.type == "ambient" ? ambCol : dirCol);
            if (e.light.type == "directional") {
                const math::Vec3 dir = e.light.sunDir.Normalized();
                const math::Vec3 tip = e.pos + dir * 2.0f;
                math::Vec3 s = math::Cross(dir, math::Vec3::Up());
                if (s.LengthSq() < 1e-4f) s = {1.0f, 0.0f, 0.0f};
                s = s.Normalized() * 0.25f;
                line(e.pos, tip, col);
                line(tip - dir * 0.5f - s, tip, col);
                line(tip - dir * 0.5f + s, tip, col);
            } else if (e.light.type == "point") {
                const int k = 12;
                for (int i = 0; i < k; ++i) {
                    const float a0 = static_cast<float>(i) / k * math::kTwoPi;
                    const float a1 = static_cast<float>(i + 1) / k * math::kTwoPi;
                    line(e.pos + math::Vec3{std::cos(a0) * e.light.radius, 0, std::sin(a0) * e.light.radius},
                         e.pos + math::Vec3{std::cos(a1) * e.light.radius, 0, std::sin(a1) * e.light.radius}, ptCol);
                }
            } else {
                const int k = 12;
                for (int i = 0; i < k; ++i) {
                    const float a0 = static_cast<float>(i) / k * math::kTwoPi;
                    const float a1 = static_cast<float>(i + 1) / k * math::kTwoPi;
                    line(e.pos + math::Vec3{std::cos(a0) * 0.3f, 0, std::sin(a0) * 0.3f},
                         e.pos + math::Vec3{std::cos(a1) * 0.3f, 0, std::sin(a1) * 0.3f}, ambCol);
                }
            }
        }
    }
}

void EditorApp::UpdateViewport(float dt) {
    // Model preview camera: only when the mouse is actually over the preview
    // panel; otherwise this falls through and the main viewport camera keeps
    // driving (right-drag in the 3D view must not rotate the preview).
    if (showModelPreview_ && previewModel_ && previewScreenRect_.w > 0.0f) {
        platform::IInput* in = Input();
        const math::Vec2 mpx = in->MousePos();
        const bool overPreview =
            mpx.x >= previewScreenRect_.x && mpx.x <= previewScreenRect_.x + previewScreenRect_.w &&
            mpx.y >= previewScreenRect_.y && mpx.y <= previewScreenRect_.y + previewScreenRect_.h;
        if (overPreview) {
            if (in->MouseDown(platform::MouseButton::Right)) {
                previewYaw_ += -in->MouseDelta().x * 0.005f;
                previewPitch_ = math::Clamp(previewPitch_ + -in->MouseDelta().y * 0.005f,
                                            -1.4f, 1.4f);
            }
            return;
        }
    }
    platform::IInput* input = Input();
    math::Vec2 mp = renderer_.ScreenToUI(input->MousePos());
    // ImGui tool windows capture mouse when hovered/active; the 3D viewport
    // area itself has no ImGui window, so camera controls stay responsive.
    // See MouseOverToolPanel() for why this is position-based.
    const bool overPanel = MouseOverToolPanel();
    bool inViewport = mp.x >= viewportRect_.x && mp.x <= viewportRect_.x + viewportRect_.w &&
                      mp.y >= viewportRect_.y && mp.y <= viewportRect_.y + viewportRect_.h;

    // 2D view: until the user zooms/pans, keep the camera framed so the
    // 1280x720 design space maps 1:1 onto the viewport (ortho half-height =
    // half the viewport height). This keeps edit view and play identical.
    if (editMode_ == EditMode::Scene2D && viewCam_ == ViewCam::Front &&
        !cameraUserAdjusted_) {
        // Frame 2D like the runtime's SceneCamera does: centre on the design
        // centre and use the camera object's ortho size (fallback to the
        // 1280x720 design half-height). The old default target stayed at the 3D
        // origin and orthoSize = panelHeight/2, so edit and play disagreed
        // whenever the dock wasn't exactly 720px tall.
        const SceneEntity* sc = nullptr;
        for (const SceneEntity& se : entities_) {
            if (se.nodeType == "Camera3D" && !sc) { sc = &se; break; }
        }
        camTarget_ = sc ? math::Vec3{sc->pos.x, sc->pos.y, 0.0f}
                        : math::Vec3{640.0f, 360.0f, 0.0f};
        orthoSize_ = (sc && sc->cameraOrthoSize > 0.0f) ? sc->cameraOrthoSize : 360.0f;
    }

    // 2D play: fixed whole-view framing - the full 1280x720 design space is
    // always fit into the dock. Camera zoom/pan is DISABLED while playing
    // (games like PvZ have no free camera; a stray wheel would crop the UI).
    if (playActive_ && editMode_ == EditMode::Scene2D) {
        if (std::fabs(input->WheelDelta()) > 0.01f) input->ConsumeWheel();
        return;
    }

    const bool ortho = viewCam_ != ViewCam::Perspective;
    if (!overPanel && inViewport) {
        // While the transform gizmo is hovered or being dragged the mouse
        // belongs to it: camera orbit/pan and left-click picking must not run.
        // UpdateViewport runs before the gizmo's Manipulate() each frame, so
        // gizmoDragActive_/IsOver() report the previous frame's gizmo state.
        const bool gizmoBusy =
            selected_ >= 0 && (gizmoDragActive_ || ImGuizmo::IsOver());
        if (!gizmoBusy) {
            if (!ortho && input->MouseDown(platform::MouseButton::Right)) {
                yaw_ += -input->MouseDelta().x * 0.005f;
                pitch_ = math::Clamp(pitch_ + input->MouseDelta().y * 0.005f, 0.05f, 1.4f);
            }
            if (input->MouseDown(platform::MouseButton::Middle)) {
                // Pan in the ACTIVE camera's plane (the perspective orbit or a
                // static ortho view): middle-drag moves the target along the
                // camera's right/up axes.
                gfx::Camera cam = ActiveCamera();
                math::Vec3 fwd = (cam.target - cam.position).Normalized();
                math::Vec3 right = math::Cross(fwd, cam.up).Normalized();
                math::Vec3 upv = math::Cross(right, fwd);
                const float worldPerPixel =
                    ortho ? orthoSize_ * 2.0f / ValidSceneRect().h : 1.0f;
                const float k = ortho ? worldPerPixel : 0.02f;
                camTarget_ -= right * input->MouseDelta().x * k;
                camTarget_ += upv * input->MouseDelta().y * k;
                cameraUserAdjusted_ = true;
            }
            float wheel = input->WheelDelta();
            if (std::fabs(wheel) > 0.01f) {
                if (ortho) {
                    // Proportional zoom: each wheel notch scales the view by
                    // 1.15x. (Absolute deltas were useless at the 2D design
                    // space's ~360-unit starting size.) Range covers far out
                    // (8192) to 360x zoom-in (1) from any starting size.
                    // Wheel up (positive) zooms IN: scale the half-height down.
                    const float factor = std::pow(1.15f, -wheel);
                    orthoSize_ = math::Clamp(orthoSize_ * factor, 1.0f, 8192.0f);
                    cameraUserAdjusted_ = true;
                } else {
                    camDist_ = math::Clamp(camDist_ - wheel * 1.2f, 3.0f, 60.0f);
                }
            }
        }
        // Play mode keeps camera navigation but not scene editing: left-click
        // picking would mutate the editor scene selection mid-play.
        if (input->MousePressed(platform::MouseButton::Left) && !playActive_ &&
            !gizmoBusy) {
            math::Ray ray = PickRay();
            // P1-1 terrain brush: paint instead of picking while enabled.
            if (terrainPaintMode_) {
                PaintTerrain(ray);
                return;
            }
            float best = 1e30f;
            int picked = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& e = entities_[i];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (e.spriteFlipX || e.spriteFlipY)
                    model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                       e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                const gfx::Mesh& pickMesh = e.spriteMesh.Valid() ? e.spriteMesh : e.mesh;
                math::AABB world = math::TransformAABB(pickMesh.Bounds(), model);
                float t = 0.0f;
                if (math::IntersectRayAABB(ray, world, t) && t < best) {
                    best = t;
                    picked = static_cast<int>(i);
                }
            }
            // P2-editor UX: Ctrl+click adds to the selection, plain click
            // replaces it (Shift+click in the hierarchy selects ranges).
            if (ImGui::GetIO().KeyCtrl)
                ToggleSelection(picked);
            else
                SetSelection(picked);
        }
        // Hold-to-paint: the brush applies every frame while the button stays
        // down (drag sculpting).
        if (terrainPaintMode_ && input->MouseDown(platform::MouseButton::Left)) {
            PaintTerrain(PickRay());
        }
        // P2-editor UX: terrain brush hover preview — track where the brush
        // would land every frame while paint mode is on.
        terrainHoverValid_ = false;
        if (terrainPaintMode_ && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()) &&
            entities_[static_cast<size_t>(selected_)].meshKey == "terrain") {
            const math::Ray ray = PickRay();
            if (std::fabs(ray.dir.y) > 1e-6f) {
                const float ty = (entities_[static_cast<size_t>(selected_)].pos.y -
                                  ray.origin.y) /
                                 ray.dir.y;
                if (ty >= 0.0f) {
                    terrainHoverPos_ = ray.origin + ray.dir * ty;
                    terrainHoverValid_ = true;
                }
            }
        }
    }
    // Data-driven play scripts use the orbit yaw for camera-relative
    // movement (same GameVar the neon_game player publishes).
    if (playActive_ && play_)
        play_->GameVars().Set("cameraYaw", script::Value::Num(yaw_));
    (void)dt;
}

void EditorApp::SetSelection(int index) {
    selected_ = index;
    selection_.clear();
    if (index >= 0) selection_.insert(index);
    selectionAnchor_ = index;
    scriptSyncEntity_ = -1; // script panel caches by index: force a re-sync
}

void EditorApp::ToggleSelection(int index) {
    if (index < 0) return;
    const auto it = selection_.find(index);
    if (it != selection_.end()) {
        selection_.erase(it);
        if (selected_ == index) {
            selected_ = selection_.empty() ? -1 : *selection_.rbegin();
        }
    } else {
        selection_.insert(index);
        selected_ = index;
        selectionAnchor_ = index;
    }
    scriptSyncEntity_ = -1;
}

void EditorApp::SelectRangeTo(int index) {
    if (index < 0 || selectionAnchor_ < 0) {
        SetSelection(index);
        return;
    }
    const int lo = std::min(selectionAnchor_, index);
    const int hi = std::max(selectionAnchor_, index);
    selection_.clear();
    for (int i = lo; i <= hi; ++i) {
        if (i >= 0 && i < static_cast<int>(entities_.size())) selection_.insert(i);
    }
    selected_ = index;
    scriptSyncEntity_ = -1;
}

void EditorApp::ClearSelection() {
    selection_.clear();
    selected_ = -1;
    selectionAnchor_ = -1;
    scriptSyncEntity_ = -1;
}

bool EditorApp::IsSelected(int idx) const {
    return selection_.count(idx) != 0;
}

void EditorApp::DrawTransformGizmo() {
    // ImGuizmo::BeginFrame() must run every frame before Manipulate(): it
    // resets mbOverGizmoHotspot (ImGuizmo.cpp:1084) so a handle can re-arm for
    // hover/activation each frame, and snapshots the last frame's hover for
    // IsOver(). Without it the activation check `CanActivate() && type !=
    // MT_NONE` can never fire again after the first hover.
    ImGuizmo::BeginFrame();
    gizmoBeginFrame_ = true;

    if (playActive_ || selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
        gizmoDragActive_ = false;
        return;
    }
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];

    // Draw the gizmo into the viewport window's draw list. The viewport is an
    // ordinary input-active docked panel (NoInputs was removed so it can be
    // undocked/re-docked), so ImGui's hover hit-test resolves to the viewport
    // window itself; point ImGuizmo's hover check at it via
    // SetAlternativeWindow.
    ImGuiWindow* viewportWindow = ImGui::GetCurrentWindow();
    ImGuizmo::SetAlternativeWindow(viewportWindow);
    gizmoAltWindowSet_ = viewportWindow != nullptr;

    const float aspect = ViewportAspect();
    gfx::Camera cam = ActiveCamera();
    ImGuizmo::SetOrthographic(cam.ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

    // The 3D scene renders into the design-fit scene rect with the matching
    // aspect, so the gizmo uses the same rect + aspect.
    const math::Rect2 vp = ValidSceneRect();
    const float rx = vp.x;
    const float ry = vp.y;
    const float rw = vp.w;
    const float rh = vp.h;
    ImGuizmo::SetRect(rx, ry, rw, rh);
    gizmoRect_[0] = rx;
    gizmoRect_[1] = ry;
    gizmoRect_[2] = rw;
    gizmoRect_[3] = rh;

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
            if (selection_.size() > 1) {
                // P2-editor UX: drag the whole selection. On the first changed
                // frame capture every selected entity's pre-drag transform,
                // then apply the active entity's delta to the others and push
                // one BatchTransformCommand (frames merge into a single undo).
                if (!gizmoBatchCaptured_) {
                    gizmoBatchIndices_ = SelectedIndices();
                    gizmoBatchFrom_.clear();
                    for (int i : gizmoBatchIndices_) {
                        const SceneEntity& se = entities_[static_cast<size_t>(i)];
                        gizmoBatchFrom_.push_back({se.pos, se.rot, se.scale});
                    }
                    gizmoBatchCaptured_ = true;
                }
                int activePos = 0;
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k)
                    if (gizmoBatchIndices_[k] == selected_) activePos = static_cast<int>(k);
                const Transform3 activeFrom = gizmoBatchFrom_[static_cast<size_t>(activePos)];
                const math::Quat activeInv{-activeFrom.rot.x, -activeFrom.rot.y,
                                           -activeFrom.rot.z, activeFrom.rot.w};
                std::vector<Transform3> fromNow, to;
                for (int i : gizmoBatchIndices_) {
                    const SceneEntity& se = entities_[static_cast<size_t>(i)];
                    fromNow.push_back({se.pos, se.rot, se.scale});
                }
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k) {
                    Transform3 t = fromNow[k];
                    if (static_cast<int>(k) == activePos) {
                        t.pos = pos;
                        t.rot = rot;
                        t.scale = scale;
                    } else {
                        t.pos = t.pos + (pos - activeFrom.pos);
                        const math::Quat rel = (activeInv * rot).Normalized();
                        t.rot = (t.rot * rel).Normalized();
                        t.scale = {t.scale.x * (scale.x / activeFrom.scale.x),
                                   t.scale.y * (scale.y / activeFrom.scale.y),
                                   t.scale.z * (scale.z / activeFrom.scale.z)};
                    }
                    to.push_back(t);
                }
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k) {
                    SceneEntity& se = entities_[static_cast<size_t>(gizmoBatchIndices_[k])];
                    se.pos = to[k].pos;
                    se.rot = to[k].rot;
                    se.scale = to[k].scale;
                }
                history_.Push(std::make_unique<BatchTransformCommand>(
                    &entities_, gizmoBatchIndices_, fromNow, to));
            } else {
                gizmoDragOriginValid_ = true;
                history_.Push(std::make_unique<EditTransformCommand>(
                    &entities_, selected_, e.pos, e.rot, e.scale, pos, rot, scale,
                    EditTransformCommand::kAll));
            }
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
        if (gizmoBatchCaptured_) {
            if (BatchTransformCommand* top =
                    dynamic_cast<BatchTransformCommand*>(history_.TopUndo()))
                top->Seal();
            gizmoBatchCaptured_ = false;
            gizmoBatchIndices_.clear();
            gizmoBatchFrom_.clear();
        }
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

    const float aspect = ViewportAspect();
    gfx::Camera cam = ActiveCamera();
    float view[16], proj[16];
    Mat4ToGizmo(cam.View(), view);
    Mat4ToGizmo(cam.Projection(aspect), proj);

    // Screen position of the entity origin under the viewport rect (the same
    // rect the gizmo now uses), in y-down ImGui pixels.
    math::Vec2 originPx;
    if (!WorldToScreenImGui(cam, aspect, ValidSceneRect(), sel.pos, originPx)) return;
    const float gx = originPx.x;
    const float gy = originPx.y;

    // The viewport is an input-active docked panel: ImGui reports IT as the
    // hovered window over the viewport, and SetAlternativeWindow points the
    // gizmo at it too.
    ImGuiWindow* vpWin = ImGui::FindWindowByName("视口");
    ImGuiWindow* hostWin = vpWin;
    report(hostWin != nullptr, "drag sim resolves the viewport window");
    if (!hostWin) return;

    // The real hover path relies on ImGui reporting the viewport window as
    // hovered when the mouse is over it (OnUpdate parked the mouse on the
    // viewport center for this smoke frame). If it doesn't match the window
    // the gizmo is bound to, SetAlternativeWindow is wrong/removed and the
    // gizmo would be undraggable - fail the smoke here.
    report(ctx.HoveredWindow == hostWin,
           "real hover over the viewport resolves to the viewport window");

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
    ImGuizmo::SetOrthographic(cam.ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    {
        const math::Rect2 vr = ValidSceneRect();
        ImGuizmo::SetRect(vr.x, vr.y, vr.w, vr.h);
    }
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

} // namespace neon::editor

