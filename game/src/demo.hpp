#pragma once
#include "neon/neon.hpp"

#include "art.hpp"
#include "sfx.hpp"

namespace neon::demo {

// --- ECS components ---
struct CTransform {
    math::Vec3 pos;
    math::Quat rot;
    math::Vec3 scale{1, 1, 1};

    math::Mat4 Model() const {
        math::Mat4 m = rot.ToMat4() * math::Mat4::Scale(scale);
        m.m[3] = pos.x;
        m.m[7] = pos.y;
        m.m[11] = pos.z;
        return m;
    }
};

struct CMesh {
    gfx::Mesh mesh;
    gfx::Material mat;
    bool visible = true;
};

struct CRigidBody {
    physics::World::BodyId body;
    float radius = 0.5f;
};

struct CHealth {
    float hp = 100.0f;
    float maxHp = 100.0f;
};

struct CPlayer {
    float speed = 8.0f;
    float dashCooldown = 0.0f;
    float dashTime = 0.0f;
    float attackCd = 0.0f;
    float swingTimer = 0.0f;
    float iframes = 0.0f;
    float mana = 50.0f;
    float maxMana = 50.0f;
    float fireCd = 0.0f;
    float healCd = 0.0f;
    int level = 1;
    int xp = 0;
    int gold = 0;
    bool alive = true;
};

struct CEnemy {
    math::Vec3 home;
    float attackCd = 0.0f;
    float respawnTimer = 0.0f;
    float bobPhase = 0.0f;
    // Skinned-wolf animation state: per-wolf clip time and cached bone
    // matrices (world * inverseBind) uploaded via Renderer::DrawSkinnedMesh.
    float animTime = 0.0f;
    std::vector<math::Mat4> bones;
    bool dead = false;
    bool aggro = false;
};

struct CNPC {
    std::string name;
};

struct CPickup {
    float bobPhase = 0.0f;
};

struct CProjectile {
    math::Vec3 vel;
    float damage = 25.0f;
    float life = 2.5f;
};

class NeonApp;
class GameScene;

class TitleScene : public scene::Scene {
public:
    explicit TitleScene(NeonApp& app);
    void OnEnter() override;
    void Update(float dt) override;
    void Draw(gfx::Renderer& renderer) override;

private:
    NeonApp& app_;
    float t_ = 0.0f;
    bool showHelp_ = false;
};

class GameScene : public scene::Scene {
public:
    explicit GameScene(NeonApp& app);
    ~GameScene() override;

    void OnEnter() override;
    void OnExit() override;
    void Update(float dt) override;
    void Draw(gfx::Renderer& renderer) override;

    void DamagePlayer(float amount);

private:
    void SetupWorld();
    void UpdatePlayer(float dt);
    void UpdateMobs(float dt);
    void UpdateProjectiles(float dt);
    void UpdateNPC(float dt);
    void UpdateCamera(float dt);
    void UpdateDayNight(float dt);
    void UpdateRings(float dt);
    void UpdateFlag(float dt);
    void MeleeAttack();
    void Fireball();
    void Heal();
    void KillMob(ecs::Entity enemy);
    void GiveXp(int amount);
    void RespawnPlayer();

    void DrawHUD(gfx::Renderer& renderer);
    void DrawNameplates(gfx::Renderer& renderer);
    void DrawMinimap(gfx::Renderer& renderer);
    void DrawDialogue(gfx::Renderer& renderer);
    void DrawOverlays(gfx::Renderer& renderer);

    float TerrainHeight(float x, float z) const;
    math::Vec2 WorldToScreen(gfx::Renderer& renderer, const math::Vec3& world) const;

    NeonApp& app_;
    ecs::World world_;
    physics::World physics_;
    gfx::ParticleSystem particles_;
    core::Rng rng_{0xC0FFEEull};

    ecs::Entity player_;
    ecs::Entity npc_;
    float yaw_ = 0.0f;
    float pitch_ = 0.35f;
    float camDist_ = 7.5f;
    // Smoothed character facing (radians, Y-rotation). Lerped toward the
    // movement direction each frame so the player's orientation reads clearly
    // in third-person instead of snapping (or freezing) while idle.
    float facingYaw_ = 0.0f;
    gfx::Camera camera_;
    float shakeTime_ = 0.0f;
    float shakeMag_ = 0.0f;

    bool questAccepted_ = false;
    int questKills_ = 0;
    bool dialogueOpen_ = false;
    bool paused_ = false;
    bool dead_ = false;
    float deadTimer_ = 0.0f;
    float hurtFlash_ = 0.0f;
    std::string banner_;
    float bannerTime_ = 0.0f;
    float trailTimer_ = 0.0f;
    float dayTime_ = 0.0f;

    std::vector<float> heights_;
    int segments_ = 72;
    float worldSize_ = 200.0f;
    float heightScale_ = 6.0f;

    std::vector<math::Mat4> pines_;
    std::vector<math::Mat4> oaks_;
    std::vector<math::Mat4> rocks_;
    std::vector<math::Mat4> logs_;

    // GPU-skinned flag rig: two bones, pose driven by time, bone matrices
    // recomputed every frame and uploaded through Renderer::DrawSkinnedMesh.
    anim::Skeleton flagSkeleton_;
    anim::Pose flagPose_;
    std::vector<math::Mat4> flagBones_;
    float flagAnimTime_ = 0.0f;

    // Skinned wolf: renderable parts (excludes the glTF ground disc) plus the
    // run/idle clips picked by aggression state.
    std::vector<assets::GltfMeshNode> wolfParts_;
    const anim::AnimationClip* wolfRunClip_ = nullptr;
    const anim::AnimationClip* wolfIdleClip_ = nullptr;

    struct Ring {
        math::Vec3 pos;
        float t;
        float maxT;
        gfx::Color color;
    };
    std::vector<Ring> rings_;
};

class NeonApp : public core::Application {
public:
    bool OnCreate() override;
    void OnShutdown() override;
    void OnUpdate(float dt) override;
    void OnRender() override;
    void OnEvent(const platform::InputEvent& event) override;

    void StartGame();
    void GoToTitle();
    void Quit() { Window()->RequestClose(); }
    void PlaySfx(const audio::SoundFx& fx, float volume = 1.0f);
    void UpdateSave(int level, int xp, int gold);

    gfx::Renderer& Renderer() { return renderer_; }
    audio::IAudioBackend* Audio() { return audio_.get(); }
    ui::Theme& Theme() { return theme_; }
    const DemoAssets& Assets() const { return assets_; }
    assets::AssetManager& AssetMgr() { return assetMgr_; }
    void SetSmokeMode(bool v) { smokeMode_ = v; }
    bool SmokeMode() const { return smokeMode_; }
    void SetNoAudio(bool v) { noAudio_ = v; }
    void SetDisableShadows(bool v) { disableShadows_ = v; }
    void SetBloomEnabled(bool v) { bloomEnabled_ = v; }
    void SetExposure(float v) { exposure_ = v; }
    void SetTonemapEnabled(bool v) { tonemapEnabled_ = v; }
    void SetMsaaEnabled(bool v) { msaaEnabled_ = v; }
    // T3.8: global IBL intensity in [0,1]. 0 disables environment lighting and
    // keeps the legacy flat ambient (the --ibl 0 reference), 1 is full IBL.
    void SetIblStrength(float v) { iblStrength_ = v; }
    void SetBackendName(const std::string& name) { backendName_ = name; }
    void RequestScreenshot(const std::string& path, uint64_t frame) {
        screenshotPath_ = path;
        screenshotFrame_ = frame;
    }
    // T3.6 verification: capture the SAME frame twice (bloom off then on) from
    // one HDR target so the two PNGs differ only by the bloom contribution.
    void RequestBloomCompare(const std::string& offPath, const std::string& onPath, uint64_t frame) {
        bloomCompareOff_ = offPath;
        bloomCompareOn_ = onPath;
        bloomCompareFrame_ = frame;
    }
    // T3.7 verification: capture the SAME frame twice (legacy clamp then ACES
    // tonemap) from one resolved HDR target so the two PNGs differ only by the
    // tone-mapping operator.
    void RequestTonemapCompare(const std::string& clampedPath, const std::string& acesPath,
                               uint64_t frame) {
        tonemapCompareClamped_ = clampedPath;
        tonemapCompareAces_ = acesPath;
        tonemapCompareFrame_ = frame;
    }

private:
    gfx::Renderer renderer_;
    std::unique_ptr<audio::IAudioBackend> audio_;
    scene::SceneManager scenes_;
    ui::Theme theme_;
    gfx::Font pixelFont_;
    gfx::Font cjkFont_;
    DemoAssets assets_;
    assets::AssetManager assetMgr_;
    assets::GltfAsset helmet_;

    audio::SoundFx sfxSwing_;
    audio::SoundFx sfxHit_;
    audio::SoundFx sfxExplosion_;
    audio::SoundFx sfxPickup_;
    audio::SoundFx sfxHurt_;
    audio::SoundFx sfxDash_;
    audio::SoundFx sfxWave_;
    audio::SoundFx sfxGameOver_;
    audio::SoundFx sfxClick_;
    audio::SoundFx sfxFireball_;
    audio::SoundFx music_;

    core::Config save_;
    int bestScore_ = 0;
    bool smokeMode_ = false;
    bool noAudio_ = false;
    bool disableShadows_ = false;
    bool bloomEnabled_ = true;
    float exposure_ = 1.0f;
    bool tonemapEnabled_ = true;
    bool msaaEnabled_ = true;
    float iblStrength_ = 1.0f;
    std::string backendName_ = "gl";
    std::string screenshotPath_;
    uint64_t screenshotFrame_ = 0;
    std::string bloomCompareOff_;
    std::string bloomCompareOn_;
    uint64_t bloomCompareFrame_ = 0;
    bool bloomCompareDone_ = false;
    std::string tonemapCompareClamped_;
    std::string tonemapCompareAces_;
    uint64_t tonemapCompareFrame_ = 0;
    bool tonemapCompareDone_ = false;

    friend class TitleScene;
    friend class GameScene;
};

} // namespace neon::demo
