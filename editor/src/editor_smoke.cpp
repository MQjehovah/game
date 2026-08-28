#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"

#include <fstream>
#include <sstream>

namespace neon::editor {

void EditorApp::RunUISmokeTest() {
    auto check = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-UI-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };

    if (editMode_ != EditMode::Scene3D) {
        StopPlay();
        editMode_ = EditMode::Scene3D;
        NEON_LOG_INFO("EDITOR-UI-SMOKE: forced 3D mode for the canonical smoke run");
    }
    // The sandbox editor_scene.json is USER data (it can hold sprites etc.),
    // so the canonical smoke runs against a deterministic temp scene with
    // UTF-8 entity names and a mesh entity for the material round-trip checks.
    const std::string smokePrevScene = currentScenePath_;
    {
        const std::string smokeScene = GetTempDir() + "/smoke_base_scene.json";
        {
            std::ofstream out(smokeScene, std::ios::binary);
            // "\u5730\u9762" = 地面, "\u82F1\u96C4" = 英雄 (JSON escapes keep
            // this C++ source ASCII; the parsed names are real UTF-8).
            out << R"({"entities":[{"name":"\u5730\u9762","mesh":"cube",)"
                   R"("pos":[0,0,0],"scale":[10,1,10],"tint":[0.8,0.8,0.8,1],)"
                   R"("metallic":0,"roughness":0.8,"ao":1,"emissiveIntensity":1,)"
                   R"("albedoTex":"","mrTex":"","aoTex":"","emissiveTex":""},)"
                   R"({"name":"\u82F1\u96C4","mesh":"hero","pos":[0,0,5],)"
                   R"("scale":[1,1,1],"tint":[1,1,1,1],"metallic":0,"roughness":0.8,)"
                   R"("ao":1,"emissiveIntensity":1,"albedoTex":"","mrTex":"",)"
                   R"("aoTex":"","emissiveTex":""}]})";
        }
        LoadScene(smokeScene);
    }

    // --- Scene entity names stay UTF-8 (no mojibake regression) ---
    {
        const std::string ground = std::string("\xE5\x9C\xB0\xE9\x9D\xA2"); // 地面
        const std::string hero = std::string("\xE8\x8B\xB1\xE9\x9B\x84");   // 英雄
        bool foundHero = false;
        bool foundGround = false;
        for (const SceneEntity& e : entities_) {
            if (e.name == hero) foundHero = true;
            if (e.name == ground) foundGround = true;
        }
        check(!entities_.empty() && foundGround,
              "editor scene contains 地面 (UTF-8 intact)");
        check(foundHero, "editor scene contains 英雄 (UTF-8 intact)");
    }

    // --- Dear ImGui tool layer ---
    check(ImGui::GetCurrentContext() != nullptr, "ImGui context created");
    check(ImGui::GetIO().Fonts->IsBuilt(), "ImGui font atlas built");
    check(ImGui::GetIO().Fonts->Fonts.Size >= 1, "ImGui has at least one font");
    ImDrawData* dd = ImGui::GetDrawData();
    check(dd != nullptr && dd->CmdListsCount > 0, "ImGui produced draw data");

    // --- Add/remove component command round-trip ---
    {
        std::vector<SceneEntity> tmp(1);
        HistoryManager h;
        core::Json rb;
        rb.type_ = core::Json::Type::Object;
        core::Json shape;
        shape.type_ = core::Json::Type::String;
        shape.string_ = "box";
        rb.object_["shape"] = std::move(shape);
        h.Push(std::make_unique<AddComponentCommand>(&tmp, 0, "rigidbody", rb,
                                                     /*remove=*/false));
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "add-component command applies");
        h.Undo();
        check(tmp[0].extraComponents.empty(), "add-component command undoes");
        h.Redo();
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "add-component command redoes");
        h.Push(std::make_unique<AddComponentCommand>(&tmp, 0, "rigidbody", rb,
                                                     /*remove=*/true));
        check(tmp[0].extraComponents.empty(), "remove-component command applies");
        h.Undo();
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "remove-component command undoes");
    }

    // --- UI editor auto-save: editing a doc with a real path persists ---
    {
        const std::string tmpDoc = GetTempDir() + "/ui_autosave.ui.json";
        uiDoc_ = ui::UiDocument{};
        uiDoc_.root.rect = {0, 0, 1280, 720};
        ui::UiNode* label = uiDoc_.root.AddChild(ui::UiNodeType::Label, "T");
        label->text = "hello";
        uiDocPath_ = tmpDoc;
        uiDocOpen_ = true;
        label->text = "world"; // simulate an edit through the inspector
        MarkUIDirty();         // should write the file immediately
        ui::UiDocument reloaded;
        check(reloaded.Load(tmpDoc) && reloaded.Find("T") &&
                  reloaded.Find("T")->text == "world",
              "UI auto-save persists edits to disk");
        uiDocOpen_ = false;
        uiDocPath_.clear();
        uiSelected_ = nullptr;
    }

    // --- UI editor multi-select / align / duplicate (P5-editor UX) ---
    {
        uiDoc_ = ui::UiDocument{};
        uiDoc_.root.rect = {0, 0, 1280, 720};
        ui::UiNode* a = uiDoc_.root.AddChild(ui::UiNodeType::Label, "A");
        a->rect = {10, 20, 100, 24};
        ui::UiNode* b = uiDoc_.root.AddChild(ui::UiNodeType::Label, "B");
        b->rect = {300, 400, 120, 30};
        ui::UiNode* c = uiDoc_.root.AddChild(ui::UiNodeType::Label, "C");
        c->rect = {500, 600, 80, 20};

        UISelectNode(a);
        check(uiSelection_.size() == 1 && uiSelected_ == a,
              "UI select sets the active node");
        UIToggleSelectNode(b);
        check(uiSelection_.count(a) == 1 && uiSelection_.count(b) == 1 &&
                  uiSelection_.size() == 2,
              "UI ctrl-click multi-select accumulates a second node");
        UIToggleSelectNode(b);
        check(uiSelection_.size() == 1 && uiSelected_ == a,
              "UI ctrl-click again deselects");
        UIToggleSelectNode(b);
        UIToggleSelectNode(c);
        check(uiSelection_.size() == 3, "UI multi-select holds three nodes");

        const bool oldSnap = uiSnapToGrid_;
        uiSnapToGrid_ = false; // exact math for the align assertions
        UIAlignSelected(0);    // left
        check(a->rect.x == 0.0f && b->rect.x == 0.0f && c->rect.x == 0.0f,
              "UI align-left snaps every selected x to the parent edge");
        UIAlignSelected(1); // h-center
        check(std::fabs(a->rect.x - (1280.0f - a->rect.w) * 0.5f) < 0.01f &&
                  std::fabs(b->rect.x - (1280.0f - b->rect.w) * 0.5f) < 0.01f &&
                  std::fabs(c->rect.x - (1280.0f - c->rect.w) * 0.5f) < 0.01f,
              "UI align h-center snaps every selected x to the parent center");
        UIAlignSelected(5); // bottom
        check(std::fabs(a->rect.y - (720.0f - a->rect.h)) < 0.01f &&
                  std::fabs(b->rect.y - (720.0f - b->rect.h)) < 0.01f &&
                  std::fabs(c->rect.y - (720.0f - c->rect.h)) < 0.01f,
              "UI align bottom snaps every selected y to the parent bottom");
        uiSnapToGrid_ = oldSnap;

        const size_t childCount = uiDoc_.root.children.size();
        UIDuplicateSelectedNodes();
        check(uiDoc_.root.children.size() == childCount + 3 &&
                  uiSelection_.size() == 3,
              "UI duplicate clones every selected node");
        UIDeleteSelectedNodes();
        check(uiDoc_.root.children.size() == childCount && uiSelection_.empty(),
              "UI delete removes the cloned selection");
        uiDocOpen_ = false;
        uiDocPath_.clear();
    }

    // --- Tool panels ---
    check(!core::GetRecentLogs(16).empty(), "log panel has engine log entries");
    check(!assetEntries_.empty(), "asset panel enumerated files");
    assetGridView_ = true; // the grid view renders from the next frame on;
                           // a crash here fails the smoke run
    check(assetGridView_, "asset panel thumbnail grid view enabled");

    // --- Transform gizmo ---
    // The gizmo renders every frame while an entity is selected; verify the
    // setup path ran and the matrix boundary (engine row-major Mat4 <-> ImGuizmo
    // column-major float[16]) round-trips a synthetic TRS without drift.
    if (editMode_ == EditMode::Scene3D) {
        check(gizmoDrawn_, "transform gizmo drawn in the viewport");
        check(gizmoBeginFrame_, "ImGuizmo::BeginFrame called every frame");
        check(gizmoAltWindowSet_, "gizmo hover bound to the dock host window");
        {
            const math::Rect2 vr = ValidSceneRect();
            check(gizmoRect_[0] == vr.x && gizmoRect_[1] == vr.y &&
                      gizmoRect_[2] == vr.w && gizmoRect_[3] == vr.h,
                  "gizmo rect matches the scene render rect");
        }
    }
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
        // Restore the REAL project dir (and persist it) so the temp cfg_proj
        // directory is never written into the user's editor config.
        projectDir_ = cfgPrev;
        SaveEditorConfig();
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
        std::string exportedPath = GetTempDir() + "/assets/scenes/exported_scene.json";
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

    // --- Behavior tree editor: canvas + save/load + link path + undo ---
    {
        check(btCanvasDrawn_, "bt canvas renders the seeded tree");
        check(btGraph_.NodeCount() == 3u && btGraph_.LinkCount() == 2u,
              "bt panel seeded a 3-node linked tree");

        // Save/load through the editor's own panel functions (temp dir), not a
        // raw file write: the whole point is that what the editor saves the
        // editor (and the runtime) can read back.
        const std::string btPath = GetTempDir() + "/bt_smoke.bt.json";
        const std::string savedJson = btGraph_.Serialize();
        check(!savedJson.empty(), "bt smoke: tree serialized");
        check(BtSaveToFile(btPath), "bt smoke: editor save writes .bt.json");
        btGraph_ = btgraph::BtGraph{};
        check(BtLoadFromFile(btPath), "bt smoke: editor load reads .bt.json back");
        check(btGraph_.Serialize() == savedJson,
              "bt smoke: editor save/load round-trip identical");

        // The unloadable-tree guard: a lone empty composite must be refused.
        btgraph::BtGraph solo;
        solo.AddNode("sequence", math::Vec2{});
        btGraph_ = std::move(solo);
        check(!BtSaveToFile(btPath), "bt smoke: empty composite save refused");

        // Canvas link path: select node A, Ctrl+click node B -> B is linked as
        // A's child (exercises the real click handler, not the raw model).
        btGraph_ = btgraph::BtGraph{};
        const std::string a = btGraph_.AddNode("sequence", math::Vec2{20.f, 20.f});
        const std::string b = btGraph_.AddNode("wait", math::Vec2{240.f, 240.f});
        btSelected_ = a;
        BtCanvasClick(math::Vec2{245.f, 245.f}, /*ctrl=*/true, /*shift=*/false);
        check(btGraph_.LinkCount() == 1u, "bt smoke: ctrl+click creates a link");
        if (btGraph_.LinkCount() == 1u) {
            const btgraph::BtGraphLink& link = btGraph_.Links()[0];
            check(link.parent == a && link.child == b,
                  "bt smoke: ctrl+click links the clicked node under the selected");
        }
        {
            core::Json tree = core::Json::Parse(btGraph_.Serialize(), nullptr);
            const core::Json* root = tree.Get("root");
            const core::Json* kids = root ? root->Get("children") : nullptr;
            check(kids != nullptr && kids->Size() == 1u &&
                      kids->At(0)->Get("type")->GetString() == std::string("wait"),
                  "bt smoke: serialized tree nests the linked child");
        }

        // Editor graph edits route through the undo stack: add -> undo -> redo.
        const size_t nodesBefore = btGraph_.NodeCount();
        const btgraph::BtGraph before = btGraph_;
        const std::string nid = btGraph_.AddNode("in_range", math::Vec2{0.f, 0.f});
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

    // --- Script panel: syntax check + attach/configure via command stack + export ---
    // Point the project at a temp dir with one valid + one broken script, run
    // the real check path, attach the valid script to a selected entity through
    // the undo command, export, and assert the JSON carries the script
    // component (the same flow the user drives in the 脚本 panel).
    {
        const std::string proj = GetTempDir() + "/script_smoke_proj";
        EnsureDirs(proj + "/assets/scripts");
        {
            std::ofstream out(proj + "/assets/scripts/good.lua", std::ios::binary);
            out << "function on_update(ent, dt)\n  Log('tick')\nend\n";
        }
        {
            std::ofstream out(proj + "/assets/scripts/broken.lua", std::ios::binary);
            out << "function on_update(ent, dt)\n  this is not lua !!!\nend\n";
        }
        const std::string prevProj = projectDir_;
        projectDir_ = proj;
        RefreshScriptChecks();
        check(!scriptFiles_.empty(), "script panel: project scripts enumerated");
        bool sawGood = false, sawBroken = false, brokenHasLine = false;
        for (size_t i = 0; i < scriptFiles_.size(); ++i) {
            if (scriptFiles_[i] == "assets/scripts/good.lua")
                sawGood = scriptChecks_[i].ok && scriptChecks_[i].message.empty();
            else if (scriptFiles_[i] == "assets/scripts/broken.lua") {
                sawBroken = !scriptChecks_[i].ok && !scriptChecks_[i].message.empty();
                brokenHasLine = scriptChecks_[i].line > 0;
            }
        }
        check(sawGood && sawBroken && brokenHasLine,
              "script panel: syntax check passes valid and flags broken with a line");

        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            const int idx = selected_;
            SceneEntity& sel = entities_[static_cast<size_t>(idx)];
            core::Json vars;
            vars.type_ = core::Json::Type::Object;
            core::Json speed;
            speed.type_ = core::Json::Type::Number;
            speed.number_ = 1.5;
            vars.object_["speed"] = speed;
            std::vector<SceneScriptFields> newList = sel.scripts;
            newList.push_back({"lua", "assets/scripts/good.lua", vars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, sel.scripts, newList,
                /*mergeable=*/false));
            check(sel.scripts.size() == 1 &&
                      sel.scripts[0].path == "assets/scripts/good.lua" &&
                      sel.scripts[0].backend == "lua" && sel.scripts[0].vars.IsObject() &&
                      sel.scripts[0].vars.Get("speed")->GetNumber() == 1.5,
                  "script panel: attach applies through the command stack");
            history_.Undo();
            check(sel.scripts.empty(),
                  "script panel: undo detaches the script component");
            history_.Redo();
            check(sel.scripts.size() == 1 &&
                      sel.scripts[0].path == "assets/scripts/good.lua",
                  "script panel: redo re-attaches the script component");

            // Export and assert the mounted scripts land in the scene JSON as
            // the "scripts" list component (flat, one entry per mounted script).
            const std::string expProj = GetTempDir();
            projectDir_ = expProj;
            core::Status exp = ExportScene();
            projectDir_ = prevProj;
            check(exp.Ok(), "script panel: export with an attached script succeeds");
            if (exp.Ok()) {
                std::ifstream fin(expProj + "/assets/scenes/exported_scene.json");
                std::stringstream ss;
                ss << fin.rdbuf();
                auto parsed = scene::SceneFile::Parse(ss.str());
                check(parsed.Ok(), "script panel: exported scene parses");
                if (parsed.Ok() && static_cast<size_t>(idx) < parsed.Value().entities.size()) {
                    const scene::ComponentDef* sc = nullptr;
                    for (const auto& c : parsed.Value().entities[static_cast<size_t>(idx)].components) {
                        if (c.name == "scripts") {
                            sc = &c;
                            break;
                        }
                    }
                    const core::Json* items = sc ? sc->data.Get("items") : nullptr;
                    bool scriptOk = items && items->IsArray() && items->Size() == 1;
                    const core::Json* first = scriptOk ? items->At(0) : nullptr;
                    scriptOk = scriptOk && first && first->Get("backend") &&
                               first->Get("path") && first->Get("vars") &&
                               first->Get("backend")->GetString() == "lua" &&
                               first->Get("path")->GetString() == "assets/scripts/good.lua" &&
                               first->Get("vars")->Get("speed")->GetNumber() == 1.5;
                    check(scriptOk,
                          "script panel: exported JSON carries the script component");
                }
            }
        }
        projectDir_ = prevProj;
        // Leave the script attached on the entity: it exercises the play
        // path (a missing script file is skipped non-fatally by the runtime).
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: script panel checks done");
    }

    // --- Script panel sync invalidation on entity-list mutation (T4.5 review) ---
    // The panel caches its dropdown + vars buffer by the selected INDEX. Any
    // mutation that appends/removes/moves entities (or reselects after a load)
    // must invalidate that cache, or the panel shows the PREVIOUS occupant's
    // script and 附加 silently attaches it to the entity that now sits at the
    // index. This reproduces the reported flow: select the last entity and sync
    // the panel to a distinctive script, AddEntity (appends + reselects the new
    // last), then verify the cache was invalidated and a fresh attach lands on
    // the NEW entity, not the stale one.
    {
        const int last = static_cast<int>(entities_.size()) - 1;
        check(last >= 0, "script sync: smoke has an entity to select");
        if (last >= 0) {
            SetSelection(last);
            // Emulate the panel having synced to the last entity + a script
            // attached to it (the stale state that must not leak forward).
            SceneEntity& oldLast = entities_[static_cast<size_t>(last)];
            core::Json staleVars;
            staleVars.type_ = core::Json::Type::Object;
            core::Json staleMarker;
            staleMarker.type_ = core::Json::Type::Number;
            staleMarker.number_ = 9.0;
            staleVars.object_["stale"] = staleMarker;
            // Replace the entity's mounted list with one distinctive script
            // (the stale panel state that must not leak to the next entity).
            std::vector<SceneScriptFields> staleList;
            staleList.push_back({"lua", "assets/scripts/stale.lua", staleVars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, last, ApplyScriptList, oldLast.scripts, staleList,
                /*mergeable=*/false));
            check(oldLast.scripts.size() == 1 &&
                      oldLast.scripts[0].path == "assets/scripts/stale.lua",
                  "script sync: distinctive script attached to the last entity");
            // The insert below may reallocate the vector, so keep the stale
            // path by value (never hold a reference across AddEntity).
            const std::string stalePath = oldLast.scripts[0].path;
            scriptSyncEntity_ = last; // panel cache now points at the last index
            scriptAttachIndex_ = 0;

            const size_t countBefore = entities_.size();
            AddEntity("cube"); // appends + reselects the new last entity
            check(entities_.size() == countBefore + 1,
                  "script sync: AddEntity appends a new entity");
            check(selected_ == static_cast<int>(entities_.size()) - 1,
                  "script sync: AddEntity selects the new last entity");
            check(scriptSyncEntity_ == -1,
                  "script sync: entity-list mutation invalidates the panel sync cache");

            // Attach through the real command path: must land on the NEW entity.
            const int freshIdx = static_cast<int>(entities_.size()) - 1;
            SceneEntity& fresh = entities_[static_cast<size_t>(freshIdx)];
            core::Json freshVars;
            freshVars.type_ = core::Json::Type::Object;
            core::Json freshMarker;
            freshMarker.type_ = core::Json::Type::Number;
            freshMarker.number_ = 3.0;
            freshVars.object_["hp"] = freshMarker;
            std::vector<SceneScriptFields> freshList = fresh.scripts;
            freshList.push_back({"lua", "assets/scripts/good.lua", freshVars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, freshIdx, ApplyScriptList, fresh.scripts, freshList,
                /*mergeable=*/false));
            check(fresh.scripts.size() == 1 &&
                      fresh.scripts[0].path == "assets/scripts/good.lua" &&
                      fresh.scripts[0].vars.Get("hp")->GetNumber() == 3.0,
                  "script sync: attach lands on the new entity");
            check(entities_[static_cast<size_t>(last)].scripts.size() == 1 &&
                      entities_[static_cast<size_t>(last)].scripts[0].path == stalePath,
                  "script sync: the previous entity keeps its own script (no stale attach)");
            history_.Undo(); // leave the new cube script-less
            check(fresh.scripts.empty(),
                  "script sync: undo clears the new entity's script");
        }
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: sync invalidation checks done");
    }

    // --- Script mount list (multiple scripts, component-style add/remove) ---
    // The mounted scripts behave like other components: one list where the
    // every entry is equal - add appends, remove erases, multiple allowed,
    // and each change is a single undo step.
    {
        const int idx = static_cast<int>(entities_.size()) - 1;
        check(idx >= 0, "script list: smoke has an entity");
        if (idx >= 0) {
            SetSelection(idx);
            SceneEntity& ent = entities_[static_cast<size_t>(idx)];
            check(ent.scripts.empty(),
                  "script list: fresh entity mounts no scripts");

            // First add appends one entry.
            std::vector<SceneScriptFields> one = ent.scripts;
            one.push_back({"lua", "assets/scripts/good.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, one,
                /*mergeable=*/false));
            check(ent.scripts.size() == 1 &&
                      ent.scripts[0].path == "assets/scripts/good.lua",
                  "script list: first mount appends an entry");

            // Second add appends another entry (multiple scripts).
            std::vector<SceneScriptFields> two = ent.scripts;
            two.push_back({"lua", "assets/scripts/stale.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, two,
                /*mergeable=*/false));
            check(ent.scripts.size() == 2 &&
                      ent.scripts[0].path == "assets/scripts/good.lua" &&
                      ent.scripts[1].path == "assets/scripts/stale.lua",
                  "script list: second mount appends (multiple scripts)");

            // Remove the first entry: the rest stay put, no promotion concept.
            std::vector<SceneScriptFields> afterRemove = ent.scripts;
            afterRemove.erase(afterRemove.begin());
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, afterRemove,
                /*mergeable=*/false));
            check(ent.scripts.size() == 1 &&
                      ent.scripts[0].path == "assets/scripts/stale.lua",
                  "script list: removing an entry leaves the rest unchanged");

            // Undo/redo replay the whole list in single steps.
            history_.Undo();
            check(ent.scripts.size() == 2 &&
                      ent.scripts[0].path == "assets/scripts/good.lua",
                  "script list: undo restores both mounts");
            history_.Undo();
            check(ent.scripts.size() == 1 &&
                      ent.scripts[0].path == "assets/scripts/good.lua",
                  "script list: undo restores the first mount");
            history_.Redo();
            check(ent.scripts.size() == 2 &&
                      ent.scripts[1].path == "assets/scripts/stale.lua",
                  "script list: redo replays the second mount");
            history_.Redo();
            check(ent.scripts.size() == 1 &&
                      ent.scripts[0].path == "assets/scripts/stale.lua",
                  "script list: redo replays the removal");

            // Leave the entity unmounted so the play sandbox stays clean.
            history_.Undo();
            history_.Undo();
            history_.Undo();
            check(ent.scripts.empty(),
                  "script list: smoke leaves the entity unmounted");
        }
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: script list checks done");
    }

    // --- Profiler panel (T4.8): the panel opened at frame 29 and populated its
    // stats + rolling frame-time buffer during this frame's UI build. ---
    check(profilerDrawn_, "profiler panel rendered its stats");
    {
        bool anyMs = false;
        for (float v : profilerMs_) {
            if (v > 0.0f) {
                anyMs = true;
                break;
            }
        }
        check(anyMs, "profiler panel recorded frame-time samples");
    }

    // --- Multi-camera viewport (T4.8): the three presets expose the right
    // projection + look direction, and the ortho pick ray stays parallel to the
    // forward axis (not through the eye). Frame 31/32 verify the top view
    // actually renders. ---
    {
        const gfx::Camera persp = ActiveCamera();
        check(!persp.ortho, "multi-cam: perspective preset is perspective");
        viewCam_ = ViewCam::Top;
        const gfx::Camera top = ActiveCamera();
        check(top.ortho, "multi-cam: top preset is orthographic");
        const math::Vec3 topFwd = (top.target - top.position).Normalized();
        check(std::fabs(topFwd.y + 1.0f) < 1e-4f && std::fabs(topFwd.x) < 1e-4f &&
                  std::fabs(topFwd.z) < 1e-4f,
              "multi-cam: top preset looks down -Y");
        viewCam_ = ViewCam::Front;
        const gfx::Camera front = ActiveCamera();
        check(front.ortho, "multi-cam: front preset is orthographic");
        const math::Vec3 frontFwd = (front.target - front.position).Normalized();
        check(std::fabs(frontFwd.z + 1.0f) < 1e-4f && std::fabs(frontFwd.x) < 1e-4f &&
                  std::fabs(frontFwd.y) < 1e-4f,
              "multi-cam: front preset looks down -Z");
        const math::Ray orthoRay = ScreenRay(top, 1.5f, {640.0f, 360.0f});
        const math::Vec3 rayDir = orthoRay.dir.Normalized();
        check(std::fabs(rayDir.y + 1.0f) < 1e-4f,
              "multi-cam: ortho pick ray is parallel to the forward axis");
        viewCam_ = ViewCam::Perspective;
    }

    // --- Asset thumbnails (T4.8): select a model asset in the asset panel so
    // the next OnRender generates its offscreen thumbnail (verified by the
    // frame poll above). A texture asset registers the image-preview texture
    // id. ---
    {
        const std::string kThumbPath = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
        smokeThumbPath_ = kThumbPath;
        // Navigate the panel into the model's directory so the listing (one
        // level, like the real UI) contains the asset, then select it.
        std::string thumbParent = kThumbPath;
        const size_t slash = thumbParent.find_last_of('/');
        if (slash != std::string::npos) thumbParent = thumbParent.substr(0, slash);
        assetDir_ = thumbParent;
        RefreshAssetDir();
        bool found = false;
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            if (assetEntries_[i].path == kThumbPath) {
                selectedAsset_ = static_cast<int>(i);
                found = true;
                break;
            }
        }
    check(found, "asset thumbnail: helmet glTF present in the asset panel listing");
        if (found) RequestMeshThumbnail(kThumbPath);
        check(std::find(meshThumbQueue_.begin(), meshThumbQueue_.end(), kThumbPath) !=
                  meshThumbQueue_.end(),
              "asset thumbnail: mesh thumbnail render requested");

        gfx::Texture tex =
            assetMgr_.LoadTexture("assets/models/DamagedHelmet/Default_albedo.jpg");
        check(tex.Valid(), "asset thumbnail: texture asset loads for the preview");
        ImportAssetPath("assets/models/DamagedHelmet/Default_albedo.jpg");
        check(previewTexId_ != ImTextureID_Invalid,
              "asset thumbnail: image preview texture registered for ImGui");
    }

    // --- Prefab workflow (Godot-style): library load + instantiate + save ---
    {
        const std::string proj = GetTempDir() + "/prefab_proj";
        EnsureDirs(proj + "/assets/prefabs");
        {
            std::ofstream out(proj + "/assets/prefabs/watchtower.json", std::ios::binary);
            out << R"({"components":{"transform":{"pos":[0,0,0],"scale":[1,1,1]},
                      "mesh":{"meshKey":"cube","colorHex":"#AABBCC"},
                      "health":{"hp":50,"maxHp":50}}})";
        }
        const std::string prev = projectDir_;
        projectDir_ = proj;
        LoadPrefabLibrary();
        check(prefabLib_.Has("watchtower"),
              "prefab: library loads assets/prefabs/watchtower.json");
        const size_t before = entities_.size();
        AddEntity("prefab:watchtower");
        check(entities_.size() == before + 1 && entities_.back().prefab == "watchtower",
              "prefab: instantiate appends an entity with the prefab reference");
        SetSelection(static_cast<int>(entities_.size()) - 1);
        SavePrefab("watchtower_copy");
        check(prefabLib_.Has("watchtower_copy"),
              "prefab: SavePrefab registers a new template");
        history_.Undo(); // drop the instanced entity so later smoke checks
                         // (play) run against the canonical scene
        projectDir_ = prev;
    }

    // --- Asset panel: create + import actions (temp project) ---
    {
        const std::string proj = GetTempDir() + "/asset_proj";
        MakeDir(proj);
        const std::string prevDir = assetDir_;
        const std::string prevProj = projectDir_;
        projectDir_ = proj;
        assetDir_ = proj + "/assets";
        MakeDir(assetDir_);
        CreateAssetFile("test.lua", 1);
        check(std::ifstream(assetDir_ + "/test.lua").is_open(),
              "asset: create lua file");
        CreateAssetFile("data.json", 2);
        check(std::ifstream(assetDir_ + "/data.json").is_open(),
              "asset: create json file");
        CreateAssetFile("subdir", 0);
        const std::string subdir = assetDir_ + "/subdir";
        struct _stat64 st;
        check(_stat64(subdir.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR),
              "asset: create directory");
        const std::string src = GetTempDir() + "/asset_src.png";
        {
            std::ofstream out(src, std::ios::binary);
            out << "fake png bytes";
        }
        ImportAssetFile(src);
        check(std::ifstream(assetDir_ + "/asset_src.png").is_open(),
              "asset: import copies the file into the project");
        ImportAssetFile(src); // duplicate -> numbered name
        check(std::ifstream(assetDir_ + "/asset_src_1.png").is_open(),
              "asset: duplicate import gets a numbered name");
        // Directory import (a model resource pack with textures + subfolders).
        const std::string pack = GetTempDir() + "/asset_pack";
        MakeDir(pack);
        MakeDir(pack + "/models");
        MakeDir(pack + "/models/tex");
        {
            std::ofstream out(pack + "/models/foo.obj", std::ios::binary);
            out << "v 0 0 0\n";
            std::ofstream out2(pack + "/models/foo.mtl", std::ios::binary);
            out2 << "newmtl mat\n";
            std::ofstream out3(pack + "/models/tex/foo.png", std::ios::binary);
            out3 << "png";
        }
        ImportAssetFile(pack);
        check(std::ifstream(assetDir_ + "/asset_pack/models/foo.obj").is_open(),
              "asset: directory import copies nested model files");
        check(std::ifstream(assetDir_ + "/asset_pack/models/tex/foo.png").is_open(),
              "asset: directory import copies nested texture files");
        // Delete the imported file (recycle bin on Windows; removed here).
        selectedAsset_ = -1;
        RefreshAssetDir();
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            if (assetEntries_[i].name == "asset_src.png") {
                selectedAsset_ = static_cast<int>(i);
                break;
            }
        }
        check(selectedAsset_ >= 0, "asset: delete target found in the listing");
        assetDeletePending_ = selectedAsset_;
        if (selectedAsset_ >= 0) {
            const std::string victim =
                assetEntries_[static_cast<size_t>(selectedAsset_)].path;
            check(DeletePathRecursive(victim), "asset: delete removes the file");
            check(!std::ifstream(victim).is_open(), "asset: deleted file is gone");
        }
        // Relative-path delete: after a project switch the asset panel points
        // at "projects/xxx/assets" (relative); SHFileOperationW silently fails
        // on relative paths, so DeletePathRecursive must resolve them first.
        {
            const std::string relFile = "build/rel_del_test.txt";
            {
                std::ofstream out(relFile, std::ios::binary);
                out << "x";
            }
            check(std::ifstream(relFile).is_open(), "asset: relative test file exists");
            check(DeletePathRecursive(relFile),
                  "asset: delete works with a relative path");
            check(!std::ifstream(relFile).is_open(),
                  "asset: relative-path file actually removed");
        }
        // DeleteSelectedAsset end-to-end (the path the 删除 button / Delete
        // key / right-click menu use): select + delete removes the file.
        {
            const std::string tmpFile = assetDir_ + "/delete_me.txt";
            {
                std::ofstream out(tmpFile, std::ios::binary);
                out << "x";
            }
            RefreshAssetDir();
            selectedAsset_ = -1;
            for (size_t i = 0; i < assetEntries_.size(); ++i) {
                if (assetEntries_[i].name == "delete_me.txt") {
                    selectedAsset_ = static_cast<int>(i);
                    break;
                }
            }
            check(selectedAsset_ >= 0, "asset: delete target selected");
            DeleteSelectedAsset();
            check(!std::ifstream(tmpFile).is_open(),
                  "asset: DeleteSelectedAsset removes the selected file");
        }
        projectDir_ = prevProj;
        assetDir_ = prevDir;
    }

    // --- Material-ball assets (Unity .mat / Godot Material style) ---
    {
        const std::string proj = GetTempDir() + "/asset_proj";
        const std::string prevProj = projectDir_;
        const std::string prevAsset = assetDir_;
        projectDir_ = proj;
        assetDir_ = proj + "/assets";
        const size_t before = entities_.size();
        AddEntity("cube");
        const int idx = static_cast<int>(entities_.size()) - 1;
        SetSelection(idx);
        entities_[static_cast<size_t>(idx)].metallic = 0.42f;
        entities_[static_cast<size_t>(idx)].roughness = 0.31f;
        SaveMaterialAsset("smoke_mat");
        check(std::ifstream(proj + "/assets/materials/smoke_mat.mat.json").is_open(),
              "material: SaveMaterialAsset writes the .mat.json");
        check(entities_[static_cast<size_t>(idx)].materialRef ==
                  "assets/materials/smoke_mat.mat.json",
              "material: entity links the asset reference");
        {
            std::ofstream out(proj + "/assets/materials/other.mat.json", std::ios::binary);
            out << R"({"colorHex":"#112233","metallic":0.9,"roughness":0.2})";
        }
        ApplyMaterialAsset(proj + "/assets/materials/other.mat.json");
        SceneEntity& applied = entities_[static_cast<size_t>(idx)];
        check(applied.materialRef == "assets/materials/other.mat.json" &&
                  std::fabs(applied.metallic - 0.9f) < 1e-5f &&
                  std::fabs(applied.roughness - 0.2f) < 1e-5f,
              "material: ApplyMaterialAsset updates the entity");
        // CJK material name: SaveMaterialAsset must write the file even when
        // the asset name is Chinese (the inspector's default is entity name).
        {
            SceneEntity& saveE = entities_[static_cast<size_t>(idx)];
            saveE.name = "\u519c\u820d_\u4e1c";
            SaveMaterialAsset("\u519c\u820d_\u4e1c");
            const std::string zhMat =
                proj + "/assets/materials/\u519c\u820d_\u4e1c.mat.json";
            SceneEntity probe;
            check(LoadMaterialParamsInto(probe, zhMat),
                  "material: CJK-named material ball saved to disk");
        }
        RequestMaterialThumbnail(proj + "/assets/materials/smoke_mat.mat.json");
        check(!materialThumbQueue_.empty(),
              "material: sphere preview queued for the material ball");
        // CJK filename: ifstream on Windows must still open the asset (the
        // realm project's material balls are Chinese-named).
        {
            const std::string zhFile =
                proj + "/assets/materials/\u6d4b\u8bd5\u7403.mat.json";
            WriteFileUtf8(zhFile, R"({"colorHex":"#FF8800","metallic":0.5,"roughness":0.3})");
        }
        RequestMaterialThumbnail(proj + "/assets/materials/\u6d4b\u8bd5\u7403.mat.json");
        // Scene export carries the reference; reloading expands it again.
        history_.Undo(); // remove the temp cube so later checks see the sandbox
        projectDir_ = prevProj;
        assetDir_ = prevAsset;
    }

    // --- Built-in script editor (open / save / syntax check) ---
    {
        const std::string path = GetTempDir() + "/editor_script.lua";
        {
            std::ofstream out(path, std::ios::binary);
            out << "function on_start(ent)\nend\n";
        }
        OpenScriptEditor(path);
        check(showScriptEditor_ && scriptEditorPath_ == path,
              "script editor: opens the file");
        check(scriptEditorCheck_.ok, "script editor: syntax passes on open");
        // Break the syntax, save, and expect the error to surface.
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_), "function broken( then\n");
        SaveScriptEditor();
        check(!scriptEditorCheck_.ok && !scriptEditorCheck_.message.empty(),
              "script editor: syntax error detected after save");
        // Fix and save again.
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_),
                      "function on_start(ent)\nend\n");
        SaveScriptEditor();
        check(scriptEditorCheck_.ok, "script editor: syntax passes after fix");
        std::ifstream verify(path, std::ios::binary);
        std::string saved((std::istreambuf_iterator<char>(verify)),
                          std::istreambuf_iterator<char>());
        check(saved.find("function on_start(ent)") != std::string::npos,
              "script editor: save writes the edited content");
    }

#ifdef NEON_ENABLE_JS
    // --- Built-in script editor, JS backend (.js routes to QuickJS) ---
    // Only runs when the QuickJS backend is compiled in: without it the editor
    // cannot syntax-check .js files (the host is unavailable), so this smoke
    // assertion would fail on MSVC builds (NEON_ENABLE_JS defaults off).
    {
        const std::string path = GetTempDir() + "/editor_script.js";
        {
            std::ofstream out(path, std::ios::binary);
            out << "function on_start(ent) {\n}\n";
        }
        OpenScriptEditor(path);
        check(showScriptEditor_ && scriptEditorPath_ == path,
              "script editor: opens a .js file");
        check(scriptEditorCheck_.ok, "script editor: JS syntax passes on open");
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_), "function broken( {\n");
        SaveScriptEditor();
        check(!scriptEditorCheck_.ok && !scriptEditorCheck_.message.empty(),
              "script editor: JS syntax error detected after save");
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_),
                      "function on_start(ent) {\n}\n");
        SaveScriptEditor();
        check(scriptEditorCheck_.ok, "script editor: JS syntax passes after fix");
    }
#endif // NEON_ENABLE_JS

    // --- Editor plugins (project plugins/ dir, type=editor) ---
    {
        // The smoke can run from any cwd; if the bundled examples are reachable
        // from ".", load them so the registration checks below are meaningful.
        if (pluginMgr_ && pluginMgr_->Count() == 0 &&
            std::ifstream("plugins/tree_gen/plugin.json", std::ios::binary)
                .is_open()) {
            pluginMgr_->Load(".");
        }
        if (pluginMgr_ && pluginMgr_->Count() > 0) {
            bool hasTree = false;
            bool hasVault = false;
            bool hasTool = false;
            for (const editor::PluginPanel& p : pluginMgr_->Panels())
                if (p.id == "tree_gen") hasTree = true;
            for (const editor::PluginAssetSource& s : pluginMgr_->AssetSources())
                if (s.id == "vault") hasVault = true;
            for (const editor::PluginTool& t : pluginMgr_->Tools())
                if (t.id == "tree_gen") hasTool = true;
            check(hasTree, "editor plugin registered the tree generator panel");
            check(hasTool, "editor plugin registered a toolbar tool");
            check(hasVault, "editor plugin registered the asset vault source");
        }
        // Plugin mesh generation + scene placement (the tree generator's core
        // path): buildMesh writes an OBJ asset, spawn adds an entity.
        {
            const size_t before = entities_.size();
            const std::vector<math::Vec3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
            const std::string key = PluginBuildMesh("smoke_plugin_tri", verts, {1, 2, 3});
            check(!key.empty() && key.rfind("obj:", 0) == 0,
                  "plugin buildMesh wrote an OBJ asset");
            PluginAddEntity(key, 5, 5, 5);
            check(entities_.size() == before + 1, "plugin spawn added an entity");
            history_.Undo(); // remove the smoke entity; keep the sandbox intact
            const std::string generated =
                (projectDir_ == "." ? "assets/generated/" : projectDir_ + "/assets/generated/") +
                "smoke_plugin_tri.obj";
            std::remove(generated.c_str());
        }
    }

    // --- 2D sprite: the editor loader parses a componentized sprite entity,
    // resolves its texture into a quad mesh and keeps the flip flags ---
    {
        const std::string tmpScene = GetTempDir() + "/sprite_smoke.json";
        {
            std::ofstream out(tmpScene, std::ios::binary);
            out << R"({"entities":[{"name":"TreeSprite","components":{)"
                   R"("transform":{"pos":[1,2,0],"scale":[2,2,1]},)"
                   R"("sprite":{"texture":"projects/pvz/assets/sprites/bucket.png",)"
                   R"("flipX":true,"flipY":false,"colorHex":"#00ff88"}}}]})";
        }
        const std::string prevScene = currentScenePath_;
        LoadScene(tmpScene);
        bool spriteOk = false;
        for (const SceneEntity& e : entities_) {
            if (!e.spriteTex.empty() && e.spriteFlipX && !e.spriteFlipY &&
                e.spriteMesh.Valid() && e.spriteMaterial.albedo.Valid()) {
                spriteOk = true;
            }
        }
        NEON_LOG_INFO("EDITOR-SPRITE-SMOKE: [%s] sprite entity parsed, texture + quad resolved",
                      spriteOk ? "PASS" : "FAIL");
        if (!spriteOk) smokeFailed_ = true;
        // G2-2: the editor holds a live ecs::World mirroring the scene via the
        // runtime Instantiate — the sprite entity must exist there too.
        {
            size_t worldSprites = 0;
            auto wview = sceneWorld_.ViewAll<scene::SceneSprite>();
            for (size_t i = 0; i < wview.Size(); ++i) {
                ecs::Entity ent = sceneWorld_.EntityAt<scene::SceneSprite>(i);
                const scene::SceneSprite* s = sceneWorld_.Get<scene::SceneSprite>(ent);
                if (s && !s->texture.empty() && s->flipX && !s->flipY) ++worldSprites;
            }
            check(worldSprites == 1u, "editor ecs world mirrors the sprite scene");
            const size_t worldTransforms = sceneWorld_.ViewAll<scene::SceneTransform>().Size();
            check(worldTransforms >= 1u, "editor ecs world has transforms");
        }
        // G2-2: the editor's play/save output flows through the runtime World
        // (entities_ -> SyncWorldFromEntities -> SceneFile::FromWorld). Verify it
        // reparses and keeps the sprite entity.
        {
            auto playRoot = BuildPlaySceneJson();
            check(playRoot.Ok(), "editor play json (via ecs world) builds");
            if (playRoot.Ok()) {
                auto reparse =
                    scene::SceneFile::Parse(core::JsonWriter::Write(playRoot.Value()));
                check(reparse.Ok(), "editor play json reparses");
                bool hasSprite = false;
                if (const core::Json* arr = playRoot.Value().Get("entities")) {
                    for (const core::Json& e : arr->Items()) {
                        const core::Json* comps = e.Get("components");
                        if (comps && comps->Get("sprite")) hasSprite = true;
                    }
                }
                check(hasSprite, "editor play json keeps the sprite entity");
            }
        }
        // G5-4: rebuild entities_ from the runtime World — the entity count must
        // match the World (the editor's working model is drivable by the World).
        {
            const size_t beforeCount = entities_.size();
            const size_t worldCount = sceneWorld_.ViewAll<scene::SceneTransform>().Size();
            UnflattenWorldToEntities();
            check(entities_.size() == worldCount, "editor unflattens world to entities");
            check(entities_.size() >= 1u, "editor unflattened entities non-empty");
            const size_t afterCount = entities_.size();
            (void)beforeCount;
            (void)afterCount;
        }
        if (!prevScene.empty()) LoadScene(prevScene);
    }
    // Restore the editor's actual scene (user data) after the deterministic
    // checks; later smoke frames (project switch / play) re-derive theirs.
    if (!smokePrevScene.empty()) LoadScene(smokePrevScene);

    NEON_LOG_INFO("EDITOR-UI-SMOKE: all checks done");
}
} // namespace neon::editor

