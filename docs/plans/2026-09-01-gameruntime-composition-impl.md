# GameRuntime 组合服务化拆分 实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 `GameRuntime`（头 718 行 + 实现 3446 行）拆成「编排器 + 15 个组合服务」，纯机械拆分、716 测试零回归。

**Architecture:** 每个内聚子系统抽成独立 `XxxSystem` 类（`engine/include/neon/scene/systems/` + `engine/src/scene/systems/`），不持有 `world_`（参数/指针传入）；`GameRuntime` 持有全部系统并转发 Start/Tick/Draw，保留跨子系统共享状态（`world_`/`scriptCtx_`/`hosts_`/`cfg_`/`compReg_`）。

**Tech Stack:** C++17、CMake + MSVC、`neon_tests`（CHECK 宏）。

**关键上下文（已核实）：**
- `game_runtime.hpp` 718 行；`game_runtime.cpp` 2395 行、`game_runtime_combat.cpp` 205 行、`game_runtime_content.cpp` 128 行。
- 完整方法分布已列（见下每任务）。
- 子系统间隐式耦合需显式参数化：`TickProjectiles` 需要 `gfx::ParticleSystem`（VFX）+ `world_`；`TickStatuses` 需要 `hosts_.lua`（OnStatusTick）；`OverlapSphere/Box` 需要 `world_` + lag-comp pose。

**构建/测试命令（全程）：**
- `cmake --build build-msvc --config Release`
- `& "build-msvc\Release\neon_tests.exe"`（基线 716 全绿）

---

## 阶段 1：最独立 6 系统（投射物/HUD/粒子/补间/lag-comp/状态）

### Task 1: `ProjectileSystem`

**Files:**
- Create: `engine/include/neon/scene/systems/projectile_system.hpp`
- Create: `engine/src/scene/systems/projectile_system.cpp`
- Modify: `engine/include/neon/scene/game_runtime.hpp`（删 `Projectile`/`ProjectileTrail`/`ProjectileBurst`/`projectiles_`/`fireballMesh_`；删 `SpawnProjectile`/`TickProjectiles` 声明）
- Modify: `engine/src/scene/game_runtime_combat.cpp`（删 `SpawnProjectile`/`TickProjectiles`/`ProjectileTrail`/`ProjectileBurst` 实现）
- Modify: `engine/src/scene/game_runtime.cpp`（`SpawnProjectile` 转发 `projectiles_.Spawn(...)`；`Tick` 里调 `projectiles_.Tick(dt, world_, particles_)`）
- Modify: `CMakeLists.txt`（把 `systems/projectile_system.cpp` 加入 neon_scene 源）
- Test: `tests/test_combat.cpp`

**Step 1:** 新头文件：

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include "neon/ecs/world.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/particles.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/status.hpp"

namespace neon::scene {
struct SceneHealth;
struct SceneTransform;

// 通用投射物系统：fixed-step 移动 + 命中检测 + VFX。数据驱动（range/hitRadius/statuses）。
class ProjectileSystem {
public:
    void Spawn(const math::Vec3& pos, const math::Vec3& dir, float speed, float damage,
               float life, ecs::Entity caster, float range, float hitRadius,
               const std::vector<SkillStatus>& statuses);
    void Tick(float dt, ecs::World& world, gfx::ParticleSystem& particles);
    size_t Count() const { return projectiles_.size(); }

private:
    struct Projectile {
        math::Vec3 pos, dir;
        float speed = 0.0f, damage = 0.0f, life = 0.0f, traveled = 0.0f, range = 0.0f;
        float hitRadius = 0.8f;
        ecs::Entity caster;
        std::vector<SkillStatus> statuses;
    };
    void Trail(const Projectile& p, gfx::ParticleSystem& particles);
    void Burst(const Projectile& p, gfx::ParticleSystem& particles);
    std::vector<Projectile> projectiles_;
    gfx::Mesh fireballMesh_;
};
} // namespace neon::scene
```

**Step 2:** `.cpp` 实现从 `game_runtime_combat.cpp` 原样迁移（`Spawn`/`Tick`/`Trail`/`Burst`，方法体直接搬，签名改）。

**Step 3:** `game_runtime.hpp` 删旧声明/成员 + 加 `ProjectileSystem projectiles_;`（含 `#include "neon/scene/systems/projectile_system.hpp"`）。

**Step 4:** `game_runtime.cpp`：`SpawnProjectile(...)` 转发 `projectiles_.Spawn(...)`；`Tick` 里 `projectiles_.Tick(dt, world_, particles_)`（替换原 `TickProjectiles(dt)`）。

**Step 5:** `CMakeLists.txt` 加 `engine/src/scene/systems/projectile_system.cpp`。

**Step 6:** 构建 + 测试全绿（`SpawnProjectile*`/`SkillTableCastMelee` 等测试应不受影响）。

**Step 7:** Commit: `refactor: GameRuntime 拆出 ProjectileSystem`

---

### Task 2: `HudSystem`

**Files:**
- Create: `engine/include/neon/scene/systems/hud_system.hpp` + `engine/src/scene/systems/hud_system.cpp`
- Modify: `game_runtime.hpp`（删 `FloatText`/`ScreenAnchor`/`EntityPlate`/`floatTexts_`/`screenAnchors_`/`plates_`/`lastViewProj_`/`lastCam_`/`lastAspect_`/`lastVpW_`/`lastVpH_` 及 `SpawnFloatText`/`SetEntityPlate`/`FloatTexts`/`ScreenAnchors`/`EntityPlates`/`WorldToScreen`/`ScreenToWorld`/`DesignWidth` 声明）
- Modify: `game_runtime.cpp`（迁移对应实现 + 转发）

**类接口：**

```cpp
class HudSystem {
public:
    struct FloatText { math::Vec3 world; std::string text; bool crit; float life, age; };
    struct ScreenAnchor { uint64_t entity; float x, y; bool onscreen; math::Vec3 world; };
    struct EntityPlate { std::string name; float hpFrac; };
    void CaptureView(const gfx::Camera& cam, float aspect, float vpW, float vpH);
    void Tick(float dt);
    void SpawnFloatText(const math::Vec3& w, const std::string& t, bool crit, float life);
    void SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac);
    bool WorldToScreen(const math::Vec3& w, float& x, float& y) const;
    bool ScreenToWorld(const math::Vec2& s, float& x, float& y) const;
    float DesignWidth() const;
    const std::vector<FloatText>& FloatTexts() const;
    const std::vector<ScreenAnchor>& ScreenAnchors() const;
    const std::map<uint64_t, EntityPlate>& EntityPlates() const;
private:
    std::vector<FloatText> floatTexts_;
    std::vector<ScreenAnchor> screenAnchors_;
    std::map<uint64_t, EntityPlate> plates_;
    math::Mat4 lastViewProj_; bool lastViewProjValid_ = false;
    gfx::Camera lastCam_; bool lastCamValid_ = false;
    float lastAspect_ = 16.0f/9.0f, lastVpW_ = 1280.0f, lastVpH_ = 720.0f;
};
```

**关键迁移**：`WorldToScreen`/`ScreenToWorld`/`DesignWidth` 依赖 `lastViewProj_`/`lastCam_`（Draw 时由 `CaptureView` 填）。`Draw` 里原来调用这些方法的地方改为先 `hud_.CaptureView(camera, ...)`。`ScreenAnchors()` 由 `Draw` 每帧刷新（画 anchor）。

**Step 1-6:** 同 Task 1 模式（建类 → 迁移实现 → 转发 → 构建测试 → commit）。`Draw` 里 HUD 相关（anchors 计算、float texts 更新）迁移进 `HudSystem`。
**Step 7:** Commit: `refactor: GameRuntime 拆出 HudSystem`

---

### Task 3: `SceneParticleSystem`

`gfx::ParticleSystem` 已是独立类。GameRuntime 只持有 `particles_` + `particleTex_`，`EmitParticles` 转发。**薄包装类**：

**Files:**
- Create: `engine/include/neon/scene/systems/scene_particle_system.hpp` + `.cpp`
- Modify: `game_runtime.hpp`（删 `particles_`/`particleTex_`/`EmitParticles` 声明）
- Modify: `game_runtime.cpp`（`EmitParticles` 转发 `sceneParticles_.Emit(cfg)`；`Tick` 里 `sceneParticles_.Update(dt)`；粒子贴图初始化移入该类 `Init(gfx::Renderer&)`）

**类接口：** `Emit(const gfx::EmitterConfig&)` / `Update(float dt)` / `Init(gfx::Renderer&)`（创建 `particleTex_`）。
**Step:** 建类 + 迁移 + 转发 + 构建测试 + commit `refactor: GameRuntime 拆出 SceneParticleSystem`

---

### Task 4: `TweenSystem`

**Files:**
- Create: `engine/include/neon/scene/systems/tween_system.hpp` + `.cpp`
- Modify: `game_runtime.hpp`（删 `Tween`/`tweens_`/`TickTweens` 声明）
- Modify: `game_runtime.cpp`（`Tick` 里 `tweens_.Tick(dt, world_)`）

**类接口：**

```cpp
class TweenSystem {
public:
    struct Tween { ecs::Entity target; int prop = 0; math::Vec3 from{}, to{}; float time = 1, elapsed = 0; int easing = 0; };
    void Start(ecs::Entity e, int prop, const math::Vec3& from, const math::Vec3& to, float time, int easing);
    void Tick(float dt, ecs::World& world); // 写 SceneTransform
    size_t Count() const;
private:
    std::vector<Tween> tweens_;
};
```

**迁移**：`TickTweens` 实现从 game_runtime.cpp 原样搬（写 SceneTransform.pos/rot/scale）。`Tween` binding 接线（`scriptCtx_.tweenStart`）改为 `tweens_.Start(...)`。
**Step:** 建类 + 迁移 + 转发 + 构建测试 + commit `refactor: GameRuntime 拆出 TweenSystem`

---

### Task 5: `LagCompSystem`

**Files:**
- Create: `engine/include/neon/scene/systems/lagcomp_system.hpp` + `.cpp`
- Modify: `game_runtime.hpp`（删 `poseSlots_`/`poseHead_`/`poseCount_`/`autoRewindTicks_`/`LagCompPosition`/`SetAutoLagComp`/`AutoLagCompTicks` 声明）
- Modify: `game_runtime_combat.cpp`（`LagCompPosition` 实现迁移；`OverlapSphere/OverlapBox` 改用 `lagComp_.Position(...)`）
- Modify: `game_runtime.cpp`（`Tick` 末尾录 pose 改 `lagComp_.Record(...)`；`SetAutoLagComp`/`AutoLagCompTicks` 转发）

**类接口：**

```cpp
class LagCompSystem {
public:
    static constexpr uint32_t kHistoryTicks = 64;
    void Record(const std::vector<std::pair<uint64_t, math::Vec3>>& poses);
    bool Position(ecs::Entity e, uint32_t rewindTicks, math::Vec3& out) const;
    void SetAutoRewind(uint32_t t) { autoRewindTicks_ = t; }
    uint32_t AutoRewindTicks() const { return autoRewindTicks_; }
private:
    std::vector<std::unordered_map<uint64_t, math::Vec3>> poseSlots_;
    size_t poseHead_ = 0, poseCount_ = 0;
    uint32_t autoRewindTicks_ = 0;
};
```

**迁移**：`LagCompPosition` 原实现（环形缓冲 + 回滚查询）搬入 `Position`。`Tick` 末尾记录 pose（`world_` 各 SceneTransform）→ `Record(poses)`。`OverlapSphere/Box` 的 `rewindTicks > 0` 分支改调 `lagComp_.Position(...)`。
**Step:** 建类 + 迁移 + 转发 + 构建测试 + commit `refactor: GameRuntime 拆出 LagCompSystem`

---

### Task 6: `StatusSystem`

**Files:**
- Create: `engine/include/neon/scene/systems/status_system.hpp` + `.cpp`
- Modify: `game_runtime.hpp`（删 `TickStatuses`/`HasStatus`/`StatusMagnitude` 声明）
- Modify: `game_runtime_combat.cpp`（`TickStatuses`/`HasStatus`/`StatusMagnitude` 实现迁移）
- Modify: `game_runtime.cpp`（`Tick` 里 `status_.Tick(dt, world_, hosts_.lua.get())`；`HasStatus`/`StatusMagnitude` 转发）

**类接口：**

```cpp
class StatusSystem {
public:
    void Tick(float dt, ecs::World& world, script::IScriptHost* luaHost); // 调 Lua OnStatusTick
    bool Has(ecs::World& world, ecs::Entity e, uint32_t id) const;
    float Magnitude(ecs::World& world, ecs::Entity e, uint32_t id) const;
};
```

**迁移**：`TickStatuses` 原实现（遍历 `ViewAll<StatusComponent>` + `TickStatus` + 调 `OnStatusTick`）搬入 `Tick`（注意 `TickStatus` 的 onTick 回调现在调 `luaHost->Call("OnStatusTick", ...)`，entity 序列化复用 `detail::EntityToValue`——需把该 helper 从 game_runtime_priv.hpp 保留）。`HasStatus`/`StatusMagnitude` 直接读 `world_` 的 `StatusComponent`。
**Step:** 建类 + 迁移 + 转发 + 构建测试 + commit `refactor: GameRuntime 拆出 StatusSystem`

---

## 阶段 2：中等耦合（4 系统）

### Task 7: `PrefabSystem`

- `prefs_`/`LoadPrefabs`/`SpawnPrefab` → 独立类。`SpawnPrefab` 需要 `world_` + 脚本附加（`AttachOneScript`）→ 传入回调或 `ecs::World&`。commit `refactor: GameRuntime 拆出 PrefabSystem`

### Task 8: `PluginSystem`

- `plugins_`/`DispatchPluginEvent`/`RunPluginCommand` → 独立类（`plugin::RuntimePluginManager` 已是独立类，薄包装 + 事件转发）。commit `refactor: GameRuntime 拆出 PluginSystem`

### Task 9: `SceneTreeSystem`

- `worldTransforms_`/`RebuildWorldTransforms`/`CachedLocalToWorld`/`LocalToWorld`/`GetChildren`/`GetDescendants` → 独立类，操作 `ecs::World&`（SceneParentLink）。commit `refactor: GameRuntime 拆出 SceneTreeSystem`

### Task 10: `ScriptCanvas`

- `draw2d_`/`FlushDraw2D`/`FlushCanvas` → 独立类，`Draw` 时 flush 到 renderer overlay。commit `refactor: GameRuntime 拆出 ScriptCanvas`

---

## 阶段 3：核心（最难，6 系统）

### Task 11: `UiSystem`

- `ui_`/`ShowUI`/`HideUI`/`UIClicked`/`UISetText`/`UISetFill`/`UISetVisible`/`UISetColor`/`DrawUI` → 独立类（`ui::IUiSystem` 已是接口，薄包装 + UI binding 转发）。commit `refactor: GameRuntime 拆出 UiSystem`

### Task 12: `AnimationSystem`（解 C2）

- 把 `DrawItem` 内的 anim 状态（`animClip`/`animTime`/`animFade`/`animSM` 等）**抽出**为 `AnimationSystem` 的 `entityKey → AnimState` 表。`DrawItem` 只保留渲染引用（skinned 模型指针）。`PlayAnimation`/`TickAnimations`/`AttachStateMachine`/`SetAnimParam`/`Progress`/`Finished` 移入。`Draw` 从 AnimationSystem 取 pose。**消除 headless 空转动画**（服务器不建 DrawItem/不 tick 动画）。commit `refactor: GameRuntime 拆出 AnimationSystem，动画状态移出 DrawItem`

### Task 13: `ScriptRuntime`

- `hosts_`/`scripts_`/`scriptCtx_`/`loadedScripts_`/`scriptFailed_`/`chunkHandlers_`/`injectedScriptHost_` + `AttachScripts`/`AttachOneScript`/`CallEntityFunctionHandle`/`ScriptHost`/`ScriptContext`/`HasScriptFunction`/`CallScriptFunction`/`SpawnEntity`/`ReadScript` → 独立类。与 `BtRuntime` 共享 `scriptCtx_`/`hosts_`（GameRuntime 持有共享引用传入）。commit `refactor: GameRuntime 拆出 ScriptRuntime`

### Task 14: `BtRuntime`

- `trees_`/`AttachTrees`/`CallScriptOnTree`/`EntityBlackboardValue`/`ActiveTreePath` → 独立类，共享 `scriptCtx_`/`hosts_`。commit `refactor: GameRuntime 拆出 BtRuntime`

### Task 15: `PhysicsBridge`

- `physics_`/`physicsAccum_`/`pluginPhysics_` + `RegisterSceneBodies`/`RegisterCharacters`/`SyncSceneBodies`/`RegisterAudioSources`/`PhysicsWorld` → 独立类，操作 `ecs::World&`（同步 transform）。commit `refactor: GameRuntime 拆出 PhysicsBridge`

### Task 16: `DrawSystem`

- `draws_`/`drawKeys_`/`drawBatches_`/`batchModels_`/`drawBvh_`/`bvhVisible_`/`drawOrder_`/`vegCache_` + `BuildDrawList`/`SyncDrawKeys`/`ResolveDrawItem`/`ResolveOrSkip`/`ResolveMeshKey`/`DrawVegetation`/`VegetationMesh`/`MeshForEntity`/`Draw` → 独立类，接受 `gfx::Renderer&`/`gfx::Camera&`。commit `refactor: GameRuntime 拆出 DrawSystem`

---

## 验收（全部完成后）

1. `neon_tests` 全程全绿（716 基线，不回归）。
2. `game_runtime.hpp` < 300 行（只留编排 + 共享状态转发）。
3. `engine/src/scene/game_runtime*.cpp` 拆到 `systems/*.cpp`，单文件 < 500 行。
4. headless 服务器 `Tick` 不触碰动画状态（Task 12 后）。
5. 三个入口（game/editor/server）行为不变。

## 风险与回滚

- 每任务独立 commit，可单独回滚。
- 子系统间隐式耦合（投射物→粒子、状态→脚本 host、Overlap→lag-comp）必须显式参数化，靠全量测试兜底。
- 阶段 3 的 Script/Bt 共享 `scriptCtx_`/`hosts_`，拆边界需谨慎（GameRuntime 持有共享引用传入两个系统）。
