#include "editor.hpp"
#include "editor_util.hpp"

namespace neon::editor {

gfx::Camera EditorApp::PlayCamera() const {
    for (const SceneEntity& se : entities_) {
        if (se.nodeType != "Camera3D") continue;
        gfx::Camera cam;
        cam.position = se.pos;
        cam.target = se.pos + se.rot.Rotate({0.0f, 0.0f, -1.0f});
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.ortho = se.cameraOrtho;
        cam.orthoSize = se.cameraOrthoSize;
        cam.fovY = se.cameraFov * math::kDegToRad;
        return cam;
    }
    // Fixed 1280x720 design-space ortho: the 2D game canvas default.
    gfx::Camera cam;
    cam.target = {640.0f, 360.0f, 0.0f};
    cam.position = cam.target + math::Vec3{0.0f, 0.0f, 14.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.ortho = true;
    cam.orthoSize = 360.0f;
    cam.fovY = 60.0f * math::kDegToRad;
    return cam;
}

void EditorApp::DrawPlayHUD() {
    if (!play_ || !playActive_) return;
    scene::GameRuntime& rt = *play_;
    const ecs::Entity hero = rt.FindNamedEntity("英雄");
    const auto heroHp = rt.EntityHealth(hero);
    const float hp = heroHp.first, maxHp = heroHp.second;
    const float mana = rt.GameVar("hero_mana");
    const float maxMana = rt.GameVar("hero_max_mana");
    const float fireCd = rt.GameVar("hero_fire_cd");
    const float healCd = rt.GameVar("hero_heal_cd");
    const float meleeCd = rt.GameVar("hero_melee_cd");
    const int level = static_cast<int>(rt.GameVar("hero_level"));
    const int gold = static_cast<int>(rt.GameVar("hero_gold"));

    ui::Theme theme;
    theme.font = cjkFont_.Valid() ? cjkFont_ : pixelFont_;

    const int w = renderer_.ScreenWidth();
    const int h = renderer_.ScreenHeight();

    // HP bar (top-left).
    ui::DrawLabel(renderer_, theme, "生命", {24, 20}, 14, theme.text, false, true);
    const float hpFrac = maxHp > 0.0f ? math::Saturate(hp / maxHp) : 0.0f;
    const gfx::Color hpColor = hpFrac > 0.5f ? gfx::Color{0.2f, 1.0f, 0.35f, 1.0f}
                               : hpFrac > 0.25f ? gfx::Color{1.0f, 0.85f, 0.2f, 1.0f}
                                                : gfx::Color{1.0f, 0.2f, 0.2f, 1.0f};
    ui::DrawBar(renderer_, theme, {70, 14, 280, 22}, hpFrac, hpColor);

    // Mana bar.
    ui::DrawLabel(renderer_, theme, "法力", {24, 48}, 14, theme.text, false, true);
    const float manaFrac = maxMana > 0.0f ? math::Saturate(mana / maxMana) : 0.0f;
    ui::DrawBar(renderer_, theme, {70, 42, 200, 14}, manaFrac, gfx::Color{0.25f, 0.45f, 1.0f, 1.0f});

    char buf[96];
    std::snprintf(buf, sizeof(buf), "等级 %d", level);
    ui::DrawLabel(renderer_, theme, buf, {24, 64}, 13, theme.text, false, false);
    std::snprintf(buf, sizeof(buf), "金币 %d", gold);
    const math::Vec2 gs = ui::MeasureText(theme.font, buf, 16);
    ui::DrawLabel(renderer_, theme, buf, {static_cast<float>(w) - gs.x - 8, 18}, 16,
                  gfx::Color{1.0f, 0.85f, 0.3f, 1.0f}, false, false);

    // Skill hotbar (bottom-left): melee / fireball / heal with cooldowns.
    float sx = 24.0f, sy = static_cast<float>(h) - 66.0f;
    const float slotW = 54.0f, slotH = 54.0f, gap = 8.0f;
    auto slot = [&](const char* name, const char* key, float cd, const gfx::Color& color) {
        const math::Rect2 r{sx, sy, slotW, slotH};
        ui::DrawPanel(renderer_, theme, r);
        ui::DrawLabel(renderer_, theme, key, {sx + 4, sy + 2}, 12, theme.dim, false, false);
        ui::DrawLabel(renderer_, theme, name, {sx + slotW * 0.5f, sy + slotH * 0.5f}, 15, color,
                      true, true);
        if (cd > 0.0f) {
            ui::DrawBar(renderer_, theme, r, 1.0f, theme.panelBg.WithAlpha(0.65f));
            std::snprintf(buf, sizeof(buf), "%.1f", cd);
            ui::DrawLabel(renderer_, theme, buf, {sx + slotW * 0.5f, sy + slotH * 0.5f}, 15,
                          theme.text, true, true);
        }
        sx += slotW + gap;
    };
    slot("近战", "左键", meleeCd, gfx::Color{0.92f, 0.92f, 1.0f, 1.0f});
    slot("火球", "1", fireCd, gfx::Color{1.0f, 0.55f, 0.20f, 1.0f});
    slot("治疗", "2", healCd, gfx::Color{0.4f, 1.0f, 0.5f, 1.0f});
}

core::Result<core::Json> EditorApp::BuildSceneJsonFromEntities() {
    NormalizeEntityIds();
    if (entities_.empty())
        return core::Result<core::Json>::Err("editor: scene is empty");
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    auto parentNameOf = [this](int parentId) {
        if (parentId == 0) return std::string();
        for (const SceneEntity& p : entities_)
            if (p.id == parentId) return p.name;
        return std::string();
    };
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        if (e.id != 0) {
            core::Json id;
            id.type_ = core::Json::Type::Number;
            id.number_ = e.id;
            obj.object_["id"] = std::move(id);
        }
        if (!e.spriteTex.empty()) {
            // G2-2: 2D sprites go through the runtime's canonical sprite
            // builder (SceneFile::MakeSpriteEntity), so the editor export and
            // the runtime parser share one code path — no hand-written JSON
            // drift (this branch historically dropped health).
            auto res = scene::SceneFile::MakeSpriteEntity(
                e.name, e.pos, e.rot, e.scale, e.spriteTex, e.spriteFlipX,
                e.spriteFlipY, ColorToHex(e.tint), e.hp, e.maxHp,
                parentNameOf(e.parentId), e.parentId, e.id);
            if (!res.Ok()) {
                NEON_LOG_ERROR("Editor: sprite export for '%s' failed: %s", e.name.c_str(),
                               res.Error().c_str());
                continue;
            }
            obj = res.Value();
        } else if (!e.meshKey.empty()) {
        std::string meshKey = ExportMeshKey(e.meshKey);
        if (e.meshKey == "npc") {
            // Encode the villager's tunic tint into the mesh key so the runtime
            // bakes it into the NPC mesh (its material tint stays white).
            char buf[48];
            std::snprintf(buf, sizeof(buf), "npc:%d,%d,%d",
                          static_cast<int>(e.tint.r * 255.0f),
                          static_cast<int>(e.tint.g * 255.0f),
                          static_cast<int>(e.tint.b * 255.0f));
            meshKey = buf;
        }
        auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale, meshKey,
                                                e.metallic, e.roughness, e.tint, e.albedoTex,
                                                e.mrTex, e.aoTex, e.emissiveTex, e.ao,
                                                e.emissiveIntensity, "", "", core::Json{}, {},
                                                e.hp, e.maxHp, parentNameOf(e.parentId),
                                                e.parentId, e.id);
        if (!res.Ok()) {
            return core::Result<core::Json>::Err("editor: " + res.Error());
        }
        obj = res.Value();
        if (!e.materialRef.empty()) {
            // Write the material-ball reference alongside the expanded params
            // (runtime reads the params; the editor keeps the asset link).
            core::Json& mesh = obj.object_["components"].object_["mesh"];
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = e.materialRef;
            mesh.object_["materialRef"] = std::move(j);
        }
        } else {
        // Logical entity (mesh renderer removed or never added): name +
        // transform + health only; scripts/extra components merge below.
        auto mkStr = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto mkNum = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto mkArr = [&mkNum](const std::initializer_list<double>& vals) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            for (double v : vals) j.array_.push_back(mkNum(v));
            return j;
        };
        obj.type_ = core::Json::Type::Object;
        obj.object_["name"] = mkStr(e.name);
        core::Json tf;
        tf.type_ = core::Json::Type::Object;
        tf.object_["pos"] = mkArr({e.pos.x, e.pos.y, e.pos.z});
        tf.object_["rot"] = mkArr({e.rot.x, e.rot.y, e.rot.z, e.rot.w});
        tf.object_["scale"] = mkArr({e.scale.x, e.scale.y, e.scale.z});
        if (e.parentId != 0) tf.object_["parentId"] = mkNum(e.parentId);
        const std::string pname = parentNameOf(e.parentId);
        if (!pname.empty()) tf.object_["parent"] = mkStr(pname);
        core::Json comps;
        comps.type_ = core::Json::Type::Object;
        comps.object_["transform"] = std::move(tf);
        if (e.maxHp > 0.0f) {
            core::Json health;
            health.type_ = core::Json::Type::Object;
            health.object_["hp"] = mkNum(e.hp);
            health.object_["maxHp"] = mkNum(e.maxHp);
            comps.object_["health"] = std::move(health);
        }
        obj.object_["components"] = std::move(comps);
        }
        if (!e.prefab.empty()) obj.object_["prefab"] = [&e]() {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = e.prefab;
            return j;
        }();
        // Merge schema-editable extra components into the exported entity so
        // project scenes round-trip without data loss.
        if (!e.extraComponents.empty()) {
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            for (const auto& [cname, cdata] : e.extraComponents)
                comps.object_[cname] = cdata;
            obj.object_["components"] = std::move(comps);
        }
        // Mounted scripts: one flat "scripts" component [{backend,path,vars},
        // ...]. Every entry is equal; the runtime attaches each in order.
        if (!e.scripts.empty()) {
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            auto mkStr2 = [](const std::string& v) {
                core::Json j;
                j.type_ = core::Json::Type::String;
                j.string_ = v;
                return j;
            };
            core::Json items;
            items.type_ = core::Json::Type::Array;
            for (const SceneScriptFields& f : e.scripts) {
                if (f.path.empty()) continue; // unconfigured script block
                core::Json it;
                it.type_ = core::Json::Type::Object;
                it.object_["backend"] = mkStr2(f.backend.empty() ? "lua" : f.backend);
                it.object_["path"] = mkStr2(f.path);
                if (f.vars.IsObject()) it.object_["vars"] = f.vars;
                items.array_.push_back(std::move(it));
            }
            core::Json scripts;
            scripts.type_ = core::Json::Type::Object;
            scripts.object_["items"] = std::move(items);
            comps.object_["scripts"] = std::move(scripts);
            obj.object_["components"] = std::move(comps);
        }
        // P1-1/P2-3 export: node type, camera, sort order and the authored
        // terrain heightmap as runtime components.
        {
            auto mkNum = [](double v) {
                core::Json j;
                j.type_ = core::Json::Type::Number;
                j.number_ = v;
                return j;
            };
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            if (!e.nodeType.empty()) {
                core::Json t;
                t.type_ = core::Json::Type::Object;
                core::Json v;
                v.type_ = core::Json::Type::String;
                v.string_ = e.nodeType;
                t.object_["value"] = v;
                comps.object_["type"] = std::move(t);
            }
            if (e.nodeType == "Camera3D") {
                core::Json cam;
                cam.type_ = core::Json::Type::Object;
                cam.object_["fov"] = mkNum(e.cameraFov);
                core::Json ortho;
                ortho.type_ = core::Json::Type::Bool;
                ortho.bool_ = e.cameraOrtho;
                cam.object_["ortho"] = ortho;
                cam.object_["orthoSize"] = mkNum(e.cameraOrthoSize);
                comps.object_["camera"] = std::move(cam);
            }
            if (e.hasLight) {
                core::Json li;
                li.type_ = core::Json::Type::Object;
                core::Json t;
                t.type_ = core::Json::Type::String;
                t.string_ = e.light.type;
                li.object_["type"] = std::move(t);
                auto vecJson = [&](float x, float y, float z) {
                    core::Json a;
                    a.type_ = core::Json::Type::Array;
                    a.array_.push_back(mkNum(x));
                    a.array_.push_back(mkNum(y));
                    a.array_.push_back(mkNum(z));
                    return a;
                };
                auto colJson = [&](float r, float g, float b, float aa) {
                    core::Json a;
                    a.type_ = core::Json::Type::Array;
                    a.array_.push_back(mkNum(r));
                    a.array_.push_back(mkNum(g));
                    a.array_.push_back(mkNum(b));
                    a.array_.push_back(mkNum(aa));
                    return a;
                };
                li.object_["sunDir"] =
                    vecJson(e.light.sunDir.x, e.light.sunDir.y, e.light.sunDir.z);
                li.object_["color"] =
                    colJson(e.light.color.r, e.light.color.g, e.light.color.b, e.light.color.a);
                li.object_["intensity"] = mkNum(e.light.intensity);
                li.object_["radius"] = mkNum(e.light.radius);
                li.object_["ambientStrength"] = mkNum(e.light.ambientStrength);
                comps.object_["light"] = std::move(li);
            }
            if (e.zOrder != 0.0f) {
                core::Json so;
                so.type_ = core::Json::Type::Object;
                so.object_["z"] = mkNum(e.zOrder);
                comps.object_["sortOrder"] = std::move(so);
            }
            if (e.meshKey == "terrain" && !e.terrainHeights_.empty()) {
                core::Json terr;
                terr.type_ = core::Json::Type::Object;
                terr.object_["segments"] = mkNum(e.terrainSegments_);
                terr.object_["size"] = mkNum(e.terrainSize_);
                terr.object_["heightScale"] = mkNum(e.terrainHeightScale_);
                core::Json hs;
                hs.type_ = core::Json::Type::Array;
                for (float h : e.terrainHeights_) hs.array_.push_back(mkNum(h));
                terr.object_["heights"] = std::move(hs);
                // G2-3 chunked LOD + vegetation knobs (written only when the
                // user enables them; the runtime reads them back verbatim).
                if (e.chunkGridDiv_ > 0) {
                    terr.object_["chunkGridDiv"] = mkNum(e.chunkGridDiv_);
                    terr.object_["chunkLodLevels"] = mkNum(e.chunkLodLevels_);
                    terr.object_["chunkBaseSubdiv"] = mkNum(e.chunkBaseSubdiv_);
                }
                if (!e.vegMeshKey_.empty() && e.vegCount_ > 0) {
                    core::Json vk;
                    vk.type_ = core::Json::Type::String;
                    vk.string_ = e.vegMeshKey_;
                    terr.object_["vegMeshKey"] = std::move(vk);
                    terr.object_["vegCount"] = mkNum(e.vegCount_);
                    terr.object_["vegSeed"] = mkNum(e.vegSeed_);
                    terr.object_["vegSize"] = mkNum(e.vegSize_);
                    terr.object_["vegImpostorDistance"] = mkNum(e.vegImpostorDistance_);
                    terr.object_["vegMinHeight"] = mkNum(e.vegMinHeight_);
                    terr.object_["vegMaxHeight"] = mkNum(e.vegMaxHeight_);
                    terr.object_["vegMaxSlope"] = mkNum(e.vegMaxSlope_);
                }
                comps.object_["terrain"] = std::move(terr);
            }
            if (e.meshKey == "tilemap" && !e.tilemapTiles_.empty()) {
                core::Json tlm;
                tlm.type_ = core::Json::Type::Object;
                tlm.object_["cols"] = mkNum(e.tilemapCols_);
                tlm.object_["rows"] = mkNum(e.tilemapRows_);
                tlm.object_["cellSize"] = mkNum(e.tilemapCellSize_);
                core::Json tls;
                tls.type_ = core::Json::Type::Array;
                for (const std::string& t : e.tilemapTiles_) {
                    core::Json s;
                    s.type_ = core::Json::Type::String;
                    s.string_ = t;
                    tls.array_.push_back(std::move(s));
                }
                tlm.object_["tiles"] = std::move(tls);
                comps.object_["tilemap"] = std::move(tlm);
            }
            if (!e.decalTex.empty()) {
                core::Json dc;
                dc.type_ = core::Json::Type::Object;
                core::Json tex;
                tex.type_ = core::Json::Type::String;
                tex.string_ = e.decalTex;
                dc.object_["texture"] = std::move(tex);
                dc.object_["size"] = mkNum(e.decalSize);
                dc.object_["alpha"] = mkNum(e.decalAlpha);
                comps.object_["decal"] = std::move(dc);
            }
            if (!e.shaderPath.empty()) {
                // Editor-only custom fragment shader, preserved so the scene
                // round-trips (the runtime ignores this component).
                core::Json sh;
                sh.type_ = core::Json::Type::Object;
                sh.object_["path"] = [&e]() {
                    core::Json p;
                    p.type_ = core::Json::Type::String;
                    p.string_ = e.shaderPath;
                    return p;
                }();
                comps.object_["shader"] = std::move(sh);
            }
            obj.object_["components"] = std::move(comps);
        }
        arr.array_.push_back(std::move(obj));
    }
    root.object_["entities"] = std::move(arr);
    return core::Result<core::Json>::Ok(std::move(root));
}

// G2-2: rebuild the editor's live sceneWorld_ from entities_ via the canonical
// builders + runtime Instantiate (the same path the player runs). Editor
// metadata the runtime would reject (materialRef in mesh, top-level prefab) is
// stripped here and re-applied by BuildPlaySceneJson afterwards.
void EditorApp::SyncWorldFromEntities() {
    sceneWorld_.Clear();
    sceneCompReg_ = scene::ComponentRegistry{};
    scene::RegisterBuiltinComponents(sceneCompReg_);
    auto rootRes = BuildSceneJsonFromEntities();
    if (!rootRes.Ok()) return;
    core::Json root = rootRes.Value();
    auto arrIt = root.object_.find("entities");
    if (arrIt != root.object_.end() && arrIt->second.IsArray()) {
        core::Json& arr = arrIt->second;
        for (size_t i = 0; i < arr.array_.size() && i < entities_.size(); ++i) {
            const SceneEntity& se = entities_[i];
            if (se.materialRef.empty() && se.prefab.empty()) continue;
            core::Json& ent = arr.array_[i];
            if (!se.materialRef.empty()) {
                auto compsIt = ent.object_.find("components");
                if (compsIt != ent.object_.end()) {
                    auto meshIt = compsIt->second.object_.find("mesh");
                    if (meshIt != compsIt->second.object_.end())
                        meshIt->second.object_.erase("materialRef");
                }
            }
            if (!se.prefab.empty()) ent.object_.erase("prefab");
        }
    }
    auto parsed = scene::SceneFile::Parse(core::JsonWriter::Write(root));
    if (!parsed.Ok()) return;
    scene::PrefabLibrary prefs;
    scene::Instantiate(sceneWorld_, parsed.Value(), prefs, sceneCompReg_);
}

// G2-2: the play/save output is now generated FROM the runtime World the
// editor hosts (entities_ -> World via SyncWorldFromEntities, then the
// canonical SceneFile::FromWorld serializer). Editor metadata that the World
// cannot carry (materialRef, prefab) is re-applied per entity afterwards.
core::Result<core::Json> EditorApp::BuildPlaySceneJson() {
    NormalizeEntityIds();
    if (entities_.empty())
        return core::Result<core::Json>::Err("editor: scene is empty");
    SyncWorldFromEntities();
    auto out = scene::SceneFile::FromWorld(sceneWorld_);
    if (!out.Ok()) return out;
    core::Json root = out.Value();
    auto arrIt = root.object_.find("entities");
    if (arrIt != root.object_.end() && arrIt->second.IsArray()) {
        core::Json& arr = arrIt->second;
        for (size_t i = 0; i < arr.array_.size() && i < entities_.size(); ++i) {
            const SceneEntity& se = entities_[i];
            if (se.materialRef.empty() && se.prefab.empty()) continue;
            core::Json& ent = arr.array_[i];
            core::Json comps;
            auto compsIt = ent.object_.find("components");
            if (compsIt != ent.object_.end() && compsIt->second.IsObject())
                comps = compsIt->second;
            comps.type_ = core::Json::Type::Object;
            if (!se.materialRef.empty()) {
                auto meshIt = comps.object_.find("mesh");
                if (meshIt != comps.object_.end()) {
                    core::Json mr;
                    mr.type_ = core::Json::Type::String;
                    mr.string_ = se.materialRef;
                    meshIt->second.object_["materialRef"] = std::move(mr);
                }
            }
            if (!se.prefab.empty()) {
                core::Json pr;
                pr.type_ = core::Json::Type::String;
                pr.string_ = se.prefab;
                ent.object_["prefab"] = std::move(pr);
            }
            ent.object_["components"] = std::move(comps);
        }
    }
    return core::Result<core::Json>::Ok(std::move(root));
}

void EditorApp::StartPlay() {
    StopPlay(); // restart semantics: a fresh snapshot each time

    scene::GameRuntimeConfig cfg;
    cfg.assets = &assetMgr_;
#ifdef NEON_ENABLE_JOLT
    cfg.physicsBackend = "jolt"; // play uses Jolt rigid bodies when compiled
#endif
    cfg.scriptBaseDir = projectDir_.empty() ? "." : projectDir_;
    cfg.pluginBaseDir = projectDir_.empty() ? "." : projectDir_; // G5-1 native plugins
    cfg.localesDir = projectDir_.empty() ? "./locales" : projectDir_ + "/locales";
    cfg.input = Input(); // hero controller reads live WASD/mouse input
    cfg.font2d = cjkFont_.Valid() ? cjkFont_ : pixelFont_; // 2D HUD / on_render
    // PlaySfx(name) from game scripts routes to the procedural synth.
    cfg.playSfx = [this](const std::string& name) {
        if (audioBackend_) audioBackend_->Play(MakePvzSfx(name), 0.7f);
    };
    // P2-2 audio hooks: music bus, 3D positional sfx against the live camera
    // listener, and bus volume control.
    cfg.playMusic = [this](const std::string& name, float vol) {
        if (audioBackend_) audioBackend_->PlayMusic(MakePvzSfx(name), vol);
    };
    cfg.playSfx3D = [this](const std::string& name, const math::Vec3& pos) {
        if (audioBackend_) {
            const gfx::Camera& cam = ActiveCamera();
            const math::Vec3 fwd = (cam.target - cam.position).Normalized();
            audioBackend_->Play3D(MakePvzSfx(name), pos, cam.position, fwd, 0.7f);
        }
    };
    cfg.setBusVolume = [this](int bus, float gain) {
        if (audioBackend_ && bus >= 0 && bus <= 2)
            audioBackend_->SetBusVolume(static_cast<neon::audio::AudioBus>(bus), gain);
    };
    std::string json;

    // M1: data-driven skills table from <project>/skills.json (optional,
    // shared by both the 2D and 3D play branches).
    {
        std::ifstream skillsIn(projectDir_ + "/skills.json", std::ios::binary);
        if (skillsIn.is_open())
            cfg.skillsJson.assign(std::istreambuf_iterator<char>(skillsIn),
                                  std::istreambuf_iterator<char>());
    }

    if (projectMode_ == "2d") {
        // 2D project (NeonPvZ / NeonSnake): play the EDITOR'S LIVE ENTITIES
        // (a serialized snapshot, exactly like the 3D play) so unsaved
        // edits - moved sprites, added plants - appear in Play immediately;
        // the old disk read ignored editor changes until the scene was saved.
        // The on-disk start scene only backs up the empty-canvas case. Follows
        // the PROJECT type, not the current camera view: a 2D game plays as a
        // 2D game even in a perspective editor camera.
        cfg.assetBaseDir = projectDir_;
        auto root = BuildPlaySceneJson();
        if (root.Ok()) {
            json = core::JsonWriter::Write(root.Value());
        } else {
            const std::string sceneRel =
                projectStartScene_.empty() ? "scenes/pvz.json" : projectStartScene_;
            const std::string scenePath = projectDir_ + "/" + sceneRel;
            std::ifstream in(scenePath, std::ios::binary);
            if (!in.is_open()) {
                NEON_LOG_ERROR("Editor: cannot read play scene '%s'", scenePath.c_str());
                return;
            }
            json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    } else {
        // 3D scene: play the editor's current entities (serialized snapshot).
        if (entities_.empty()) {
            NEON_LOG_WARN("Editor: nothing to play (scene is empty)");
            return;
        }
        auto root = BuildPlaySceneJson();
        if (!root.Ok()) {
            NEON_LOG_ERROR("Editor: cannot build play scene: %s", root.Error().c_str());
            return;
        }
        json = core::JsonWriter::Write(root.Value());
    }

    play_ = std::make_unique<scene::GameRuntime>();
    core::Status st = play_->Start(json, cfg);
    if (!st.Ok()) {
        NEON_LOG_ERROR("Editor: play failed to start: %s", st.Error().c_str());
        play_.reset();
        return;
    }
    playActive_ = true;
    // Detach the input method while the play runs so game keys (WASD,
    // digits, space) arrive as raw key events even with a Chinese/Japanese IME
    // in composition mode; StopPlay re-attaches it for ImGui text input.
    if (Window()) Window()->SetImeEnabled(false);
    NEON_LOG_INFO("Editor: play started (%zu entities, %zu scripts, %zu trees)",
                  play_->EntityCount(), play_->ScriptCount(),
                  play_->BehaviorTreeCount());
}

void EditorApp::StopPlay() {
    if (!play_) return;
    play_->Stop();
    play_.reset();
    playActive_ = false;
    if (Window()) Window()->SetImeEnabled(true);
    NEON_LOG_INFO("Editor: play stopped");
}

void EditorApp::TogglePlay() {
    if (playActive_) {
        StopPlay();
    } else {
        StartPlay();
    }
}

} // namespace neon::editor

