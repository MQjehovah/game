# P1：连续生物群系地形 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把房车世界的固定 288 m 高度场换成引擎侧程序化、可无限流式加载的生物群系地形，并在上面接回现有 POI、道路与存档。

**Architecture:** 新增 `neon::world`（纯 CPU，进 `neon_core`）：确定性噪声 → 气候/生物群系权重 → 高度函数 → 道路/场地压平。`neon_gfx` 新增四叉树 LOD 流式地形渲染器与 8 层 `sampler2DArray` 地表材质。`neon_scene` 在 `SceneTerrain` 的 procedural 模式下创建世界并用新的 `GetGroundHeight` 驱动 Lua。P1 只做地形、道路、场地与贴地；草/树/真实水体/建筑换模属于后续批次。

**Tech Stack:** C++17、OpenGL 后端、Lua 脚本层、CMake/Ninja、内置 GLSL shader（`builtin_shaders.hpp`）、现有 Jolt/ECS/Scene 框架。

**Spec:** `docs/superpowers/specs/2026-11-04-rv-photoreal-biome-world-design.md`

## Global Constraints

- 验收后端是 **OpenGL**；不要打开 `NEON_ENABLE_VULKAN`，不要为 Vulkan 双写 shader。
- 世界生成必须是**确定性**的：同一 `seed` + 同一份 `WorldFeatures` 得到逐位相同的高度。
- 地形不是纯 `(x,z,seed)`：道路/场地是显式 feature，运行时 `AddPad` 必须能让附近 chunk 重建。
- 所有新代码放在 `neon::world`（`engine/include/neon/world/`、`engine/src/world/`），渲染相关放 `neon::gfx`，Lua 只调用现有脚本绑定。
- 不改玩法数值/UI；唯一允许的手感改动是 Task 15 的坡度牵引。
- 贴图一律 2K PNG、按层入库到 `projects/rv/assets/textures/terrain/<layer>/`；源文件不压缩成 BC。
- 每个任务结束必须 `cmake --build build/release` 通过，并提交一次。
- 测试用项目现有 `TEST/CHECK/CHECK_EQ/CHECK_NEAR`（`tests/helpers.hpp`），新测试文件要加进 `CMakeLists.txt` 的 `neon_tests` 源列表。
- Lua 侧现有 `terrainHeight(x, z)` 的名字和签名不变；老档没有 seed 时用 `1337`。

## Review Focus

- **Chunk 边界**：相机快速移动或斜跨 LOD 边界时，相邻块不能出现裂缝或高低台阶；同层相邻块共享边必须逐点等高。
- **道路/场地压平**：压平权重在道路中心线、路肩、场地边缘必须连续；不能出现房车/POI 悬空或半截埋进地形。
- **极端坐标**：离原点 ±100 km 时高度/法线/权重仍是有限值，不能出现 NaN 或精度崩塌导致的尖刺。
- **流式预算**：焦点在 chunk 边界抖动时不能每帧重建同一批 chunk；驻留数量必须有上限。
- **老档迁移**：缺 seed、缺 procedural 字段、POI 坐标落在新山体上时，加载后必须重新贴地且不丢内容。
- **陡坡牵引**：25°/35°/45° 三档必须单调、连续，平地不能误伤速度，垂直山壁不能被开上去。

---

### Task 1: 确定性噪声与 fBm

**Files:**
- Create: `engine/include/neon/world/noise.hpp`
- Create: `engine/src/world/noise.cpp`
- Test: `tests/test_world_noise.cpp`
- Modify: `CMakeLists.txt`（`neon_core` 源列表、`neon_tests` 源列表）

**Interfaces:**
- Consumes: nothing
- Produces:
  - `uint32_t neon::world::Hash2D(int x, int z, uint32_t seed)`
  - `float neon::world::ValueNoise2D(float x, float z, uint32_t seed)` → `[-1, 1]`
  - `float neon::world::Fbm2D(float x, float z, uint32_t seed, int octaves, float lacunarity, float gain)` → 约 `[-1, 1]`
  - `float neon::world::Ridged2D(float x, float z, uint32_t seed, int octaves)` → `[0, 1]`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_world_noise.cpp
#include "helpers.hpp"
#include "neon/world/noise.hpp"

TEST(WorldNoiseDeterministic) {
    CHECK_EQ(neon::world::Hash2D(3, -7, 42), neon::world::Hash2D(3, -7, 42));
    CHECK_NE(neon::world::Hash2D(3, -7, 42), neon::world::Hash2D(3, -7, 43));
    CHECK_NEAR(neon::world::ValueNoise2D(1.25f, -3.5f, 7),
               neon::world::ValueNoise2D(1.25f, -3.5f, 7), 1e-9);
}

TEST(WorldNoiseRangeAndContinuity) {
    float prev = neon::world::ValueNoise2D(0.0f, 0.0f, 1);
    for (int i = 1; i <= 200; ++i) {
        float x = static_cast<float>(i) * 0.05f;
        float v = neon::world::ValueNoise2D(x, 0.0f, 1);
        CHECK(v >= -1.0001f && v <= 1.0001f);
        CHECK_NEAR(v, prev, 0.08);   // 连续
        prev = v;
    }
    float r = neon::world::Ridged2D(11.3f, -4.2f, 9, 4);
    CHECK(r >= -0.0001f && r <= 1.0001f);
    float f = neon::world::Fbm2D(11.3f, -4.2f, 9, 5, 2.0f, 0.5f);
    CHECK(f >= -1.0001f && f <= 1.0001f);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败（找不到 `neon/world/noise.hpp`）。

- [ ] **Step 3: 实现噪声**

`Hash2D` 用整数混合（如 xorshift + 乘法），`ValueNoise2D` 用 quintic 插值的 value noise；`Fbm2D` 逐 octave 叠加并按 `gain` 衰减、`lacunarity` 放大；`Ridged2D` 用 `1 - |noise|` 的加权叠加。所有函数不得使用 `std::rand` 或静态可变状态。

- [ ] **Step 4: 运行测试通过**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="WorldNoise*"`
Expected: 2 个用例 PASS。

- [ ] **Step 5: 提交**

```bash
git add engine/include/neon/world/noise.hpp engine/src/world/noise.cpp tests/test_world_noise.cpp CMakeLists.txt
git commit -m "feat(world): deterministic noise and fBm"
```

---

### Task 2: 气候场与生物群系权重

**Files:**
- Create: `engine/include/neon/world/biome.hpp`
- Create: `engine/src/world/biome.cpp`
- Test: `tests/test_biome.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `world::Fbm2D`
- Produces:
  - `enum class Biome : uint8_t { Grassland, Forest, Badlands, Alpine, Count };`
  - `struct Climate { float temperature = 0.0f; float moisture = 0.0f; };`
  - `struct BiomeWeights { float w[4] = {1,0,0,0}; float At(Biome) const; float Sum() const; void Normalize(); };`
  - `Climate SampleClimate(float x, float z, uint32_t seed)`
  - `BiomeWeights SampleBiomeWeights(float x, float z, uint32_t seed)`
  - `Biome DominantBiome(const BiomeWeights&)`

- [ ] **Step 1: 写失败测试**

```cpp
#include "helpers.hpp"
#include "neon/world/biome.hpp"

TEST(BiomeWeightsNormalizedAndDeterministic) {
    for (int i = 0; i < 500; ++i) {
        float x = static_cast<float>(i % 25) * 80.0f - 1000.0f;
        float z = static_cast<float>(i / 25) * 80.0f - 800.0f;
        auto w = neon::world::SampleBiomeWeights(x, z, 1337);
        CHECK_NEAR(w.Sum(), 1.0f, 1e-4);
        for (int b = 0; b < 4; ++b) CHECK(w.w[b] >= 0.0f && w.w[b] <= 1.0f);
        auto w2 = neon::world::SampleBiomeWeights(x, z, 1337);
        for (int b = 0; b < 4; ++b) CHECK_NEAR(w.w[b], w2.w[b], 1e-9);
    }
}

TEST(BiomeCampIsGrasslandAndAllBiomesReachable) {
    using namespace neon::world;
    CHECK(DominantBiome(SampleBiomeWeights(0.0f, 0.0f, 1337)) == Biome::Grassland);
    bool seen[4] = {false, false, false, false};
    for (int z = -2000; z <= 2000; z += 100)
        for (int x = -2000; x <= 2000; x += 100)
            seen[static_cast<int>(DominantBiome(SampleBiomeWeights(
                static_cast<float>(x), static_cast<float>(z), 1337)))] = true;
    for (bool b : seen) CHECK(b);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现气候与权重**

`temperature` 用低频 fBm（尺度 `1/900`）并减去北向海拔趋势；`moisture` 用另一组 seed 的 fBm（尺度 `1/650`）。权重用气候空间里的 4 个锚点做软分配（smoothstep 后归一化）；营地 `(0,0)` 必须落在草原锚点。不要写死圆形区域。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="Biome*"`
Expected: PASS。

```bash
git add engine/include/neon/world/biome.hpp engine/src/world/biome.cpp tests/test_biome.cpp CMakeLists.txt
git commit -m "feat(world): climate and biome weights"
```

---

### Task 3: 世界要素与压平

**Files:**
- Create: `engine/include/neon/world/world_features.hpp`
- Create: `engine/src/world/world_features.cpp`
- Test: `tests/test_world_features.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `math::Vec2`
- Produces:
  - `struct RoadSpec { std::vector<math::Vec2> points; float halfWidth = 4.0f; float shoulder = 3.0f; };`
  - `struct PadSpec { math::Vec2 center; float radius = 12.0f; float falloff = 10.0f; };`
  - `struct WorldFeatures { std::vector<RoadSpec> roads; std::vector<PadSpec> pads; };`
  - `struct FlattenSample { float weight = 0.0f; float height = 0.0f; };`
  - `using HeightFn = std::function<float(float, float)>;`
  - `float DistanceToPolyline(const std::vector<math::Vec2>& pts, math::Vec2 p, math::Vec2* closest)`
  - `FlattenSample SampleFlattening(const WorldFeatures& f, float x, float z, const HeightFn& baseHeight)`
  - `void AddPad(WorldFeatures& f, math::Vec2 center, float radius, float falloff = 10.0f)`

`baseHeight(x, z)` 是**未压平**的地形高度回调；道路目标高度取最近中心线点的 `baseHeight`，场地目标高度取中心点的 `baseHeight`。

- [ ] **Step 1: 写失败测试**

```cpp
#include "helpers.hpp"
#include "neon/world/world_features.hpp"

TEST(FlattenRoadCoreAndShoulder) {
    using namespace neon::world;
    WorldFeatures f;
    f.roads.push_back({{{0,0},{0,200}}, 4.0f, 3.0f});
    auto flat9 = [](float, float) { return 9.0f; };
    // 道路中心：完全压平到中心线高度。
    auto core = SampleFlattening(f, 0.0f, 50.0f, flat9);
    CHECK_NEAR(core.weight, 1.0f, 1e-4);
    CHECK_NEAR(core.height, 9.0f, 1e-4);
    // 路肩外：不压平。
    auto out = SampleFlattening(f, 9.0f, 50.0f, flat9);
    CHECK_NEAR(out.weight, 0.0f, 1e-4);
}

TEST(FlattenPadFalloffMonotonicAndDeterministic) {
    using namespace neon::world;
    WorldFeatures f;
    AddPad(f, {100.0f, 100.0f}, 12.0f, 10.0f);
    auto flat7 = [](float, float) { return 7.0f; };
    float prev = 2.0f;
    for (int i = 0; i <= 20; ++i) {
        auto s = SampleFlattening(f, 100.0f + static_cast<float>(i), 100.0f, flat7);
        CHECK(s.weight <= prev + 1e-5f);
        CHECK(s.height >= 0.0f);
        prev = s.weight;
    }
    CHECK_NEAR(SampleFlattening(f, 100.0f, 100.0f, flat7).weight, 1.0f, 1e-4);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现压平**

道路：对每条折线取最近点，距离 `<= halfWidth` 权重 1，`halfWidth..halfWidth+shoulder` 用 smoothstep 降到 0；目标高度取折线最近点的 `baseHeight`（这样路面跟随山势）。场地：距离 `<= radius` 权重 1，`radius..radius+falloff` 衰减到 0；目标高度 = 场地中心 `baseHeight`。多个要素取**权重最大**的那个，不叠加高度。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="Flatten*"`
Expected: PASS。

```bash
git add engine/include/neon/world/world_features.hpp engine/src/world/world_features.cpp tests/test_world_features.cpp CMakeLists.txt
git commit -m "feat(world): road and pad flattening"
```

---

### Task 4: TerrainWorld 高度与法线

**Files:**
- Create: `engine/include/neon/world/terrain_world.hpp`
- Create: `engine/src/world/terrain_world.cpp`
- Test: `tests/test_terrain_world.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `world::SampleBiomeWeights`、`world::SampleFlattening`、`world::Fbm2D/Ridged2D`
- Produces:
  - `struct TerrainWorldConfig { uint32_t seed = 1337; float biomeScale = 1.0f / 420.0f; WorldFeatures features; float mountainStartZ = 300.0f; };`
  - `class TerrainWorld { public: explicit TerrainWorld(TerrainWorldConfig); float Height(float x, float z) const; math::Vec3 Normal(float x, float z) const; BiomeWeights Weights(float x, float z) const; const TerrainWorldConfig& Config() const; };`

- [ ] **Step 1: 写失败测试**

```cpp
#include <cmath>
#include "helpers.hpp"
#include "neon/world/terrain_world.hpp"

TEST(TerrainWorldCampIsFlatAndNorthRises) {
    neon::world::TerrainWorld w({1337, 1.0f/420.0f, {}, 300.0f});
    // 营地核心 40 m 内接近平地。
    for (int i = 0; i < 32; ++i) {
        float a = static_cast<float>(i) * 0.196f;
        for (float r = 0.0f; r <= 40.0f; r += 10.0f)
            CHECK(std::fabs(w.Height(std::cos(a)*r, std::sin(a)*r)) < 0.6f);
    }
    // 北侧雪山必须明显抬升。
    CHECK(w.Height(0.0f, 800.0f) > 30.0f);
}

TEST(TerrainWorldDeterministicAndFinite) {
    neon::world::TerrainWorld a({7, 1.0f/420.0f, {}, 300.0f});
    neon::world::TerrainWorld b({7, 1.0f/420.0f, {}, 300.0f});
    for (int i = 0; i < 200; ++i) {
        float x = static_cast<float>(i) * 37.0f - 3700.0f;
        float z = static_cast<float>(i) * -53.0f + 5300.0f;
        CHECK_NEAR(a.Height(x, z), b.Height(x, z), 1e-6);
    }
    for (float x : {-100000.0f, 0.0f, 100000.0f})
        for (float z : {-100000.0f, 0.0f, 100000.0f}) {
            CHECK(std::isfinite(a.Height(x, z)));
            auto n = a.Normal(x, z);
            CHECK(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
            CHECK_NEAR(n.y, 1.0f, 0.5f);
        }
}

TEST(TerrainWorldRoadStaysFlat) {
    neon::world::WorldFeatures f;
    f.roads.push_back({{{0,0},{0,400}}, 4.0f, 4.0f});
    neon::world::TerrainWorld w({1337, 1.0f/420.0f, f, 300.0f});
    float h0 = w.Height(0.0f, 60.0f);
    for (float z = 60.0f; z <= 70.0f; z += 1.0f)
        CHECK_NEAR(w.Height(0.0f, z), h0, 0.35f);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现高度混合**

每个群系一个高度函数：草原 = 缓丘 fBm（幅度 ~6 m）；森林 = fBm + 沟谷（~14 m）；荒原 = 台地量化 + 冲沟（~22 m）；高山 = ridged 山脊，只在 `z > mountainStartZ` 后按 smoothstep 抬升（幅度 ~90 m）。用 `BiomeWeights` 线性混合得到 `RawHeight`；营地出生区（原点半径 45 m）用 smoothstep 压到 0（现有 POI 都锚在这里）；再 `SampleFlattening(..., [this](float x, float z){ return RawHeight(x, z); })` 覆盖得到 `Height`。`Normal` 用中心差分（步长 0.5 m）并归一化。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="TerrainWorld*"`
Expected: PASS。

```bash
git add engine/include/neon/world/terrain_world.hpp engine/src/world/terrain_world.cpp tests/test_terrain_world.cpp CMakeLists.txt
git commit -m "feat(world): biome-blended procedural terrain height"
```

---

### Task 5: 四叉树 LOD 与 chunk CPU 网格

**Files:**
- Create: `engine/include/neon/world/terrain_chunk.hpp`
- Create: `engine/src/world/terrain_chunk.cpp`
- Test: `tests/test_terrain_chunk.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `world::TerrainWorld`
- Produces:
  - `struct TerrainVertex { math::Vec3 position; math::Vec3 normal; math::Vec2 uv; math::Vec4 weights; math::Vec4 tint; };`
  - `struct TerrainChunkKey { int level = 0; int x = 0; int z = 0; bool operator==(const TerrainChunkKey&) const; };`
  - `float TerrainChunkSize(int level)` → `32.0f * 2^level`
  - `void SelectTerrainChunks(math::Vec2 focus, float viewDistance, int maxLevel, std::vector<TerrainChunkKey>& out)`
  - `struct TerrainChunkMeshData { std::vector<TerrainVertex> vertices; std::vector<uint32_t> indices; };`
  - `void BuildTerrainChunkData(const TerrainWorld&, const TerrainChunkKey&, int cells, float skirtDepth, TerrainChunkMeshData& out)`

- [ ] **Step 1: 写失败测试**

```cpp
#include <set>
#include "helpers.hpp"
#include "neon/world/terrain_chunk.hpp"

TEST(TerrainChunksCoverFocusWithoutOverlap) {
    std::vector<neon::world::TerrainChunkKey> keys;
    neon::world::SelectTerrainChunks({0.0f, 0.0f}, 512.0f, 4, keys);
    CHECK(keys.size() > 8u);
    CHECK(keys.size() < 600u);
    std::set<std::tuple<int,int,int>> seen;
    for (auto& k : keys) CHECK(seen.insert({k.level, k.x, k.z}).second);
    // 焦点所在区域必须被覆盖：至少有一个叶子跨过原点。
    bool coversOrigin = false;
    for (auto& k : keys) {
        float s = neon::world::TerrainChunkSize(k.level);
        float x0 = static_cast<float>(k.x) * s, z0 = static_cast<float>(k.z) * s;
        if (x0 <= 0.0f && 0.0f < x0 + s && z0 <= 0.0f && 0.0f < z0 + s) coversOrigin = true;
    }
    CHECK(coversOrigin);
}

TEST(TerrainChunkMeshMatchesHeightAndHasSkirt) {
    neon::world::TerrainWorld w({1337, 1.0f/420.0f, {}, 300.0f});
    neon::world::TerrainChunkMeshData m;
    neon::world::BuildTerrainChunkData(w, {0, 0, 0}, 8, 2.0f, m);
    // 8x8 网格 = 9x9 = 81 个面顶点，外加裙边 4 条边。
    CHECK(m.vertices.size() >= 81u);
    CHECK(!m.indices.empty());
    for (auto& v : m.vertices) {
        CHECK(std::isfinite(v.position.y));
        CHECK_NEAR(v.normal.Length(), 1.0f, 1e-3);
    }
    // 网格点高度必须与 Height 一致（前 81 个顶点是正常网格）。
    // level 0 的块边长 32 m，8 个 cell -> 每格 4 m。
    const float cell = neon::world::TerrainChunkSize(0) / 8.0f;
    for (int j = 0; j <= 8; ++j)
        for (int i = 0; i <= 8; ++i) {
            float x = static_cast<float>(i) * cell, z = static_cast<float>(j) * cell;
            CHECK_NEAR(m.vertices[j * 9 + i].position.y, w.Height(x, z), 0.02);
        }
}

TEST(TerrainChunkAdjacentEdgesMatch) {
    neon::world::TerrainWorld w({1337, 1.0f/420.0f, {}, 300.0f});
    neon::world::TerrainChunkMeshData a, b;
    neon::world::BuildTerrainChunkData(w, {0, 0, 0}, 8, 2.0f, a);
    neon::world::BuildTerrainChunkData(w, {0, 1, 0}, 8, 2.0f, b);  // 东邻（x+1）
    for (int j = 0; j <= 8; ++j) {
        CHECK_NEAR(a.vertices[j * 9 + 8].position.y,
                   b.vertices[j * 9 + 0].position.y, 1e-3);
        CHECK_NEAR(a.vertices[j * 9 + 8].position.x,
                   b.vertices[j * 9 + 0].position.x, 1e-3);
    }
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现选择与建网格**

LOD 选择：从 `maxLevel` 向下递归，一个块在「到焦点距离 < 块尺寸 × 2」时继续细分，否则作为叶子；只输出叶子。叶子尺寸固定 `32 << level`，每块固定 `cells × cells`（测试传 8，运行时用 16 或 32）。裙边：四条边各向下延伸 `skirtDepth`，防止 LOD 裂缝。`weights = (soil, rock, snow, wet)`，按坡度/高度/群系计算；`tint` 存群系主色（草原绿/森林深绿/荒原黄褐/雪原冷白）。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="TerrainChunk*"`
Expected: PASS。

```bash
git add engine/include/neon/world/terrain_chunk.hpp engine/src/world/terrain_chunk.cpp tests/test_terrain_chunk.cpp CMakeLists.txt
git commit -m "feat(world): quadtree LOD and chunk mesh generation"
```

---

### Task 6: 纹理数组 + 地形材质字段

**Files:**
- Modify: `engine/include/neon/gfx/backend.hpp`（`TextureDesc` 增加 `int layers = 1;`）
- Modify: `engine/src/gfx/gl/gl_backend.cpp`（`CreateTexture` 支持 `layers > 1`）
- Modify: `engine/include/neon/gfx/material.hpp`（新增地形字段）
- Modify: `engine/src/gfx/renderer.cpp:FindMaterial`/`DrawMesh`（绑定 unit 5/6/7）
- Test: `tests/test_terrain_material.cpp`

**Interfaces:**
- Consumes: `gfx::TextureDesc`
- Produces:
  - `TextureDesc::layers`
  - `Material::terrainAlbedoArray`、`terrainNormalArray`、`terrainOrmArray`（`TextureHandle`）
  - `Material::terrainLayers[4]`（`int`，默认 `{0,1,2,3}`）
  - `Material::terrainUvScale[4]`（`float`，默认 `{0.25f,0.25f,0.1f,0.1f}`）
  - `Material::terrainSnowLine`（`float`，默认 `0.0f` = 关闭）

- [ ] **Step 1: 写失败测试**

```cpp
#include "helpers.hpp"
#include "neon/gfx/renderer.hpp"
#include "test_backend.hpp"

TEST(TerrainMaterialDefaultsAndArrayTexture) {
    neon::gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    neon::gfx::TextureDesc desc;
    desc.width = 4; desc.height = 4; desc.layers = 8;
    unsigned char px[4 * 4 * 8 * 4] = {};
    desc.rgba = px;
    auto tex = renderer.CreateTexture(desc);
    CHECK(tex.Valid());

    neon::gfx::Material m;
    CHECK_EQ(m.terrainLayers[0], 0);
    CHECK_EQ(m.terrainLayers[3], 3);
    CHECK_NEAR(m.terrainUvScale[0], 0.25, 1e-6);
    m.terrainAlbedoArray = tex;
    CHECK(m.terrainAlbedoArray.Valid());
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期 `layers`/`terrainLayers` 不存在。

- [ ] **Step 3: 实现后端与材质**

GL：`layers > 1` 时用 `glTexImage3D(GL_TEXTURE_2D_ARRAY, ..., layers, ..., GL_RGBA, GL_UNSIGNED_BYTE, rgba)`，mipmap 用 `glGenerateMipmap`。`NullBackend` 的 `CreateTexture` 已忽略参数，无需改动。`DrawMesh` 把三个 array 绑到 unit 5/6/7，设置 `uTerrainAlbedo=5`、`uTerrainNormal=6`、`uTerrainOrm=7`、`uTerrainLayer0..3`、`uTerrainUvScale0..3`、`uTerrainSnowLine`。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="TerrainMaterial*"`
Expected: PASS。

```bash
git add engine/include/neon/gfx/backend.hpp engine/src/gfx/gl/gl_backend.cpp engine/include/neon/gfx/material.hpp engine/src/gfx/renderer.cpp tests/test_terrain_material.cpp CMakeLists.txt
git commit -m "feat(gfx): texture arrays and terrain material layers"
```

---

### Task 7: 8 层地表 shader

**Files:**
- Modify: `engine/include/neon/gfx/builtin_shaders.hpp`（`TERRAIN_SPLAT` 分支替换/扩展）
- Modify: `engine/src/gfx/renderer.cpp`（注册 `lit_terrain_layers`）
- Modify: `engine/include/neon/gfx/renderer.hpp`（`TerrainLayersShader()`）

**Interfaces:**
- Consumes: `Material::terrain*` 字段
- Produces: `ShaderHandle TerrainLayersShader() const`

- [ ] **Step 1: 写验证脚本（可执行检查）**

在 `docs/visual_acceptance.md` 增加一节：

```bat
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_terrain.png 120
```

预期：控制台无 `shader compile failed`（此时场景还没切到新地形，材质层次在 Task 15 验收）。

- [ ] **Step 2: 实现 shader**

在 `kLitFragmentShader` 里新增 `#ifdef TERRAIN_LAYERS`：用 `sampler2DArray` 采样 4 个层，`uv` 来自 `vWorldPos.xz * uTerrainUvScale[i]`；权重来自 `vColor`（同时保留旧的 `TERRAIN_SPLAT` 路径不动）。坡度 > 0.55 时用三平面投影（按 `abs(N)` 混合三个平面）。`terrainSnowLine > 0` 时按世界 Y 叠加雪层。法线 = 几何法线 + 各层法线按权重混合（无切线时沿用现有屏幕空间 TBN）。

- [ ] **Step 3: 注册 shader 变体**

`renderer.cpp` 的 shader 初始化处加：

```cpp
createShader("lit_terrain_layers", kLitVertexShader, kLitFragmentShader,
             "#define TERRAIN_LAYERS 1\n");
```

`renderer.hpp` 加 `ShaderHandle TerrainLayersShader() const { return Program("lit_terrain_layers"); }`。

- [ ] **Step 4: 构建并在真实场景里确认编译**

Run: `cmake --build build/release && build/release/neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_shader.png 60`
Expected: 程序启动，无 shader 编译错误，截图非纯色。

- [ ] **Step 5: 提交**

```bash
git add engine/include/neon/gfx/builtin_shaders.hpp engine/src/gfx/renderer.cpp engine/include/neon/gfx/renderer.hpp docs/visual_acceptance.md
git commit -m "feat(gfx): 4-layer splat terrain shader"
```

---

### Task 8: 地形流式渲染器

**Files:**
- Create: `engine/include/neon/gfx/terrain_streamer.hpp`
- Create: `engine/src/gfx/terrain_streamer.cpp`
- Modify: `CMakeLists.txt`（`neon_gfx` 源列表）
- Test: `tests/test_terrain_streamer.cpp`

**Interfaces:**
- Consumes: `world::SelectTerrainChunks`、`world::BuildTerrainChunkData`、`gfx::Renderer`
- Produces:
  - `struct TerrainStreamerConfig { int maxLevel = 5; int cells = 16; float viewDistance = 2048.0f; size_t maxResident = 256; int buildsPerFrame = 2; float skirtDepth = 4.0f; };`
  - `class TerrainStreamer { public: void Configure(const world::TerrainWorld*, const TerrainStreamerConfig&); void Update(Renderer&, const math::Vec3& focus, float dt); void Draw(Renderer&, const math::Mat4& viewProj, const Material& terrainMaterial); void Clear(Renderer&); size_t Resident() const; const TerrainStreamerConfig& Config() const; };`

- [ ] **Step 1: 写失败测试**

```cpp
#include "helpers.hpp"
#include "neon/gfx/terrain_streamer.hpp"
#include "test_backend.hpp"

TEST(TerrainStreamerResidencyBudgetAndHysteresis) {
    neon::gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    neon::world::TerrainWorld world({1337, 1.0f/420.0f, {}, 300.0f});
    neon::gfx::TerrainStreamer s;
    neon::gfx::TerrainStreamerConfig cfg;
    cfg.maxLevel = 2; cfg.viewDistance = 128.0f; cfg.maxResident = 40;
    s.Configure(&world, cfg);
    // 分多帧喂，模拟 buildsPerFrame 预算。
    for (int i = 0; i < 60; ++i) s.Update(renderer, {0, 0, 0}, 0.016f);
    CHECK(s.Resident() > 0u);
    CHECK(s.Resident() <= cfg.maxResident);
    size_t before = s.Resident();
    for (int i = 0; i < 3; ++i) s.Update(renderer, {0, 0, 0}, 0.016f);
    CHECK_EQ(s.Resident(), before);          // 焦点不动不重建
    for (int i = 0; i < 60; ++i) s.Update(renderer, {4096, 0, 0}, 0.016f);
    CHECK(s.Resident() <= cfg.maxResident);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现**

`Update` 做三件事：1) 用 `SelectTerrainChunks` 算目标集合；2) 卸载不在集合里的 chunk；3) 每帧最多建 `buildsPerFrame` 个缺失 chunk（`BuildTerrainChunkData` → `Renderer::CreateMesh`）。`Draw` 遍历 resident，按 chunk 包围盒做视锥剔除，选材质（`TerrainLayersShader()`）并按 chunk 的 `Biome` 设置 hint（通过 `Material::tint` 或顶点 tint 已足够）。`Clear` 释放所有 mesh。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="TerrainStreamer*"`
Expected: PASS。

```bash
git add engine/include/neon/gfx/terrain_streamer.hpp engine/src/gfx/terrain_streamer.cpp tests/test_terrain_streamer.cpp CMakeLists.txt
git commit -m "feat(gfx): streamed LOD terrain renderer"
```

---

### Task 9: SceneTerrain procedural + GameRuntime 接线

**Files:**
- Modify: `engine/include/neon/scene/scene_file.hpp`（`SceneTerrain` 新增字段）
- Modify: `engine/src/scene/scene_file.cpp`（序列化/反序列化）
- Modify: `engine/src/scene/game_runtime.cpp`（创建 `TerrainWorld` + `TerrainStreamer`，`groundHeight` 走新世界）
- Modify: `engine/include/neon/scene/game_runtime.hpp`（`float GroundHeight(float x, float z) const`）
- Test: `tests/test_scene_file.cpp` 追加用例、`tests/test_game_runtime.cpp` 追加用例

**Interfaces:**
- Consumes: `world::TerrainWorld`、`gfx::TerrainStreamer`
- Produces:
  - `SceneTerrain::procedural`（`bool`，默认 false）
  - `SceneTerrain::worldSeed`（`uint32_t`，默认 1337）
  - `SceneTerrain::terrainViewDistance`（`float`，默认 2048）
  - `SceneTerrain::terrainCellSize`（`float`，默认 1.0）
  - `float GameRuntime::GroundHeight(float x, float z) const`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_game_runtime.cpp
TEST(GameRuntimeProceduralTerrainGroundHeight) {
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    const char* scene =
        R"({"entities":[{"name":"ground","components":{
            "transform":{"pos":[0,0,0]},
            "mesh":{"meshKey":"terrain"},
            "terrain":{"procedural":true,"worldSeed":1337,
                       "terrainViewDistance":1024,"terrainCellSize":1.0}}}]})";
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    CHECK(runtime.Start(scene, cfg).Ok());
    auto view = runtime.World().ViewAll<scene::SceneTerrain>();
    CHECK_EQ(view.Size(), 1u);
    const scene::SceneTerrain* t = runtime.World().Get<scene::SceneTerrain>(
        runtime.World().EntityAt<scene::SceneTerrain>(0));
    CHECK(t->procedural);
    CHECK_EQ(t->worldSeed, 1337u);
    CHECK(std::isfinite(runtime.GroundHeight(0.0f, 0.0f)));
    CHECK(runtime.GroundHeight(0.0f, 800.0f) > 30.0f);
    runtime.Stop();
}
```

（复用 `tests/test_terrain.cpp` 里 `StartFlatTerrain` 的同一套 `GameRuntime.Start + HeadlessAssetFixture` 写法；不要新写解析器。`SceneTerrain` 组件的实例化在 `scene_file.cpp` 的 terrain 分支里补新字段。）

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期编译失败。

- [ ] **Step 3: 实现**

`SceneTerrain` 新增字段并在 JSON 读写里带上。`GameRuntime` 在 `Init`/场景载入时：若存在 `procedural == true` 的 `SceneTerrain`，构造 `TerrainWorld`（features 先留空，Task 12 再接 `world_features.json`），创建 `TerrainStreamer`，并把 `scriptCtx_.groundHeight` 改成 `[this](x,z){ return terrainWorld_->Height(x,z); }`。每帧更新 streamer 焦点为相机位置。旧的 heightfield 路径保持不变（`procedural == false`）。

- [ ] **Step 4: 运行测试通过并提交**

Run: `cmake --build build/release --target neon_tests && build/release/neon_tests.exe --test-case="*Terrain*"`
Expected: PASS（含旧 heightfield 测试）。

```bash
git add engine/include/neon/scene/scene_file.hpp engine/src/scene/scene_file.cpp engine/src/scene/game_runtime.cpp engine/include/neon/scene/game_runtime.hpp tests/test_scene_file.cpp tests/test_game_runtime.cpp
git commit -m "feat(scene): procedural terrain world in runtime"
```

---

### Task 10: Lua 地形采样切换 + seed 存档

**Files:**
- Modify: `projects/rv/assets/scripts/lc/lc_terrain.lua`
- Modify: `projects/rv/assets/scripts/lc/lc_util.lua`（新增 `groundAt`）
- Modify: `projects/rv/assets/scripts/lc/lc_json.lua`
- Modify: `projects/rv/assets/scripts/lc/lc_state.lua`
- Modify: `projects/rv/assets/scripts/garage.lua`
- Modify: `projects/rv/assets/scripts/lc/lc_selftest.lua`

**Interfaces:**
- Consumes: `GetGroundHeight` 脚本绑定
- Produces:
  - `terrainHeight(x, z)` 直接返回 `GetGroundHeight(x, z)`（无绑定才回落旧 JSON）
  - `groundAt(x, z)`（`lc_util`）= `terrainHeight` 的别名
  - 存档 v6：`layout.world = { seed = <number> }`

- [ ] **Step 1: 写 selftest（失败）**

在 `lc_selftest.lua` 加：

```lua
local function selftestTerrainWorld()
  local ok = true
  -- 确定性：同一点采样两次一致
  ok = ok and terrainHeight(12.5, -33.25) == terrainHeight(12.5, -33.25)
  -- 有限：远处不 NaN
  local h = terrainHeight(100000, -100000)
  ok = ok and h == h and math.abs(h) < 10000
  -- 营地平地
  ok = ok and math.abs(terrainHeight(0, 0)) < 0.6
  -- 北侧抬升
  ok = ok and terrainHeight(0, 800) > 30
  -- 老档没有 world.seed：迁移后默认 1337，不崩
  local oldLayout = { version = 5, rv = { x = 0, z = 0, yaw = 0 } }
  local seed = (oldLayout.world and oldLayout.world.seed) or 1337
  ok = ok and seed == 1337
  return ok, "terrain world"
end
```

- [ ] **Step 2: 运行确认失败**

Run:
```bat
echo selftest > projects\rv\assets\scripts\selftest_on.txt
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --smoke-test 300
type projects\rv\assets\scripts\selftest_result.txt
```
Expected: 新用例 FAIL（`terrainHeight` 仍读旧 JSON，`terrainHeight(0,800)` 为 0）。

- [ ] **Step 3: 改 Lua**

`terrainHeight` 优先 `GetGroundHeight`；删除对 `rv_terrain.json` 的依赖（保留兜底函数，文件缺失时仍能跑）。存档 v6 写入 `world.seed`，读档缺省 `1337`。`garage.json` 的 terrain 实体加 `procedural: true, worldSeed: 1337`（Task 9 的字段）。删除 selftest 里对旧高度常量（`mid=2.86` 等）的断言，改成上面的相对断言。

- [ ] **Step 4: 运行 selftest 通过并提交**

Run: 同上命令
Expected: `selftest_result.txt` 里 `terrain world` 为 PASS，且旧用例不回归。

```bash
git add projects/rv/assets/scripts/lc/lc_terrain.lua projects/rv/assets/scripts/lc/lc_json.lua projects/rv/assets/scripts/lc/lc_state.lua projects/rv/assets/scripts/garage.lua projects/rv/assets/scripts/lc/lc_selftest.lua projects/rv/assets/scenes/garage.json
git commit -m "feat(rv): sample heights from procedural terrain world"
```

---

### Task 11: 现有内容重新贴地

**Files:**
- Modify: `projects/rv/assets/scripts/lc/lc_scenery.lua`（POI/道具/码头/池塘的 y）
- Modify: `projects/rv/assets/scripts/lc/lc_util.lua`（`groundAt`）
- Modify: `projects/rv/assets/scripts/lc/lc_selftest.lua`

**Interfaces:**
- Consumes: `terrainHeight`
- Produces: `groundAt(x, z)` → world Y；所有 `spawnRuinFor`/`placeAt`/`spawnPoi*`/`spawnPond`/`spawnDock` 的基准 y 改为 `groundAt(x,z)` + 原偏移。

- [ ] **Step 1: 写 selftest（失败）**

```lua
local function selftestPoisGrounded()
  for _, nd in ipairs(NODES or {}) do
    if nd.building == nil or nd.marker == nil then
      return false, "poi not spawned: " .. tostring(nd.name)
    end
    for _, ent in ipairs({ nd.building, nd.marker }) do
      local p = GetPosition(ent)
      if math.abs(p.y - groundAt(p.x, p.z)) > 0.5 then
        return false, "poi not grounded: " .. tostring(nd.name)
      end
    end
  end
  return true, "pois grounded"
end
```

（`nd.building` / `nd.marker` 在 `trySpawnPoi` / `spawnMarkerFor` 创建实体时记录；如果节点表已有别的句柄字段，用它替换。）

- [ ] **Step 2: 运行确认失败**

Run selftest（命令同 Task 10）→ 预期 POI 悬空/埋地失败。

- [ ] **Step 3: 实现贴地**

新增 `groundAt`。把所有固定 `y = 0.0` / `y = 0.02` 的摆放改成 `groundAt(x, z) + offset`；房车初始位置、模块、网格贴花也要用 `groundAt`。建筑内部地面保持水平（用建筑中心高度），只抬整体，不在建筑内部做斜坡。

- [ ] **Step 4: 运行 selftest 通过并提交**

Run selftest → `pois grounded` PASS；再跑 `autopilot_on.txt` 的驾驶回路，确认不穿地。

```bash
git add projects/rv/assets/scripts/lc/lc_scenery.lua projects/rv/assets/scripts/lc/lc_util.lua projects/rv/assets/scripts/lc/lc_selftest.lua
git commit -m "feat(rv): ground existing POIs and props on procedural terrain"
```

---

### Task 12: 远端 POI 与道路延伸

**Files:**
- Modify: `projects/rv/assets/data/worldmap.json`（新增 6 个节点与链接）
- Modify: `projects/rv/assets/scripts/lc/lc_config.lua`（节点表/场景池）
- Modify: `projects/rv/assets/scripts/lc/lc_scenery.lua`（新 POI 的 prefab 选择与场地）
- Create: `projects/rv/assets/prefabs/rv_ski_lodge.json` 等 6 个占位 prefab（几何可以先复用现有建筑，P4 再换模）
- Modify: `engine/src/scene/game_runtime.cpp` 或新增 `projects/rv/assets/data/world_features.json`（道路样条 + 场地清单）

**Interfaces:**
- Consumes: `WorldFeatures` 数据
- Produces: `WorldFeatures LoadWorldFeatures(const std::string& jsonPath)`（world_features.cpp 新增），从 `world_features.json` 读 `roads[]` 与 `pads[]`。

- [ ] **Step 1: 写数据校验测试**

```cpp
TEST(WorldFeaturesJsonLoadsRoadsAndPads) {
    // 读 projects/rv/assets/data/world_features.json
    // 断言 roads >= 14 条、pads >= 15 个、所有坐标有限。
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build/release --target neon_tests` → 预期 JSON 不存在/字段缺失。

- [ ] **Step 3: 写数据与内容**

把现有 13 条道路折线、9 个 POI 场地写进 `world_features.json`；追加雪原 3 条路 + 3 场地、林场 2 条 + 2 场地、矿场 1 条 + 1 场地。`worldmap.json` 增加 6 个节点（名字/危险度/战利品/链接）。`lc_config` 的节点表同步。场地半径覆盖建筑 + 10 m 空地。

- [ ] **Step 4: 运行测试与实机检查**

Run: `cmake --build build/release && build/release/neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_snow_poi.png 120`
Expected: 地图上能看到新节点；开到雪原 POI 时建筑贴地、无悬空。

- [ ] **Step 5: 提交**

```bash
git add projects/rv/assets/data/worldmap.json projects/rv/assets/data/world_features.json projects/rv/assets/scripts/lc/lc_config.lua projects/rv/assets/scripts/lc/lc_scenery.lua projects/rv/assets/prefabs/rv_*.json engine/src/world/world_features.cpp tests/test_world_features.cpp
git commit -m "feat(rv): remote biome POIs and extended road network"
```

---

### Task 13: CC0 地表材质管线

**Files:**
- Create: `tools/fetch_rv_terrain_textures.py`
- Create: `projects/rv/assets/textures/terrain/<layer>/{albedo,normal,orm}.png`（8 层 × 3 张，2K）
- Create: `projects/rv/assets/data/terrain_layers.json`（层名、路径、UV scale、默认混合参数）
- Modify: `engine/src/gfx/terrain_streamer.cpp` 或 `game_runtime.cpp`（加载 8 层并创建 array 纹理）

**Interfaces:**
- Consumes: `gfx::Renderer::CreateTexture(TextureDesc{layers})`
- Produces: `TerrainLayerSet LoadTerrainLayers(const std::string& manifest, Renderer&)` → 三个 `TextureHandle` + 层参数

- [ ] **Step 1: 写数据完整性检查**

```python
# tools/fetch_rv_terrain_textures.py --check
# 断言 8 个层目录都存在，每层 albedo/normal/orm 都是 2048x2048 PNG；
# 缺文件时打印缺失清单并以非零码退出。
```

- [ ] **Step 2: 运行确认失败**

Run: `python tools/fetch_rv_terrain_textures.py --check` → 预期缺文件、非零退出。

- [ ] **Step 3: 实现下载与转换**

脚本按 `terrain_layers.json` 的清单从 Poly Haven / ambientCG 下载 2K 源图，转成 2048² PNG（albedo 保留 sRGB；normal 保留切线空间；orm 打包 `R=AO, G=Roughness, B=0`）。已存在且尺寸正确则跳过（幂等）。网络失败时打印手动下载清单。

- [ ] **Step 4: 接入渲染**

`LoadTerrainLayers` 读 PNG（已有 stb_image），按层拼成连续内存，调用 `CreateTexture` 一次上传 `layers=8` 的三个 array。缺失时用 1×1 中性色兜底，保证无贴图也能跑。

- [ ] **Step 5: 运行检查与提交**

Run: `python tools/fetch_rv_terrain_textures.py --check` → 0 退出；`cmake --build build/release` 通过。

```bash
git add tools/fetch_rv_terrain_textures.py projects/rv/assets/data/terrain_layers.json projects/rv/assets/textures/terrain engine/src/gfx/terrain_streamer.cpp
git commit -m "feat(rv): 2K CC0 terrain layer textures"
```

---

### Task 14: 坡度牵引

**Files:**
- Modify: `projects/rv/assets/scripts/lc/lc_terrain.lua`（新增 `slopeSpeedFactor`）
- Modify: `projects/rv/assets/scripts/lc/lc_drive.lua`（目标速度乘该系数）
- Modify: `projects/rv/assets/scripts/lc/lc_selftest.lua`

**Interfaces:**
- Consumes: `terrainSlope(x, z)`
- Produces: `slopeSpeedFactor(x, z)` → `1.0`（≤25°）、线性降到 `0.55`（35°）、降到 `0`（≥45°）

- [ ] **Step 1: 写 selftest（失败）**

```lua
local function selftestSlopeTraction()
  -- 用解析高度构造三个坡度：0、30、50 度。
  local function factorForDeg(deg)
    local t = math.tan(math.rad(deg))
    -- 直接测公式，不依赖地形采样。
    return slopeFactorFromGradient(t)
  end
  local ok = factorForDeg(0) == 1.0
    and factorForDeg(30) < 0.75 and factorForDeg(30) > 0.55
    and factorForDeg(50) == 0.0
  return ok, "slope traction"
end
```

- [ ] **Step 2: 运行确认失败**

Run selftest（同 Task 10）→ 预期 `slopeFactorFromGradient` 未定义。

- [ ] **Step 3: 实现**

`slopeFactorFromGradient(g)`：`deg = atan(g)`；`<=25` → 1；`25..35` → `1 - 0.45 * t`；`35..45` → `0.55 * (1 - t)`；`>45` → 0。`slopeSpeedFactor(x,z)` 用 `terrainSlope` 求梯度模长后调用它。`lc_drive` 里把油门目标速度乘上该系数；倒车同样生效。

- [ ] **Step 4: 运行 selftest 通过并提交**

Run selftest → `slope traction` PASS；开上雪山时速度明显下降且不会垂直爬壁。

```bash
git add projects/rv/assets/scripts/lc/lc_terrain.lua projects/rv/assets/scripts/lc/lc_drive.lua projects/rv/assets/scripts/lc/lc_selftest.lua
git commit -m "feat(rv): slope traction on steep terrain"
```

---

### Task 15: P1 验收

**Files:**
- Modify: `docs/visual_acceptance.md`（追加 P1 章节）
- Create: `docs/visual_acceptance/p1_*.png`（截图证据，按仓库现有截图习惯放）

- [ ] **Step 1: 连续性与群系验收**

```bat
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_camp.png 60
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_forest.png 300
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --screenshot shots\p1_snow.png 900
```

Expected：三张图地形/植被/雾色明显不同；雪原能看到雪线；无接缝、无空洞。

- [ ] **Step 2: 快速移动 LOD 检查**

用 autopilot 跑一圈，记录：
```bat
echo autopilot > projects\rv\assets\scripts\autopilot_on.txt
build\release\neon_game.exe --scene projects\rv\assets\scenes\garage.json --smoke-test 1200
```
Expected：日志里 chunk 驻留数稳定、无每帧重建；截图无 LOD 跳变。

- [ ] **Step 3: 回归**

Run: `build/release/neon_tests.exe`、Lua selftest、`--smoke-test`。
Expected：全绿；`lc_selftest` 的 `pois grounded` / `terrain world` PASS；老档能载入并重新贴地。

- [ ] **Step 4: 记录并提交**

把三张截图、帧时间、驻留 chunk 数写进 `docs/visual_acceptance.md`。

```bash
git add docs/visual_acceptance.md docs/visual_acceptance
git commit -m "test(rv): P1 biome terrain acceptance"
```
