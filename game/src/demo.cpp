#include "demo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "font_data.hpp"

#include "neon/scene/skinned_model.hpp"

#include <fstream>
#include <sstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace neon::demo {
namespace {

uint64_t EncodeEntity(ecs::Entity e) {
    return (static_cast<uint64_t>(e.generation) << 32) | e.id;
}

math::Quat FaceDirection(const math::Vec3& dir) {
    if (dir.LengthSq() < 1e-4f) return math::Quat::Identity();
    return math::Quat::FromEuler(0.0f, std::atan2(dir.x, dir.z), 0.0f);
}

float Hash2D(int x, int z) {
    uint32_t n = static_cast<uint32_t>(x) * 0x8DA6B343u + static_cast<uint32_t>(z) * 0xD8163841u +
                 0x9E3779B9u;
    n ^= n >> 13;
    n *= 0x85EBCA6Bu;
    n ^= n >> 16;
    return (n & 0xFFFF) / 65535.0f;
}

float SmoothNoise(float x, float z) {
    int x0 = static_cast<int>(std::floor(x));
    int z0 = static_cast<int>(std::floor(z));
    float fx = x - x0;
    float fz = z - z0;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sz = fz * fz * (3.0f - 2.0f * fz);
    return math::Lerp(math::Lerp(Hash2D(x0, z0), Hash2D(x0 + 1, z0), sx),
                      math::Lerp(Hash2D(x0, z0 + 1), Hash2D(x0 + 1, z0 + 1), sx), sz);
}

float Fbm(float x, float z, int octaves) {
    float amp = 0.5f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += SmoothNoise(x * freq, z * freq) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.1f;
    }
    return sum / norm;
}

} // namespace

// ---------------------------------------------------------------------------
// TitleScene
// ---------------------------------------------------------------------------

TitleScene::TitleScene(NeonApp& app) : app_(app) {}

void TitleScene::OnEnter() { app_.Window()->SetCaptureMouse(false); }

void TitleScene::Update(float dt) {
    t_ += dt;
    if (app_.Input()->Pressed(platform::Key::Escape)) app_.Quit();
}

void TitleScene::Draw(gfx::Renderer& renderer) {
    float aspect = static_cast<float>(renderer.ScreenWidth()) / renderer.ScreenHeight();
    gfx::Camera cam;
    float angle = t_ * 0.25f;
    cam.position = {std::sin(angle) * 18.0f, 10.0f, std::cos(angle) * 18.0f};
    cam.target = {0, 1.0f, 0};
    renderer.SetCamera(cam, aspect);
    renderer.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.32f);

    const DemoAssets& assets = app_.Assets();
    renderer.DrawMesh(assets.plane, gfx::Material::Lit(assets.ground.Handle()), math::Mat4::Identity());
    for (int i = 0; i < 8; ++i) {
        float a = static_cast<float>(i) / 8.0f * math::kTwoPi;
        math::Mat4 m = math::Mat4::Translation({std::cos(a) * 11.0f, 1.5f, std::sin(a) * 11.0f}) *
                       math::Mat4::Scale({1.4f, 3.0f, 1.4f});
        renderer.DrawMesh(assets.cylinder, gfx::Material::Lit(assets.pillar.Handle()), m);
    }
    math::Mat4 playerM = math::Mat4::RotationY(t_ * 1.2f);
    renderer.DrawMesh(assets.playerMesh,
                      gfx::Material::Lit({}, gfx::Color{0.2f, 0.7f, 1.0f, 1.0f}, 32.0f), playerM);

    ui::Theme& theme = app_.Theme();
    ui::DrawLabel(renderer, theme, "霓虹大陆", {640, 105}, 64, theme.accent, true, true);
    ui::DrawLabel(renderer, theme, "NEON REALM - NeonEngine 3D Demo", {640, 165}, 14, theme.text, true, true);

    if (ui::DrawButton(renderer, theme, "开始冒险", {390, 330, 500, 58}, *app_.Input())) {
        app_.PlaySfx(app_.sfxClick_);
        app_.StartGame();
    }
    if (ui::DrawButton(renderer, theme, "操作说明", {390, 402, 500, 58}, *app_.Input())) {
        app_.PlaySfx(app_.sfxClick_);
        showHelp_ = !showHelp_;
    }
    if (ui::DrawButton(renderer, theme, "退出游戏", {390, 474, 500, 58}, *app_.Input())) {
        app_.PlaySfx(app_.sfxClick_);
        app_.Quit();
    }

    if (showHelp_) {
        ui::DrawPanel(renderer, theme, {240, 210, 800, 360});
        const char* lines[] = {
            "WASD / 方向键   移动",
            "鼠标拖动         旋转视角",
            "左键             近战攻击",
            "右键             冲刺（无敌帧）",
            "空格             跳跃",
            "1 / 2            火球术 / 治疗术",
            "F                与 NPC 对话 / 交接任务",
            "Esc              暂停",
            "",
            "任务：帮助村长猎杀 5 只野狼，获得经验与金币。",
        };
        float y = 240;
        for (const char* line : lines) {
            ui::DrawLabel(renderer, theme, line, {640, y}, 15, theme.text, true, false);
            y += 26;
        }
    }
}

// ---------------------------------------------------------------------------
// GameScene
// ---------------------------------------------------------------------------

GameScene::GameScene(NeonApp& app) : app_(app) {}

GameScene::~GameScene() { app_.Window()->SetCaptureMouse(false); }

void GameScene::OnEnter() {
    if (!app_.SmokeMode()) app_.Window()->SetCaptureMouse(true);
    SetupWorld();
    banner_ = "欢迎来到霓虹大陆";
    bannerTime_ = 3.0f;
}

void GameScene::OnExit() {
    app_.Window()->SetCaptureMouse(false);
    physics_.Clear();
    world_.Clear();
}

float GameScene::TerrainHeight(float x, float z) const {
    float h = Fbm(x * 0.018f, z * 0.018f, 4) * 5.5f +
              Fbm(x * 0.07f + 37.3f, z * 0.07f - 11.7f, 2) * 0.8f;
    float d = std::sqrt(x * x + z * z);
    float flat = math::Saturate((d - 9.0f) / 13.0f); // village area stays flat
    return h * flat;
}

void GameScene::SetupWorld() {
    world_.Clear();
    physics_.Clear();
    rings_.clear();
    particles_.Clear();
    pines_.clear();
    oaks_.clear();
    rocks_.clear();
    logs_.clear();

    const DemoAssets& assets = app_.Assets();

    // Heightfield terrain.
    heights_.assign(static_cast<size_t>(segments_ + 1) * (segments_ + 1), 0.0f);
    float half = worldSize_ * 0.5f;
    for (int row = 0; row <= segments_; ++row) {
        for (int col = 0; col <= segments_; ++col) {
            float x = -half + col * (worldSize_ / segments_);
            float z = -half + row * (worldSize_ / segments_);
            heights_[static_cast<size_t>(row) * (segments_ + 1) + col] = TerrainHeight(x, z);
        }
    }
    gfx::Mesh terrain = gfx::Mesh::CreateTerrain(app_.Renderer(), segments_, worldSize_, heights_,
                                                 heightScale_, "terrain");

    // NPC quest giver in the village.
    npc_ = world_.Create();
    world_.Add<CTransform>(npc_, CTransform{{0, 0, 0}, math::Quat::Identity(), {1, 1, 1}});
    world_.Add<CMesh>(npc_, CMesh{assets.playerMesh,
                                  gfx::Material::Lit({}, gfx::Color{0.85f, 0.6f, 0.35f, 1.0f}, 20.0f)});
    world_.Add<CNPC>(npc_, CNPC{"村长"});

    // Player.
    player_ = world_.Create();
    math::Vec3 playerPos{0, 0.9f, 4.0f};
    world_.Add<CTransform>(player_, CTransform{playerPos, math::Quat::Identity(), {1, 1, 1}});
    world_.Add<CMesh>(player_, CMesh{assets.playerMesh,
                                     gfx::Material::Lit({}, gfx::Color{0.2f, 0.7f, 1.0f, 1.0f}, 32.0f)});
    CPlayer playerComp;
    playerComp.level = app_.save_.GetInt("level", 1);
    playerComp.xp = app_.save_.GetInt("xp", 0);
    playerComp.gold = app_.save_.GetInt("gold", 0);
    playerComp.maxMana = 50.0f + (playerComp.level - 1) * 8.0f;
    playerComp.mana = playerComp.maxMana;
    world_.Add<CPlayer>(player_, playerComp);
    float maxHp = 100.0f + (playerComp.level - 1) * 15.0f;
    world_.Add<CHealth>(player_, CHealth{maxHp, maxHp});
    world_.Add<CRigidBody>(player_, CRigidBody{physics_.AddSphere(EncodeEntity(player_), playerPos, 0.45f, true)});

    // Wolves: real skinned glTF model + animation (fallback to the procedural
    // box wolf if the glTF failed to load).
    const bool wolfOk = !assets.wolfGltf.nodes.empty() &&
                        assets.wolfGltf.nodes[0].mesh.Skinned();
    wolfParts_.clear();
    if (wolfOk) {
        for (const assets::GltfMeshNode& node : assets.wolfGltf.nodes) {
            // Skip flat ground-disc nodes (e.g. Blender's shadow/ground plane).
            const math::AABB& b = node.mesh.Bounds();
            if (b.max.y - b.min.y < 0.05f) continue;
            wolfParts_.push_back(node);
        }
        for (const anim::AnimationClip& clip : assets.wolfAnim.clips) {
            if (clip.name.find("01_Run") != std::string::npos) wolfRunClip_ = &clip;
            if (clip.name.find("04_Idle") != std::string::npos) wolfIdleClip_ = &clip;
        }
        if (wolfRunClip_ == nullptr && !assets.wolfAnim.clips.empty()) {
            wolfRunClip_ = &assets.wolfAnim.clips[0];
        }
        if (wolfIdleClip_ == nullptr) wolfIdleClip_ = wolfRunClip_;
        NEON_LOG_INFO("Wolf: %zu render parts, run=%s idle=%s", wolfParts_.size(),
                      wolfRunClip_ ? wolfRunClip_->name.c_str() : "?",
                      wolfIdleClip_ ? wolfIdleClip_->name.c_str() : "?");
    }
    const gfx::Mesh wolfMesh =
        wolfOk ? assets.wolfGltf.nodes[0].mesh : assets.wolfMesh;
    const gfx::Material wolfMat =
        wolfOk ? assets.wolfGltf.nodes[0].material
               : gfx::Material::Lit({}, gfx::Color{0.5f, 0.4f, 0.32f, 1.0f}, 14.0f);
    const anim::Skeleton& wolfSkel = assets.wolfAnim.skeleton;
    for (int i = 0; i < 9; ++i) {
        float a = rng_.Range(0.0f, math::kTwoPi);
        float r = rng_.Range(22.0f, 62.0f);
        math::Vec3 home{std::cos(a) * r, 0.0f, std::sin(a) * r};
        home.y = TerrainHeight(home.x, home.z);
        if (home.y < 0.2f) home.y = 0.2f;
        ecs::Entity e = world_.Create();
        math::Vec3 pos = home;
        pos.y = home.y;
        world_.Add<CTransform>(e, CTransform{pos, math::Quat::Identity(), {1, 1, 1}});
        world_.Add<CMesh>(e, CMesh{wolfMesh, wolfMat});
        world_.Add<CHealth>(e, CHealth{45.0f, 45.0f});
        CEnemy ce;
        ce.home = home;
        if (wolfOk) {
            ce.animTime = rng_.Range(
                0.0f, assets.wolfAnim.clips.empty() ? 1.0f
                                                     : assets.wolfAnim.clips[0].duration);
            ce.bones.resize(wolfSkel.bones.size());
            ce.bones = wolfSkel.ComputeBoneMatrices(wolfSkel.BindPose());
        }
        world_.Add<CEnemy>(e, ce);
        world_.Add<CRigidBody>(e, CRigidBody{physics_.AddSphere(EncodeEntity(e), pos, 0.6f, true)});
    }

    // Instanced scenery from Kenney (CC0) models. Keep the village plaza and
    // the spawn point clear so the play area stays open and readable.
    const math::ExclusionZone exclusionZones[] = {
        {0.0f, 0.0f, 26.0f}, // village plaza (NPC, flag, helmet, play space)
        {0.0f, 4.0f, 6.0f},  // player spawn
    };
    const int kExclusionZoneCount = static_cast<int>(sizeof(exclusionZones) / sizeof(exclusionZones[0]));

    // Each category enforces a minimum spacing from every already-placed prop
    // (same or different category), so props never overlap or cluster on the
    // flat ground the height rejection leaves behind. O(n^2) over <= ~200
    // placements is fine and simpler than a spatial hash grid at this scale.
    // Candidates are drawn in a fixed order (angle, radius, scale on
    // acceptance) so the whole pass is reproducible run-to-run.
    std::vector<math::Vec2> outPos;
    auto scatter = [&](const gfx::Mesh& mesh, std::vector<math::Mat4>& out, int count,
                       float minR, float maxR, float minS, float maxS, float maxY,
                       float minSpacing) {
        const math::AABB& b = mesh.Bounds();
        float baseY = -b.min.y;
        for (int i = 0; i < count; ++i) {
            bool placed = false;
            for (int attempt = 0; attempt < 64 && !placed; ++attempt) {
                float a = rng_.Range(0.0f, math::kTwoPi);
                float r = rng_.Range(minR, maxR);
                math::Vec2 xz{std::cos(a) * r, std::sin(a) * r};
                if (math::InExclusionZones(xz, exclusionZones, kExclusionZoneCount)) continue;
                float groundY = TerrainHeight(xz.x, xz.y);
                if (groundY > maxY) continue;
                if (math::TooCloseToAny(xz, outPos.data(), static_cast<int>(outPos.size()),
                                        minSpacing))
                    continue;
                float s = rng_.Range(minS, maxS);
                math::Mat4 m = math::Mat4::Translation({xz.x, groundY + baseY * s, xz.y}) *
                               math::Mat4::RotationY(rng_.Range(0.0f, math::kTwoPi)) *
                               math::Mat4::Scale({s, s, s});
                out.push_back(m);
                outPos.push_back(xz);
                placed = true;
            }
        }
    };
    scatter(app_.assets_.kenneyPine, pines_, 70, 26.0f, 96.0f, 2.2f, 3.6f, 8.0f, 3.2f);
    scatter(app_.assets_.kenneyOak, oaks_, 28, 26.0f, 96.0f, 2.0f, 3.6f, 8.0f, 4.0f);
    scatter(app_.assets_.kenneyRock, rocks_, 18, 26.0f, 96.0f, 0.5f, 1.5f, 6.0f, 2.6f);
    scatter(app_.assets_.kenneyLog, logs_, 8, 12.0f, 42.0f, 0.8f, 1.4f, 3.0f, 3.0f);
    NEON_LOG_CAT(neon::core::LogCategory::Game, neon::core::LogLevel::Info,
                 "scenery placed: pines=%d oaks=%d rocks=%d logs=%d",
                 static_cast<int>(pines_.size()), static_cast<int>(oaks_.size()),
                 static_cast<int>(rocks_.size()), static_cast<int>(logs_.size()));

    // GPU-skinned demo flag rig: bone 0 = static pole (pins the bottom edge),
    // bone 1 = child rotating the top of the flag around the local Z axis.
    {
        flagSkeleton_.bones.resize(2);
        flagSkeleton_.bones[0].name = "flagRoot";
        flagSkeleton_.bones[0].parent = -1;
        flagSkeleton_.bones[0].inverseBind = math::Mat4::Identity();
        flagSkeleton_.bones[1].name = "flagTop";
        flagSkeleton_.bones[1].parent = 0;
        flagSkeleton_.bones[1].inverseBind = math::Mat4::Identity();
        flagPose_ = flagSkeleton_.BindPose();
        flagBones_ = flagSkeleton_.ComputeBoneMatrices(flagPose_);
    }

    // Terrain is drawn directly; keep a static entity so it is part of the scene.
    ecs::Entity terrainEntity = world_.Create();
    world_.Add<CTransform>(terrainEntity, CTransform{{0, 0, 0}, math::Quat::Identity(), {1, 1, 1}});
    world_.Add<CMesh>(terrainEntity, CMesh{terrain, gfx::Material::Lit({}, gfx::Color::White, 4.0f)});
}

void GameScene::MeleeAttack() {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CTransform* pt = world_.Get<CTransform>(player_);
    if (!player || !pt) return;
    math::Vec3 forward = pt->rot.Rotate(math::Vec3::Forward());
    math::Vec3 origin = pt->pos + math::Vec3{0, 1.2f, 0};

    auto view = world_.ViewAll<CEnemy>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CEnemy>(i);
        CTransform* et = world_.Get<CTransform>(e);
        CHealth* eh = world_.Get<CHealth>(e);
        CEnemy* ce = world_.Get<CEnemy>(e);
        CRigidBody* rb = world_.Get<CRigidBody>(e);
        if (!et || !eh || !ce || ce->dead) continue;
        math::Vec3 toEnemy = et->pos - origin;
        toEnemy.y = 0.0f;
        float dist = toEnemy.Length();
        if (dist > 3.4f) continue;
        toEnemy = toEnemy * (1.0f / dist);
        if (math::Dot(forward, toEnemy) < std::cos(60.0f * math::kDegToRad)) continue;

        eh->hp -= 38.0f;
        ce->aggro = true;
        app_.PlaySfx(app_.sfxHit_, 0.8f);
        gfx::EmitterConfig sparks;
        sparks.count = 10;
        sparks.position = et->pos + math::Vec3{0, 1.0f, 0};
        sparks.speedMin = 2.0f;
        sparks.speedMax = 6.0f;
        sparks.lifeMin = 0.25f;
        sparks.lifeMax = 0.5f;
        sparks.sizeStart = 0.3f;
        sparks.sizeEnd = 0.05f;
        sparks.colorStart = {1.0f, 0.85f, 0.4f, 1.0f};
        sparks.colorEnd = {1.0f, 0.4f, 0.1f, 0.0f};
        particles_.Emit(sparks);
        if (rb) physics_.SetVelocity(rb->body, toEnemy * 4.0f + math::Vec3{0, 2.5f, 0});
        if (eh->hp <= 0.0f) KillMob(e);
    }

    gfx::EmitterConfig slash;
    slash.count = 12;
    slash.position = origin + forward * 1.6f;
    slash.baseVelocity = forward * 3.0f;
    slash.speedMin = 0.5f;
    slash.speedMax = 2.5f;
    slash.lifeMin = 0.15f;
    slash.lifeMax = 0.3f;
    slash.sizeStart = 0.3f;
    slash.sizeEnd = 0.05f;
    slash.colorStart = {0.6f, 0.9f, 1.0f, 1.0f};
    slash.colorEnd = {0.6f, 0.9f, 1.0f, 0.0f};
    particles_.Emit(slash);
}

void GameScene::Fireball() {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CTransform* pt = world_.Get<CTransform>(player_);
    if (!player || !pt || player->mana < 12.0f) return;
    player->mana -= 12.0f;
    player->fireCd = 2.0f;
    app_.PlaySfx(app_.sfxFireball_);

    math::Vec3 fwd = pt->rot.Rotate(math::Vec3::Forward());
    ecs::Entity e = world_.Create();
    math::Vec3 pos = pt->pos + fwd * 1.3f + math::Vec3{0, 1.2f, 0};
    world_.Add<CTransform>(e, CTransform{pos, math::Quat::Identity(), {0.2f, 0.2f, 0.2f}});
    world_.Add<CMesh>(e, CMesh{app_.Assets().sphere,
                               gfx::Material::Unlit({}, gfx::Color{1.0f, 0.5f, 0.15f, 1.0f})});
    CProjectile proj;
    proj.vel = fwd * 24.0f;
    proj.damage = 28.0f;
    world_.Add<CProjectile>(e, proj);
}

void GameScene::Heal() {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CHealth* health = world_.Get<CHealth>(player_);
    CTransform* pt = world_.Get<CTransform>(player_);
    if (!player || !health || player->mana < 15.0f || health->hp >= health->maxHp) return;
    player->mana -= 15.0f;
    player->healCd = 5.0f;
    health->hp = std::min(health->maxHp, health->hp + 35.0f);
    app_.PlaySfx(app_.sfxPickup_);
    if (pt) {
        gfx::EmitterConfig healFx;
        healFx.count = 18;
        healFx.position = pt->pos + math::Vec3{0, 1.2f, 0};
        healFx.speedMin = 1.0f;
        healFx.speedMax = 3.5f;
        healFx.lifeMin = 0.3f;
        healFx.lifeMax = 0.7f;
        healFx.sizeStart = 0.4f;
        healFx.sizeEnd = 0.05f;
        healFx.colorStart = {0.4f, 1.0f, 0.6f, 1.0f};
        healFx.colorEnd = {0.4f, 1.0f, 0.6f, 0.0f};
        healFx.gravity = 2.0f;
        particles_.Emit(healFx);
    }
}

void GameScene::KillMob(ecs::Entity enemy) {
    CTransform* et = world_.Get<CTransform>(enemy);
    CEnemy* ce = world_.Get<CEnemy>(enemy);
    CPlayer* player = world_.Get<CPlayer>(player_);
    CRigidBody* rb = world_.Get<CRigidBody>(enemy);
    CMesh* mesh = world_.Get<CMesh>(enemy);
    if (!et || !ce || !player) return;

    math::Vec3 pos = et->pos;
    int goldDrop = rng_.Int(2, 6);
    player->gold += goldDrop;
    GiveXp(14);
    if (questAccepted_) {
        ++questKills_;
        if (questKills_ >= 5) {
            banner_ = "任务完成！回去找村长领取奖励";
            bannerTime_ = 3.0f;
        }
    }

    app_.PlaySfx(app_.sfxExplosion_);
    gfx::EmitterConfig boom;
    boom.count = 20;
    boom.position = pos;
    boom.speedMin = 2.0f;
    boom.speedMax = 7.0f;
    boom.lifeMin = 0.3f;
    boom.lifeMax = 0.8f;
    boom.sizeStart = 0.5f;
    boom.sizeEnd = 0.05f;
    boom.colorStart = {0.9f, 0.35f, 0.2f, 1.0f};
    boom.colorEnd = {0.5f, 0.15f, 0.1f, 0.0f};
    boom.gravity = -3.0f;
    particles_.Emit(boom);
    rings_.push_back({pos, 0.0f, 0.45f, {1.0f, 0.4f, 0.25f, 1.0f}});

    ce->dead = true;
    ce->respawnTimer = 12.0f;
    ce->aggro = false;
    if (mesh) mesh->visible = false;
    if (rb) physics_.SetEnabled(rb->body, false);
}

void GameScene::GiveXp(int amount) {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CHealth* health = world_.Get<CHealth>(player_);
    if (!player || !health) return;
    player->xp += amount;
    int toNext = 50 + player->level * 30;
    if (player->xp >= toNext) {
        player->xp -= toNext;
        ++player->level;
        health->maxHp += 15.0f;
        health->hp = health->maxHp;
        player->maxMana += 8.0f;
        player->mana = player->maxMana;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "等级提升！Lv.%d", player->level);
        banner_ = buf;
        bannerTime_ = 3.0f;
        app_.PlaySfx(app_.sfxWave_);
    }
    app_.UpdateSave(player->level, player->xp, player->gold);
}

void GameScene::UpdatePlayer(float dt) {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CTransform* pt = world_.Get<CTransform>(player_);
    CHealth* ph = world_.Get<CHealth>(player_);
    CRigidBody* rb = world_.Get<CRigidBody>(player_);
    if (!player || !pt || !ph || !rb || !player->alive) return;

    platform::IInput* input = app_.Input();
    player->dashCooldown -= dt;
    player->attackCd -= dt;
    player->iframes -= dt;
    player->swingTimer -= dt;
    player->dashTime -= dt;
    player->fireCd -= dt;
    player->healCd -= dt;
    player->mana = std::min(player->maxMana, player->mana + 3.0f * dt);

    float ix = (input->IsDown(platform::Key::D) ? 1.0f : 0.0f) -
               (input->IsDown(platform::Key::A) ? 1.0f : 0.0f);
    float iz = (input->IsDown(platform::Key::W) ? 1.0f : 0.0f) -
               (input->IsDown(platform::Key::S) ? 1.0f : 0.0f);
    float horiz = (input->IsDown(platform::Key::ArrowRight) ? 1.0f : 0.0f) -
                  (input->IsDown(platform::Key::ArrowLeft) ? 1.0f : 0.0f);
    float vert = (input->IsDown(platform::Key::ArrowUp) ? 1.0f : 0.0f) -
                 (input->IsDown(platform::Key::ArrowDown) ? 1.0f : 0.0f);
    ix += horiz;
    iz += vert;

    math::Vec3 camFwd{-std::sin(yaw_), 0.0f, -std::cos(yaw_)};
    math::Vec3 camRight{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
    math::Vec3 dir = camFwd * iz + camRight * ix;
    if (dir.LengthSq() > 1.0f) dir = dir.Normalized();

    math::Vec3 dashDir = dir;
    if (input->MousePressed(platform::MouseButton::Right) && player->dashCooldown <= 0.0f &&
        dir.LengthSq() > 0.01f) {
        player->dashTime = 0.18f;
        player->dashCooldown = 2.5f;
        player->iframes = std::max(player->iframes, 0.35f);
        app_.PlaySfx(app_.sfxDash_);
    }

    math::Vec3 moveDir = player->dashTime > 0.0f ? dashDir : dir;
    float speed = player->dashTime > 0.0f ? player->speed * 3.4f : player->speed;
    math::Vec3 vel = physics_.GetVelocity(rb->body);
    vel.x = moveDir.x * speed;
    vel.z = moveDir.z * speed;
    if (input->Pressed(platform::Key::Space) && physics_.IsOnGround(rb->body)) vel.y = 8.0f;
    physics_.SetVelocity(rb->body, vel);

    // Smoothly turn the character toward the movement direction (camera-relative:
    // pressing W faces "camera forward"). When idle the last facing is kept.
    if (dir.LengthSq() > 0.01f) {
        float target = std::atan2(dir.x, dir.z);
        float diff = math::WrapAngle(target - facingYaw_);
        facingYaw_ += math::Approach(diff, 0.0f, 12.0f * dt);
        pt->rot = math::Quat::FromEuler(0.0f, facingYaw_, 0.0f);
    }

    if (input->MousePressed(platform::MouseButton::Left) && player->attackCd <= 0.0f) {
        player->attackCd = 0.4f;
        player->swingTimer = 0.18f;
        app_.PlaySfx(app_.sfxSwing_);
        MeleeAttack();
    }
    if (input->Pressed(platform::Key::D1) && player->fireCd <= 0.0f) Fireball();
    if (input->Pressed(platform::Key::D2) && player->healCd <= 0.0f) Heal();

    trailTimer_ -= dt;
    if (moveDir.LengthSq() > 0.1f && trailTimer_ <= 0.0f) {
        trailTimer_ = 0.035f;
        gfx::EmitterConfig trail;
        trail.count = 1;
        trail.position = pt->pos + math::Vec3{0, 0.2f, 0};
        trail.baseVelocity = -moveDir * 1.5f + math::Vec3{0, 1.0f, 0};
        trail.speedMin = 0.0f;
        trail.speedMax = 0.5f;
        trail.lifeMin = 0.25f;
        trail.lifeMax = 0.4f;
        trail.sizeStart = 0.3f;
        trail.sizeEnd = 0.03f;
        trail.colorStart = {0.3f, 0.8f, 1.0f, 0.8f};
        trail.colorEnd = {0.3f, 0.8f, 1.0f, 0.0f};
        trail.gravity = -1.0f;
        particles_.Emit(trail);
    }

    math::Vec3 p = physics_.GetPosition(rb->body);
    float limit = worldSize_ * 0.48f;
    p.x = math::Clamp(p.x, -limit, limit);
    p.z = math::Clamp(p.z, -limit, limit);
    physics_.SetPosition(rb->body, p);
    pt->pos = physics_.GetPosition(rb->body);
}

void GameScene::UpdateMobs(float dt) {
    CTransform* pt = world_.Get<CTransform>(player_);
    math::Vec3 playerPos = pt ? pt->pos : math::Vec3{};

    auto view = world_.ViewAll<CEnemy>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CEnemy>(i);
        CEnemy& ce = view[i];
        CTransform* et = world_.Get<CTransform>(e);
        CHealth* eh = world_.Get<CHealth>(e);
        CRigidBody* rb = world_.Get<CRigidBody>(e);
        CMesh* mesh = world_.Get<CMesh>(e);
        if (!et || !eh || !rb) continue;

        if (ce.dead) {
            ce.respawnTimer -= dt;
            if (ce.respawnTimer <= 0.0f) {
                ce.dead = false;
                ce.aggro = false;
                eh->hp = eh->maxHp;
                et->pos = ce.home;
                physics_.SetPosition(rb->body, ce.home);
                physics_.SetEnabled(rb->body, true);
                if (mesh) mesh->visible = true;
            }
            continue;
        }

        ce.attackCd -= dt;
        ce.bobPhase += dt * 8.0f;
        // Advance the skinned wolf animation (faster while chasing).
        const anim::AnimationClip* clip = ce.aggro ? wolfRunClip_ : wolfIdleClip_;
        if (!ce.bones.empty() && clip) {
            ce.animTime += dt * (ce.aggro ? 1.2f : 1.0f);
            const anim::Skeleton& skel = app_.Assets().wolfAnim.skeleton;
            if (ce.bones.size() == skel.bones.size()) {
                anim::Pose pose = skel.BindPose();
                clip->Sample(ce.animTime, pose);
                ce.bones = skel.ComputeBoneMatrices(pose);
            }
        }
        math::Vec3 toPlayer = playerPos - et->pos;
        toPlayer.y = 0.0f;
        float distPlayer = toPlayer.Length();
        math::Vec3 toHome = ce.home - et->pos;
        toHome.y = 0.0f;
        float distHome = toHome.Length();

        bool chase = playerPos.y > -20.0f && distPlayer < 14.0f && distHome < 30.0f;
        ce.aggro = chase;
        if (chase) {
            math::Vec3 dir = toPlayer * (1.0f / distPlayer);
            physics_.SetVelocity(rb->body, {dir.x * 5.5f, 0.0f, dir.z * 5.5f});
            et->rot = FaceDirection(dir);
            if (distPlayer < 1.7f && ce.attackCd <= 0.0f) {
                ce.attackCd = 1.2f;
                DamagePlayer(8.0f);
            }
        } else if (distHome > 1.0f) {
            math::Vec3 dir = toHome * (1.0f / distHome);
            physics_.SetVelocity(rb->body, {dir.x * 3.0f, 0.0f, dir.z * 3.0f});
            et->rot = FaceDirection(dir);
        } else {
            physics_.SetVelocity(rb->body, {0, 0, 0});
        }

        math::Vec3 p = physics_.GetPosition(rb->body);
        float groundY = TerrainHeight(p.x, p.z);
        if (groundY < 0.1f) groundY = 0.1f;
        p.y = groundY + 0.55f + std::sin(ce.bobPhase) * 0.06f;
        physics_.SetPosition(rb->body, p);
        et->pos = p;
    }
}

void GameScene::UpdateProjectiles(float dt) {
    auto view = world_.ViewAll<CProjectile>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CProjectile>(i);
        CProjectile& proj = view[i];
        CTransform* pt = world_.Get<CTransform>(e);
        if (!pt) continue;
        proj.life -= dt;
        pt->pos += proj.vel * dt;

        bool hit = false;
        auto mobs = world_.ViewAll<CEnemy>();
        for (size_t m = 0; m < mobs.Size(); ++m) {
            ecs::Entity mob = world_.EntityAt<CEnemy>(m);
            CEnemy* ce = world_.Get<CEnemy>(mob);
            CTransform* mt = world_.Get<CTransform>(mob);
            CHealth* mh = world_.Get<CHealth>(mob);
            if (!ce || !mt || !mh || ce->dead) continue;
            if ((mt->pos - pt->pos).LengthSq() < 1.8f) {
                mh->hp -= proj.damage;
                ce->aggro = true;
                hit = true;
                gfx::EmitterConfig impact;
                impact.count = 12;
                impact.position = pt->pos;
                impact.speedMin = 1.5f;
                impact.speedMax = 5.0f;
                impact.lifeMin = 0.2f;
                impact.lifeMax = 0.45f;
                impact.sizeStart = 0.35f;
                impact.sizeEnd = 0.05f;
                impact.colorStart = {1.0f, 0.6f, 0.2f, 1.0f};
                impact.colorEnd = {1.0f, 0.25f, 0.1f, 0.0f};
                particles_.Emit(impact);
                if (mh->hp <= 0.0f) KillMob(mob);
                break;
            }
        }
        if (hit || proj.life <= 0.0f) world_.Destroy(e);
    }
}

void GameScene::UpdateNPC(float dt) {
    CTransform* nt = world_.Get<CTransform>(npc_);
    CTransform* pt = world_.Get<CTransform>(player_);
    if (!nt || !pt) return;
    float dist = (nt->pos - pt->pos).Length();

    if (app_.Input()->Pressed(platform::Key::F) && dist < 3.5f && !dead_) {
        dialogueOpen_ = !dialogueOpen_;
        app_.Window()->SetCaptureMouse(dialogueOpen_ ? false : true);
        if (dialogueOpen_) app_.PlaySfx(app_.sfxClick_);
    }
    if (dialogueOpen_ && dist > 6.0f) {
        dialogueOpen_ = false;
        app_.Window()->SetCaptureMouse(true);
    }
}

void GameScene::UpdateCamera(float dt) {
    CTransform* pt = world_.Get<CTransform>(player_);
    math::Vec3 playerPos = pt ? pt->pos : math::Vec3{};
    playerPos.y = std::max(playerPos.y, TerrainHeight(playerPos.x, playerPos.z) + 0.4f);

    if (!paused_ && !dialogueOpen_ && !dead_) {
        platform::IInput* input = app_.Input();
        yaw_ += -input->MouseDelta().x * 0.004f;
        pitch_ += -input->MouseDelta().y * 0.004f;
        pitch_ = math::Clamp(pitch_, -0.15f, 1.25f);
        float wheel = input->WheelDelta();
        if (std::fabs(wheel) > 0.01f) camDist_ = math::Clamp(camDist_ - wheel * 1.5f, 4.0f, 18.0f);
    }
    if (dead_) yaw_ += dt * 0.35f;

    math::Vec3 offset{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
                      std::cos(yaw_) * std::cos(pitch_)};
    math::Vec3 camPos = playerPos + math::Vec3{0, 1.1f, 0} + offset * camDist_;
    if (shakeTime_ > 0.0f) {
        shakeTime_ -= dt;
        camPos += rng_.OnUnitSphere() * shakeMag_ * (shakeTime_ / 0.35f);
    }
    camera_.position = camPos;
    camera_.target = playerPos + math::Vec3{0, 1.5f, 0};
    camera_.up = {0, 1, 0};
}

void GameScene::UpdateDayNight(float dt) {
    dayTime_ += dt;
    if (dayTime_ > 90.0f) dayTime_ -= 90.0f;
}

void GameScene::UpdateFlag(float dt) {
    flagAnimTime_ += dt;
    // Rotate the top bone around the flag's local Z axis (FromEuler's first
    // arg is yaw = Z); the weight gradient (pinned bottom -> free top) bends
    // the ribbon like a waving banner.
    float angle = std::sin(flagAnimTime_ * 1.6f) * 0.7f;
    flagPose_.r[1] = math::Quat::FromEuler(angle, 0.0f, 0.0f);
    flagBones_ = flagSkeleton_.ComputeBoneMatrices(flagPose_);
}

void GameScene::UpdateRings(float dt) {
    for (Ring& ring : rings_) ring.t += dt;
    rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
                                [](const Ring& r) { return r.t >= r.maxT; }),
                 rings_.end());
}

void GameScene::DamagePlayer(float amount) {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CHealth* health = world_.Get<CHealth>(player_);
    if (!player || !health || !player->alive) return;
    if (player->iframes > 0.0f || player->dashTime > 0.0f) return;
    player->iframes = 0.8f;
    health->hp -= amount;
    hurtFlash_ = 1.0f;
    shakeTime_ = 0.3f;
    shakeMag_ = 0.22f;
    app_.PlaySfx(app_.sfxHurt_);
    if (health->hp <= 0.0f) {
        player->alive = false;
        dead_ = true;
        deadTimer_ = 1.5f;
        app_.Window()->SetCaptureMouse(false);
        app_.PlaySfx(app_.sfxGameOver_);
        app_.UpdateSave(player->level, player->xp, player->gold);
        CTransform* pt = world_.Get<CTransform>(player_);
        if (pt) {
            gfx::EmitterConfig boom;
            boom.count = 34;
            boom.position = pt->pos + math::Vec3{0, 1.0f, 0};
            boom.speedMin = 3.0f;
            boom.speedMax = 9.0f;
            boom.lifeMin = 0.4f;
            boom.lifeMax = 1.0f;
            boom.sizeStart = 0.6f;
            boom.sizeEnd = 0.05f;
            boom.colorStart = {0.4f, 0.8f, 1.0f, 1.0f};
            boom.colorEnd = {0.2f, 0.4f, 1.0f, 0.0f};
            boom.gravity = -4.0f;
            particles_.Emit(boom);
        }
    }
}

void GameScene::RespawnPlayer() {
    CPlayer* player = world_.Get<CPlayer>(player_);
    CHealth* health = world_.Get<CHealth>(player_);
    CTransform* pt = world_.Get<CTransform>(player_);
    CRigidBody* rb = world_.Get<CRigidBody>(player_);
    if (!player || !health || !pt || !rb) return;
    player->alive = true;
    health->hp = health->maxHp;
    player->mana = player->maxMana;
    pt->pos = {0, 0.9f, 4.0f};
    physics_.SetPosition(rb->body, pt->pos);
    physics_.SetVelocity(rb->body, {0, 0, 0});
    dead_ = false;
    dialogueOpen_ = false;
    if (!app_.SmokeMode()) app_.Window()->SetCaptureMouse(true);
}

void GameScene::Update(float dt) {
    if (bannerTime_ > 0.0f) bannerTime_ -= dt;
    hurtFlash_ = std::max(0.0f, hurtFlash_ - dt * 2.0f);
    UpdateRings(dt);
    UpdateFlag(dt);
    particles_.Update(dt);
    UpdateDayNight(dt);
    UpdateCamera(dt);

    if (dead_) {
        deadTimer_ -= dt;
        return;
    }

    if (app_.Input()->Pressed(platform::Key::Escape) && !dialogueOpen_) {
        paused_ = !paused_;
        app_.Window()->SetCaptureMouse(!paused_);
        if (!paused_) app_.PlaySfx(app_.sfxClick_);
    }
    UpdateNPC(dt);
    if (paused_ || dialogueOpen_) return;

    UpdatePlayer(dt);
    UpdateMobs(dt);
    UpdateProjectiles(dt);
    physics_.Step(dt, {0, -20.0f, 0});
}

math::Vec2 GameScene::WorldToScreen(gfx::Renderer& renderer, const math::Vec3& world) const {
    float aspect = static_cast<float>(renderer.ScreenWidth()) / renderer.ScreenHeight();
    math::Vec4 clip = camera_.ViewProjection(aspect).TransformVec4(
        math::Vec4(world.x, world.y, world.z, 1.0f));
    if (clip.w <= 0.01f) return {-9999, -9999};
    float px = (clip.x / clip.w * 0.5f + 0.5f) * renderer.ScreenWidth();
    float py = (0.5f - clip.y / clip.w * 0.5f) * renderer.ScreenHeight();
    return renderer.ScreenToUI({px, py});
}

void GameScene::Draw(gfx::Renderer& renderer) {
    float aspect = static_cast<float>(renderer.ScreenWidth()) / renderer.ScreenHeight();
    renderer.SetCamera(camera_, aspect);

    // Day/night cycle.
    float cycle = dayTime_ / 90.0f;
    float dayFactor = math::Clamp(std::cos(cycle * math::kTwoPi) * 1.25f, 0.06f, 1.0f);
    gfx::Color skyTop = gfx::Lerp(gfx::Color{0.015f, 0.025f, 0.07f, 1.0f},
                                  gfx::Color{0.22f, 0.48f, 0.8f, 1.0f}, dayFactor);
    gfx::Color horizon = gfx::Lerp(gfx::Color{0.06f, 0.09f, 0.18f, 1.0f},
                                   gfx::Color{0.55f, 0.7f, 0.88f, 1.0f}, dayFactor);
    gfx::Color fog = horizon; // distant scenery fades into the horizon exactly
    renderer.SetSky(skyTop, horizon);
    // Fog spans the scenery ring (minR 12 .. maxR 96): props past ~45 units
    // visibly soften toward the horizon color, the village stays crisp.
    renderer.SetFog(fog, 45.0f, 170.0f);
    renderer.SetDirectionalLight({-0.4f, -1.0f, -0.3f},
                                 gfx::Color{1.0f, 0.92f, 0.78f, 1.0f}.Multiplied(dayFactor),
                                 0.16f + 0.18f * dayFactor);

    CTransform* pt = world_.Get<CTransform>(player_);
    if (pt) renderer.SetPlayerLight(pt->pos + math::Vec3{0, 1.6f, 0}, {0.25f, 0.55f, 1.0f}, 16.0f);

    // Village torch / campfire: a static warm point light above the NPC. With
    // point-light cubemap shadows (Task 3.5) its light casts a visible shadow
    // of the NPC/player onto the flat village ground.
    renderer.SetPointLight(0, {0.0f, 3.0f, -1.0f}, {1.6f, 0.95f, 0.5f, 1.0f}, 10.0f);

    const DemoAssets& assets = app_.Assets();

    math::Vec3 sunDir{-0.4f, -1.0f, -0.3f};
    sunDir = sunDir.Normalized();

    // 1. Terrain first (shadow receiver).
    {
        auto meshesT = world_.ViewAll<CMesh>();
        for (size_t i = 0; i < meshesT.Size(); ++i) {
            ecs::Entity e = world_.EntityAt<CMesh>(i);
            CMesh* cm = world_.Get<CMesh>(e);
            CTransform* tf = world_.Get<CTransform>(e);
            if (cm && tf && cm->mesh.Name() == "terrain") {
                renderer.DrawMesh(cm->mesh, cm->mat, tf->Model());
                break;
            }
        }
    }

    // 2. Instanced scenery (trees/rocks/logs).
    // The demo's Kenney scenery is single-LOD: each type draws one mesh for
    // every instance regardless of distance. LOD asset chains (Task 5.3) are
    // wired for data-driven scenes — a SceneMesh with a `lod` list is resolved
    // into a gfx::LodChain and its level picked per frame by camera distance by
    // scene::GameRuntime::Draw — but the demo's instanced props are drawn
    // directly here (not through GameRuntime) and have no low-poly variants, so
    // they stay single-LOD by design. Packed games / editor playtest get LOD
    // for free via GameRuntime.
    renderer.DrawMeshInstanced(assets.kenneyPine, gfx::Material::Lit({}, gfx::Color::White, 8.0f),
                               pines_.data(), static_cast<uint32_t>(pines_.size()));
    renderer.DrawMeshInstanced(assets.kenneyOak, gfx::Material::Lit({}, gfx::Color::White, 8.0f),
                               oaks_.data(), static_cast<uint32_t>(oaks_.size()));
    renderer.DrawMeshInstanced(assets.kenneyRock, gfx::Material::Lit({}, gfx::Color::White, 6.0f),
                               rocks_.data(), static_cast<uint32_t>(rocks_.size()));
    renderer.DrawMeshInstanced(assets.kenneyLog, gfx::Material::Lit({}, gfx::Color::White, 6.0f),
                               logs_.data(), static_cast<uint32_t>(logs_.size()));

    // DamagedHelmet - real glTF model with PBR textures (CC-BY 4.0, Khronos).
    for (const assets::GltfMeshNode& node : app_.helmet_.nodes) {
        math::Mat4 display = math::Mat4::Translation({8.0f, 0.9f, -5.0f}) *
                             math::Mat4::RotationY(-0.6f) * node.transform;
        renderer.DrawMesh(node.mesh, node.material, display);
    }

    // GPU-skinned demo: waving flag, bone matrices recomputed in Update().
    if (!flagBones_.empty()) {
        math::Mat4 flagM = math::Mat4::Translation({2.6f, 0.0f, 1.2f});
        renderer.DrawSkinnedMesh(assets.flagMesh,
                                 gfx::Material::Lit({}, gfx::Color::White, 16.0f), flagM,
                                 flagBones_, static_cast<int>(flagBones_.size()));
    }

    // 4. Remaining entity meshes (player, mobs, NPC, projectiles), painter-sorted.
    std::vector<std::pair<float, ecs::Entity>> drawList;
    auto meshes = world_.ViewAll<CMesh>();
    for (size_t i = 0; i < meshes.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CMesh>(i);
        CTransform* tf = world_.Get<CTransform>(e);
        const CMesh& cm = meshes[i];
        if (!tf || !cm.visible) continue;
        if (cm.mesh.Name() == "terrain") continue; // already drawn
        float distSq = (tf->pos - camera_.position).LengthSq();
        drawList.emplace_back(distSq, e);
    }
    std::sort(drawList.begin(), drawList.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [distSq, e] : drawList) {
        (void)distSq;
        CMesh* cm = world_.Get<CMesh>(e);
        CTransform* tf = world_.Get<CTransform>(e);
        if (!cm || !tf) continue;
        CEnemy* ce = world_.Get<CEnemy>(e);
        if (ce && !ce->bones.empty() && cm->mesh.Skinned()) {
            const math::Mat4 model = tf->Model();
            for (const assets::GltfMeshNode& part : wolfParts_) {
                renderer.DrawSkinnedMesh(part.mesh, part.material, model, ce->bones,
                                         static_cast<int>(ce->bones.size()));
            }
        } else {
            renderer.DrawMesh(cm->mesh, cm->mat, tf->Model());
        }
    }

    // Note: water plane removed - transparent geometry needs a depth buffer,
    // which this Intel driver does not provide (detected at startup).

    // Fireball glow + point light.
    auto projectiles = world_.ViewAll<CProjectile>();
    for (size_t i = 0; i < projectiles.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CProjectile>(i);
        CTransform* ptProj = world_.Get<CTransform>(e);
        if (!ptProj) continue;
        renderer.DrawBillboard(ptProj->pos, 1.0f, {1.0f, 0.45f, 0.15f, 1.0f}, assets.glow.Handle());
        renderer.SetPointLight(0, ptProj->pos, {1.0f, 0.4f, 0.15f, 1.0f}, 9.0f);
    }

    // NPC quest marker.
    CTransform* nt = world_.Get<CTransform>(npc_);
    if (nt) renderer.DrawBillboard(nt->pos + math::Vec3{0, 2.7f, 0}, 1.2f,
                                   {1.0f, 0.85f, 0.2f, 1.0f}, assets.glow.Handle());

    for (const Ring& ring : rings_) {
        float f = ring.t / ring.maxT;
        renderer.DrawSphere(ring.pos, 0.5f + f * 4.0f, ring.color.WithAlpha((1.0f - f) * 0.9f), 20);
    }
    particles_.Draw(renderer, assets.glow, 1.0f);

    // 3. Shadows. CSM (cascaded shadow maps) replaces the CPU projected
    // contact shadows when the driver's FBO/depth path works; otherwise the
    // projected-shadow fallback stays (this Intel driver is known to corrupt
    // VAO rendering into FBOs, which the renderer self-test detects).
    if (!renderer.ShadowsEnabled()) {
        gfx::Color shadowCol{0.02f, 0.02f, 0.04f, 0.45f};
        if (pt) renderer.DrawProjectedShadow(assets.playerMesh, pt->Model(), -sunDir, shadowCol);
        {
            auto mobsS = world_.ViewAll<CEnemy>();
            for (size_t i = 0; i < mobsS.Size(); ++i) {
                ecs::Entity e = world_.EntityAt<CEnemy>(i);
                CEnemy* ce = world_.Get<CEnemy>(e);
                CTransform* et = world_.Get<CTransform>(e);
                if (ce && et && !ce->dead) {
                    if (!ce->bones.empty() &&
                        !assets.wolfGltf.nodes.empty() &&
                        assets.wolfGltf.nodes[0].mesh.Skinned()) {
                        const math::Mat4 model = et->Model();
                        for (const assets::GltfMeshNode& part : wolfParts_) {
                            renderer.DrawProjectedShadowSkinned(
                                part.mesh, model, ce->bones,
                                static_cast<int>(ce->bones.size()), -sunDir, shadowCol);
                        }
                    } else {
                        renderer.DrawProjectedShadow(assets.wolfMesh, et->Model(), -sunDir,
                                                     shadowCol);
                    }
                }
            }
        }
        CTransform* npcT = world_.Get<CTransform>(npc_);
        if (npcT) renderer.DrawProjectedShadow(assets.playerMesh, npcT->Model(), -sunDir, shadowCol);
        for (const math::Mat4& rock : rocks_) {
            renderer.DrawProjectedShadow(assets.kenneyRock, rock, -sunDir, shadowCol);
        }
    }

    // Ground facing marker: a subtle chevron a few units ahead of the player,
    // oriented by the camera yaw and projected onto the terrain. It answers
    // "which way am I facing" without obstructing gameplay. Drawn before
    // EndScene so it stays a scene element (bloomed with the HDR image).
    if (pt) {
        math::Vec3 fwd{-std::sin(yaw_), 0.0f, -std::cos(yaw_)};
        math::Vec3 perp = math::Cross(fwd, math::Vec3::Up());
        auto ground = [&](const math::Vec3& p) {
            return math::Vec3{p.x, std::max(TerrainHeight(p.x, p.z), 0.1f) + 0.04f, p.z};
        };
        math::Vec3 base = ground(pt->pos + fwd * 2.1f);
        math::Vec3 tip = ground(pt->pos + fwd * 3.6f);
        math::Vec3 left = ground(base - perp * 0.55f);
        math::Vec3 right = ground(base + perp * 0.55f);
        const gfx::Color chevCol{0.45f, 0.9f, 1.0f, 0.4f};
        gfx::Renderer::LineVertex chevron[6] = {
            {left, chevCol},  {tip, chevCol}, {tip, chevCol},
            {right, chevCol}, {left, chevCol}, {right, chevCol},
        };
        renderer.DrawLines(chevron, 6, math::Mat4::Identity());
    }

    // End the 3D scene phase: composite the HDR frame (bloom) to the backbuffer
    // and bind the backbuffer so every HUD draw below is crisp and unbloomed.
    renderer.EndScene();

    DrawNameplates(renderer);
    DrawMinimap(renderer);
    DrawHUD(renderer);
    DrawOverlays(renderer);
    if (dialogueOpen_) DrawDialogue(renderer);
}

void GameScene::DrawHUD(gfx::Renderer& renderer) {
    ui::Theme& theme = app_.Theme();
    CHealth* ph = world_.Get<CHealth>(player_);
    CPlayer* pl = world_.Get<CPlayer>(player_);
    if (!ph || !pl) return;

    ui::DrawLabel(renderer, theme, "生命", {24, 20}, 14, theme.text, false, true);
    float hpFrac = math::Saturate(ph->hp / ph->maxHp);
    gfx::Color hpColor = hpFrac > 0.5f ? gfx::Color{0.2f, 1.0f, 0.35f, 1.0f}
                        : hpFrac > 0.25f ? gfx::Color{1.0f, 0.85f, 0.2f, 1.0f}
                                         : gfx::Color{1.0f, 0.2f, 0.2f, 1.0f};
    ui::DrawBar(renderer, theme, {70, 14, 280, 22}, hpFrac, hpColor);

    ui::DrawLabel(renderer, theme, "法力", {24, 48}, 14, theme.text, false, true);
    ui::DrawBar(renderer, theme, {70, 42, 200, 14}, math::Saturate(pl->mana / pl->maxMana),
            gfx::Color{0.25f, 0.45f, 1.0f, 1.0f});

    char buf[128];
    std::snprintf(buf, sizeof(buf), "等级 %d", pl->level);
    ui::DrawLabel(renderer, theme, buf, {24, 64}, 13, theme.text, false, false);
    int toNext = 50 + pl->level * 30;
    ui::DrawBar(renderer, theme, {90, 66, 260, 8}, math::Saturate(static_cast<float>(pl->xp) / toNext),
            gfx::Color{1.0f, 0.85f, 0.3f, 1.0f});

    std::snprintf(buf, sizeof(buf), "金币 %d", pl->gold);
    math::Vec2 goldSize = ui::MeasureText(theme.font, buf, 16);
    ui::DrawLabel(renderer, theme, buf, {1276 - goldSize.x, 18}, 16, gfx::Color{1.0f, 0.85f, 0.3f, 1.0f},
              false, false);

    if (questAccepted_ || questKills_ >= 5) {
        std::snprintf(buf, sizeof(buf), "任务：猎杀野狼 %d/5", std::min(questKills_, 5));
        ui::DrawLabel(renderer, theme, buf, {640, 62}, 15, theme.accent, true, false);
    }

    if (bannerTime_ > 0.0f) {
        float alpha = math::Clamp(bannerTime_, 0.0f, 1.0f);
        ui::DrawLabel(renderer, theme, banner_, {640, 300}, 26, theme.accent.WithAlpha(alpha), true, true);
    }

    // Hotbar.
    auto drawSlot = [&](const math::Rect2& rect, const char* label, float cdFrac, float manaCost,
                        bool ready) {
        renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h},
                          ready ? theme.panel : gfx::Color{0.15f, 0.12f, 0.12f, 0.9f});
        renderer.DrawRectOutline(rect, 2, ready ? theme.border : theme.disabled);
        ui::DrawLabel(renderer, theme, label, {rect.x + rect.w * 0.5f, rect.y + rect.h * 0.42f}, 13,
                  ready ? theme.text : theme.disabled, true, true);
        if (cdFrac > 0.01f) {
            renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h * cdFrac},
                              {0.05f, 0.06f, 0.1f, 0.75f});
        }
        std::snprintf(buf, sizeof(buf), "MP %d", static_cast<int>(manaCost));
        ui::DrawLabel(renderer, theme, buf, {rect.x + rect.w * 0.5f, rect.y + rect.h - 10}, 10,
                  theme.text.WithAlpha(0.7f), true, false);
    };
    drawSlot({540, 640, 110, 56}, "1 火球术", math::Saturate(pl->fireCd / 2.0f), 12, pl->fireCd <= 0.0f);
    drawSlot({660, 640, 110, 56}, "2 治疗术", math::Saturate(pl->healCd / 5.0f), 15, pl->healCd <= 0.0f);

    ui::DrawLabel(renderer, theme, "WASD 移动 | 鼠标旋转 | 左键攻击 | 右键冲刺 | F 对话 | 1/2 技能 | Esc 暂停",
              {640, 712}, 11, theme.text.WithAlpha(0.6f), true, false);

    const gfx::Renderer::RenderStats& stats = renderer.Stats();
    std::snprintf(buf, sizeof(buf), "帧率 %.0f | 绘制 %u | 三角形 %u | 实例 %u",
                  app_.TimeRef().Fps(), stats.drawCalls, stats.triangles, stats.instances);
    math::Vec2 statsSize = ui::MeasureText(theme.font, buf, 11);
    ui::DrawLabel(renderer, theme, buf, {1276 - statsSize.x, 692}, 11, theme.text.WithAlpha(0.55f), false, false);

    if (hurtFlash_ > 0.0f) {
        renderer.DrawRect({0, 0}, {1280, 720}, {1.0f, 0.05f, 0.05f, hurtFlash_ * 0.25f});
    }
}

void GameScene::DrawNameplates(gfx::Renderer& renderer) {
    ui::Theme& theme = app_.Theme();
    auto mobs = world_.ViewAll<CEnemy>();
    for (size_t i = 0; i < mobs.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CEnemy>(i);
        CEnemy* ce = world_.Get<CEnemy>(e);
        CTransform* et = world_.Get<CTransform>(e);
        CHealth* eh = world_.Get<CHealth>(e);
        if (!ce || !et || !eh || ce->dead) continue;
        if (!ce->aggro && eh->hp >= eh->maxHp) continue;
        math::Vec2 s = WorldToScreen(renderer, et->pos + math::Vec3{0, 2.0f, 0});
        if (s.x < -100 || s.x > 1380 || s.y < -100 || s.y > 820) continue;
        renderer.DrawRect({s.x - 22, s.y - 10}, {44, 4},
                          gfx::Color{0.1f, 0.1f, 0.1f, 0.8f});
        renderer.DrawRect({s.x - 21, s.y - 9}, {42 * math::Saturate(eh->hp / eh->maxHp), 2},
                          gfx::Color{0.9f, 0.2f, 0.2f, 1.0f});
        ui::DrawLabel(renderer, theme, "野狼", {s.x, s.y - 26}, 11, theme.text, true, false);
    }
    CTransform* nt = world_.Get<CTransform>(npc_);
    if (nt) {
        math::Vec2 s = WorldToScreen(renderer, nt->pos + math::Vec3{0, 2.2f, 0});
        if (s.x > -100 && s.x < 1380 && s.y > -100 && s.y < 820) {
            ui::DrawLabel(renderer, theme, "村长", {s.x, s.y - 14}, 13, gfx::Color{0.4f, 1.0f, 0.5f, 1.0f},
                      true, false);
            CTransform* pt = world_.Get<CTransform>(player_);
            if (pt && (nt->pos - pt->pos).Length() < 3.5f) {
                ui::DrawLabel(renderer, theme, "按 F 交谈", {s.x, s.y + 2}, 11, theme.accent, true, false);
            }
        }
    }
}

void GameScene::DrawMinimap(gfx::Renderer& renderer) {
    const float cx = 1195.0f;
    const float cy = 115.0f;
    const float scale = 0.85f;
    renderer.DrawQuad({cx - 95, cy - 95}, {190, 190}, {0.02f, 0.03f, 0.07f, 0.82f},
                      app_.Assets().glow.Handle());
    renderer.DrawRectOutline({cx - 95, cy - 95, 190, 190}, 2.0f,
                             gfx::Color{0.25f, 0.45f, 0.7f, 0.9f});
    auto dot = [&](const math::Vec3& world, const gfx::Color& color, float size) {
        float x = cx + world.x * scale;
        float z = cy + world.z * scale;
        if (x < cx - 92 || x > cx + 92 || z < cy - 92 || z > cy + 92) return;
        renderer.DrawRect({x - size * 0.5f, z - size * 0.5f}, {size, size}, color);
    };
    CTransform* pt = world_.Get<CTransform>(player_);
    CTransform* nt = world_.Get<CTransform>(npc_);
    if (pt) {
        // Facing arrow (under the dot): a small triangle rotated by the camera
        // yaw - movement is camera-relative, so the character faces "camera
        // forward", which is exactly what the arrow points along.
        float x = cx + pt->pos.x * scale;
        float z = cy + pt->pos.z * scale;
        if (x > cx - 92 && x < cx + 92 && z > cy - 92 && z < cy + 92) {
            math::Vec2 tip, left, right;
            math::FacingArrowPoints({x, z}, yaw_, 9.0f, 3.0f, tip, left, right);
            renderer.DrawTriangle2D(left, right, tip,
                                    gfx::Color{0.3f, 0.85f, 1.0f, 0.85f});
        }
        dot(pt->pos, gfx::Color{0.3f, 0.8f, 1.0f, 1.0f}, 5.0f);
    }
    if (nt) dot(nt->pos, gfx::Color{0.4f, 1.0f, 0.5f, 1.0f}, 5.0f);
    auto mobs = world_.ViewAll<CEnemy>();
    for (size_t i = 0; i < mobs.Size(); ++i) {
        ecs::Entity e = world_.EntityAt<CEnemy>(i);
        CEnemy* ce = world_.Get<CEnemy>(e);
        CTransform* et = world_.Get<CTransform>(e);
        if (!ce || !et || ce->dead) continue;
        dot(et->pos, ce->aggro ? gfx::Color{1.0f, 0.25f, 0.2f, 1.0f}
                               : gfx::Color{0.9f, 0.5f, 0.3f, 0.8f}, 3.5f);
    }
    ui::DrawLabel(renderer, app_.Theme(), "小地图", {cx, cy - 88}, 11, app_.Theme().text.WithAlpha(0.7f),
              true, false);
}

void GameScene::DrawDialogue(gfx::Renderer& renderer) {
    ui::Theme& theme = app_.Theme();
    renderer.DrawRect({0, 0}, {1280, 720}, {0, 0, 0, 0.45f});
    ui::DrawPanel(renderer, theme, {290, 200, 700, 320});
    ui::DrawLabel(renderer, theme, "村长", {640, 230}, 24, gfx::Color{0.4f, 1.0f, 0.5f, 1.0f}, true, true);

    if (!questAccepted_) {
        ui::DrawLabel(renderer, theme, "年轻的冒险者，村外的野狼正在威胁我们的安全。",
                  {640, 290}, 16, theme.text, true, false);
        ui::DrawLabel(renderer, theme, "帮我猎杀 5 只野狼，我会给你丰厚的报酬！",
                  {640, 318}, 16, theme.text, true, false);
        if (ui::DrawButton(renderer, theme, "接受任务", {390, 380, 500, 52}, *app_.Input())) {
            questAccepted_ = true;
            questKills_ = 0;
            dialogueOpen_ = false;
            app_.Window()->SetCaptureMouse(true);
            app_.PlaySfx(app_.sfxPickup_);
        }
    } else if (questKills_ >= 5) {
        ui::DrawLabel(renderer, theme, "干得漂亮！野狼再也不敢靠近村庄了。",
                  {640, 290}, 16, theme.text, true, false);
        ui::DrawLabel(renderer, theme, "这是你的奖励。", {640, 318}, 16, theme.text, true, false);
        if (ui::DrawButton(renderer, theme, "交付任务（+50 经验 +20 金币）", {340, 380, 600, 52},
                       *app_.Input())) {
            CPlayer* player = world_.Get<CPlayer>(player_);
            if (player) {
                player->gold += 20;
                GiveXp(50);
            }
            questAccepted_ = false;
            questKills_ = 0;
            dialogueOpen_ = false;
            app_.Window()->SetCaptureMouse(true);
            app_.PlaySfx(app_.sfxWave_);
        }
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "任务进度：%d/5，继续加油！", questKills_);
        ui::DrawLabel(renderer, theme, buf, {640, 300}, 16, theme.text, true, false);
        if (ui::DrawButton(renderer, theme, "离开", {490, 380, 300, 48}, *app_.Input())) {
            dialogueOpen_ = false;
            app_.Window()->SetCaptureMouse(true);
        }
    }
}

void GameScene::DrawOverlays(gfx::Renderer& renderer) {
    ui::Theme& theme = app_.Theme();
    if (paused_) {
        renderer.DrawRect({0, 0}, {1280, 720}, {0, 0, 0, 0.55f});
        ui::DrawPanel(renderer, theme, {390, 180, 500, 360});
        ui::DrawLabel(renderer, theme, "暂停", {640, 210}, 30, theme.accent, true, true);
        if (ui::DrawButton(renderer, theme, "继续游戏", {440, 290, 400, 52}, *app_.Input())) {
            paused_ = false;
            app_.Window()->SetCaptureMouse(true);
            app_.PlaySfx(app_.sfxClick_);
        }
        if (ui::DrawButton(renderer, theme, "返回标题", {440, 360, 400, 52}, *app_.Input())) {
            app_.PlaySfx(app_.sfxClick_);
            app_.GoToTitle();
        }
    }
    if (dead_ && deadTimer_ <= 0.0f) {
        renderer.DrawRect({0, 0}, {1280, 720}, {0, 0, 0, 0.6f});
        ui::DrawPanel(renderer, theme, {390, 200, 500, 300});
        ui::DrawLabel(renderer, theme, "你死了", {640, 240}, 30, gfx::Color{1.0f, 0.25f, 0.25f, 1.0f},
                  true, true);
        if (ui::DrawButton(renderer, theme, "在村庄复活", {440, 310, 400, 52}, *app_.Input())) {
            RespawnPlayer();
        }
        if (ui::DrawButton(renderer, theme, "返回标题", {440, 380, 400, 52}, *app_.Input())) {
            app_.GoToTitle();
        }
    }
}

// ---------------------------------------------------------------------------
// NeonApp
// ---------------------------------------------------------------------------

bool NeonApp::OnCreate() {
    if (disableShadows_) renderer_.SetShadowsEnabled(false);
    renderer_.SetBackendName(backendName_);
    renderer_.SetBloomEnabled(bloomEnabled_);
    renderer_.SetExposure(exposure_);
    renderer_.SetTonemapEnabled(tonemapEnabled_);
    renderer_.SetMsaaEnabled(msaaEnabled_);
    renderer_.SetIblStrength(iblStrength_);
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Demo: renderer init failed");
        return false;
    }
    audio_ = nullptr;
    if (!noAudio_) {
        audio_ = audio::CreatePlatformAudioBackend();
        if (audio_) audio_->Init();
    }
    assetMgr_.Init(&renderer_);

    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
    // 所有 UI 中文文本集合：新增中文文本时请同步加入，否则对应字形不会烘焙。
        // System CJK font with DYNAMIC glyphs (any Chinese text renders
    // without maintaining a character list).
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24);
    theme_.font = cjkFont_.Valid() ? cjkFont_ : pixelFont_;

    demo::CreateDemoAssets(renderer_, assetMgr_, assets_);
    helmet_ = assetMgr_.LoadGLTF("assets/models/DamagedHelmet/DamagedHelmet.gltf");

    // Real skinned wolf (Blender glTF export). The animation set is imported
    // from the same glTF JSON so wolves move with the authored clips.
    assets_.wolfGltf =
        assetMgr_.LoadGLTF("assets/models/wolf/Wolf-Blender-2.82a.gltf");
    {
        std::ifstream in("assets/models/wolf/Wolf-Blender-2.82a.gltf");
        std::stringstream ss;
        ss << in.rdbuf();
        core::Result<anim::AnimSet> animResult =
            anim::ImportGltf(ss.str(), assets_.wolfGltf, 0);
        if (animResult.Ok()) {
            assets_.wolfAnim = std::move(animResult.Value());
            if (!assets_.wolfGltf.skins.empty())
                scene::EnsureValidSkinBind(assets_.wolfAnim.skeleton,
                                           assets_.wolfGltf.skins[0].joints);
            NEON_LOG_INFO("Wolf: %zu clips, %zu bones, %zu mesh nodes",
                          assets_.wolfAnim.clips.size(),
                          assets_.wolfAnim.skeleton.bones.size(),
                          assets_.wolfGltf.nodes.size());
        } else {
            NEON_LOG_WARN("Wolf: animation import failed: %s",
                          animResult.Error().c_str());
        }
    }

    sfxSwing_ = sfx::MakeSwing();
    sfxHit_ = sfx::MakeHit();
    sfxExplosion_ = sfx::MakeExplosion();
    sfxPickup_ = sfx::MakePickup();
    sfxHurt_ = sfx::MakeHurt();
    sfxDash_ = sfx::MakeDash();
    sfxWave_ = sfx::MakeWave();
    sfxGameOver_ = sfx::MakeGameOver();
    sfxClick_ = sfx::MakeClick();
    sfxFireball_ = sfx::MakeFireball();
    music_ = sfx::MakeMusic();

    save_.Load("neon_realm_save.dat");
    if (audio_ && audio_->Available()) audio_->Play(music_, 0.45f);

    if (smokeMode_) {
        scenes_.Change(std::make_unique<GameScene>(*this));
    } else {
        scenes_.Change(std::make_unique<TitleScene>(*this));
    }
    NEON_LOG_INFO("Demo: NeonRealm ready (CJK font %s)", cjkFont_.Valid() ? "ok" : "missing");
    return true;
}

void NeonApp::OnShutdown() {
    save_.Save("neon_realm_save.dat");
    if (audio_) audio_->Shutdown();
    renderer_.Shutdown();
}

void NeonApp::OnUpdate(float dt) {
    // Drain completed async texture decodes (uploads + callbacks, main thread).
    assetMgr_.PumpAsync();
    scenes_.Update(dt);
}

void NeonApp::OnRender() {
    renderer_.BeginFrame({0.02f, 0.03f, 0.08f, 1.0f});
    renderer_.DrawSky();
    scenes_.Draw(renderer_);
    if (!screenshotPath_.empty() && TimeRef().frameIndex >= screenshotFrame_) {
        std::vector<uint8_t> pixels;
        if (renderer_.CaptureFrame(pixels)) {
            int ok = stbi_write_png(screenshotPath_.c_str(), renderer_.ScreenWidth(),
                                    renderer_.ScreenHeight(), 4, pixels.data(),
                                    renderer_.ScreenWidth() * 4);
            NEON_LOG_INFO("Screenshot: %s (%s)", screenshotPath_.c_str(), ok ? "ok" : "failed");
        }
        screenshotPath_.clear();
    }
    if (!bloomCompareDone_ && !bloomCompareOff_.empty() &&
        TimeRef().frameIndex >= bloomCompareFrame_) {
        std::vector<uint8_t> off, on;
        if (renderer_.CaptureBloomComparison(off, on)) {
            const int w = renderer_.ScreenWidth();
            const int h = renderer_.ScreenHeight();
            int okOff = stbi_write_png(bloomCompareOff_.c_str(), w, h, 4, off.data(), w * 4);
            int okOn = stbi_write_png(bloomCompareOn_.c_str(), w, h, 4, on.data(), w * 4);
            NEON_LOG_INFO("Bloom compare: off=%s (%s) on=%s (%s)", bloomCompareOff_.c_str(),
                          okOff ? "ok" : "failed", bloomCompareOn_.c_str(),
                          okOn ? "ok" : "failed");
        } else {
            NEON_LOG_WARN("Bloom compare skipped: HDR pipeline inactive");
        }
        bloomCompareDone_ = true;
    }
    if (!tonemapCompareDone_ && !tonemapCompareClamped_.empty() &&
        TimeRef().frameIndex >= tonemapCompareFrame_) {
        std::vector<uint8_t> clamped, aces;
        if (renderer_.CaptureTonemapComparison(clamped, aces)) {
            const int w = renderer_.ScreenWidth();
            const int h = renderer_.ScreenHeight();
            int okClamped = stbi_write_png(tonemapCompareClamped_.c_str(), w, h, 4, clamped.data(),
                                           w * 4);
            int okAces = stbi_write_png(tonemapCompareAces_.c_str(), w, h, 4, aces.data(), w * 4);
            NEON_LOG_INFO("Tonemap compare: clamp=%s (%s) aces=%s (%s)",
                          tonemapCompareClamped_.c_str(), okClamped ? "ok" : "failed",
                          tonemapCompareAces_.c_str(), okAces ? "ok" : "failed");
        } else {
            NEON_LOG_WARN("Tonemap compare skipped: HDR pipeline inactive");
        }
        tonemapCompareDone_ = true;
    }
    renderer_.EndFrame();
}

void NeonApp::OnEvent(const platform::InputEvent& event) { scenes_.OnEvent(event); }

void NeonApp::StartGame() { scenes_.Change(std::make_unique<GameScene>(*this)); }

void NeonApp::GoToTitle() { scenes_.Change(std::make_unique<TitleScene>(*this)); }

void NeonApp::PlaySfx(const audio::SoundFx& fx, float volume) {
    if (audio_ && audio_->Available()) audio_->Play(fx, volume);
}

void NeonApp::UpdateSave(int level, int xp, int gold) {
    save_.SetInt("level", level);
    save_.SetInt("xp", xp);
    save_.SetInt("gold", gold);
    save_.Save("neon_realm_save.dat");
}

} // namespace neon::demo
