#pragma once

// C1: 渲染编排子系统（Task 16）：GameRuntime 绘制子系统的独立类。拥有 draw list
// （draws_/drawKeys_/drawBatches_/batchModels_/drawBvh_/bvhVisible_/drawOrder_）+
// 植被缓存（vegCache_），实现 BuildDrawList/SyncDrawKeys/ResolveDrawItem/
// ResolveOrSkip/ResolveMeshKey/DrawVegetation/VegetationMesh 以及整个 Draw 方法体
// （BeginFrame 后处理 + 场景相机/光照 + BuildDrawList + HUD anchors + 实例批处理 +
// BVH 剔除 + 粒子/投射物/植被 + 2D canvas on_render）。纯机械拆分自 GameRuntime
// （Task 16），不改变任何绘制行为。
//
// 共享状态边界：ecs::World（GameRuntime 拥有）经参数传入；AssetManager/
// asyncMeshLoad/FullAssetPath 经 Configure() 注入（cfg_.assets / assetBaseDir /
// variantTable 在 Start 时刷新）；已拆系统（HudSystem/SceneTreeSystem/
// AnimationSystem/ProjectileSystem/SceneParticleSystem/ScriptCanvas）+ scriptCtx_/
// hosts_/hiddenEntities_ 经 Draw 参数传入。GameRuntime 保留公开转发
// （Draw/DrawCount/MeshForEntity/SetPostFx）与公开脚本/渲染 API。

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/gfx/material.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/math/bvh.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::assets {
class AssetManager;
}
namespace neon::gfx {
struct Camera;
class Renderer;
}
namespace neon::script {
struct ScriptContext;
class IScriptHost;
}

namespace neon::scene {

struct SkinnedModel;
class AnimationSystem;
class HudSystem;
class ProjectileSystem;
class SceneParticleSystem;
class SceneTreeSystem;
class ScriptCanvas;

class DrawSystem {
public:
    // 渲染参数：编辑器整体缩放（previewZoom，除以场景相机 orthoSize）与后处理 FX
    // 覆盖（原 SetPostFx 存储；在 Draw 开头应用到 renderer）。
    struct DrawParams {
        float previewZoom = 1.0f;
        bool ssao = false;
        bool volumetric = false;
        bool ssr = false;
        float ssaoIntensity = 1.0f;
        float volumetricIntensity = 1.0f;
        float ssrIntensity = 0.8f;
    };

    // 运行时持有的共享状态（Start 时由 GameRuntime 注入；ChangeScene 复用时刷新）。
    struct Content {
        assets::AssetManager* assets = nullptr; // 空 = sim-only，Draw 是 no-op
        bool asyncMeshLoad = false;             // G6-2 异步网格流式加载
        // FullAssetPath：解析资产引用（obj:/gltf:/texture 路径）。GameRuntime 用
        // cfg_.assetBaseDir + variantTable 提供。
        std::function<std::string(const std::string&)> fullAssetPath;
    };

    void Configure(Content content);
    // Out-of-line: DrawItem holds a std::shared_ptr<SkinnedModel> (forward-
    // declared), so the vector<DrawItem>/shared_ptr destruction must happen
    // where SkinnedModel is complete (draw_system.cpp).
    ~DrawSystem();

    // BuildDrawList：把带渲染组件的实体收集成 draw items（mesh/sprite/tilemap/
    // decal/terrain chunk），并维护动画状态（Prune/SyncOverride）。Start 与 Draw
    // 每帧调用；headless（无 assets）时由调用方跳过。原 Start 的无 renderer 调用
    // 即此签名（BuildDrawList 本身不触碰 renderer）。
    void Build(ecs::World& world, AnimationSystem& anims);
    // 解析通道：对每个 draw item 执行 ResolveOrSkip（异步未就绪的项跳过，下帧
    // 重试）。Draw 内部的 BVH 与绘制循环为保持原调用时序逐项内联 ResolveOrSkip，
    // 本方法是整列表的一次性解析入口（test/host 用）。
    void Resolve(ecs::World& world, gfx::Renderer& renderer, AnimationSystem& anims);

    // 整个 Draw 方法体（BeginFrame + 场景 3D + 批处理 + 粒子 + HUD anchors +
    // ProjectileSystem::Draw + 植被 + 2D canvas on_render）。已拆系统经引用传入；
    // uiScale/uiOffset 输出末尾快照的 2D 设计映射（GameRuntime 的 DrawUI/Tick 的
    // screenToUi 继续读它）。
    void Draw(gfx::Renderer& renderer, const gfx::Camera& camera, const DrawParams& params,
              ecs::World& world, script::ScriptContext& scriptCtx,
              script::IScriptHost* luaHost, script::IScriptHost* jsHost,
              const std::set<uint64_t>& hiddenEntities,
              HudSystem& hud, SceneTreeSystem& sceneTree, AnimationSystem& anims,
              ProjectileSystem& projectiles, SceneParticleSystem& particles,
              ScriptCanvas& canvas, float& uiScale, math::Vec2& uiOffset);

    // 原 GameRuntime::MeshForEntity（test/debug 观测）：实体已解析 draw item 在
    // 给定相机距离下的 LOD 网格。实体无 draw item / 未解析 / 失败时返回无效网格。
    gfx::Mesh MeshForEntity(const ecs::Entity& ent, const gfx::Camera& camera,
                            const ecs::World& world) const;
    // 脚本 hook（setSprite/setSpriteSheet）对 draw item 的应用：重置 sprite 帧
    // 时钟并标记重解析（SceneSprite 组件本体由 GameRuntime 更新）。原 AttachScripts
    // 里的 draws_ 更新段。
    void SetSpriteFrames(ecs::Entity e, const std::vector<std::string>& frames, float fps);
    void SetSpriteSheet(ecs::Entity e, const std::string& sheet, int count, float fps);
    // Tick 里的序列帧 sprite 动画推进（原 GameRuntime::Tick 尾部；固定步进 dt）。
    void AdvanceSprites(float dt);
    // PlayAnimation 的 skinned 检查：实体是否有已解析的 skinned draw item（原
    // draws_ 线性扫描）。
    bool HasSkinned(ecs::Entity e) const;
    // Stop 生命周期（原 draws_.clear()；vegetation 缓存保持原样，由每帧
    // DrawVegetation 的存活检查清理）。
    void Clear();
    size_t DrawCount() const { return draws_.size(); }

private:
    // 一个 draw item：实体渲染引用（动画实例状态在 AnimationSystem，Task 12）。
    struct DrawItem {
        ecs::Entity ent;
        std::string meshKey;
        // 2D sprite: texture path + flips. When isSprite is true the item
        // draws an XY quad with an unlit texture material instead of a mesh.
        bool isSprite = false;
        // Billboard mode: the quad is re-oriented every frame to face the
        // camera (world-space glow particles / VFX in 3D scenes).
        bool billboard = false;
        std::string spriteTex;
        bool flipX = false;
        bool flipY = false;
        // Sequence-frame sprite animation: frames/fps/loop copied from the
        // SceneSprite component; Draw advances the clock and swaps spriteTex.
        std::vector<std::string> spriteFrames;
        float spriteFps = 0.0f;
        bool spriteLoop = true;
        float spriteAnimTime = 0.0f;
        int spriteFrame = -1;
        // Spritesheet variant: one horizontal atlas texture, `sheetFrames`
        // equal sub-rects; the quad's UV window is rebuilt per frame.
        std::string sheetTex;
        int sheetFrames = 0;
        math::Vec3 tileOffset{};  // P1-1: per-cell offset for tilemap quads
        // P2-1 ground decal: draws a flat XZ-plane quad with the texture.
        bool isDecal = false;
        float decalSize = 2.0f;
        // LOD chain spec from the entity's SceneMesh (data-driven: distance +
        // meshKey per level). Resolved into `chain` during ResolveDrawItem.
        std::vector<LodEntry> lod;
        // G2-3 chunked-LOD terrain: this item is one patch of a grid-split
        // terrain. The mesh carries its own local position (verts already span
        // the patch), so the entity transform places it; `chunkCenterLocal` is
        // used only for camera-distance LOD selection (per-patch, not per
        // entity). Off for ordinary draw items.
        bool isTerrainChunk = false;
        int chunkGridX = 0;
        int chunkGridZ = 0;
        int chunkGridDiv = 0;
        math::Vec3 chunkCenterLocal{0.0f, 0.0f, 0.0f};
        gfx::Mesh mesh;
        gfx::LodChain chain; // resolved levels+thresholds; empty = single mesh
        gfx::Material mat;
        // Animated skinned glTF (meshKey "gltf:...") resolved once; when set,
        // drawing uses the skinned parts + bone matrices instead of `mesh`.
        // The per-entity ANIMATION state (override clip / ASM / pose clock)
        // lives in AnimationSystem (entityKey -> State, registered at
        // ResolveDrawItem), NOT here - DrawItem is a render reference only, so
        // headless hosts (no draw items) carry no idle animation state (C2).
        std::shared_ptr<SkinnedModel> skinned;
        // 多 mesh glTF 场景（如 Sponza）：主 mesh 是 nodes[0]，这里是第 2+ 个
        // mesh 节点（自带累积变换 + 材质）。Draw 时用 itemModel * sub.transform
        // 绘制，使一个 `gltf:` 实体渲染整个场景（大型场景渲染，C15 延伸）。
        std::vector<assets::GltfMeshNode> gltfSubNodes;
        // 多 mesh glTF 场景的合并 AABB（主 mesh + 全部子节点），用于视锥剔除。
        // 若只按 nodes[0] 的 bounds 剔除，相机看向宫殿中心（nodes[0] 是边缘小
        // 块）时整个 item 被错误剔除，导致 Sponza 不可见（draws=1）。
        math::AABB gltfBounds;
        bool hasGltfBounds = false;
        bool resolved = false;
        bool failed = false;
        bool asyncPending = false;   // G6-2: mesh load kicked, waiting on cache
    };
    // Snapshot of an instanced-batching run (opaque static meshes with the same
    // mesh + material group into one instanced draw call).
    struct DrawBatch {
        gfx::Mesh mesh;
        gfx::Material mat;
        uint32_t start = 0;
        uint32_t count = 0;
    };
    // G2-3 vegetation field attached to a terrain entity: deterministic scatter
    // positions plus lazily-resolved plant + impostor meshes. Built once per
    // entity per Start (terrain heights are static during play).
    struct VegField {
        ecs::Entity ent;
        std::vector<math::Vec3> positions;
        gfx::Mesh mesh;     // full plant/bush/rock mesh (near instances)
        gfx::Mesh impostor; // billboard card (far instances)
        gfx::Material mat;
        gfx::Material impostorMat;
        float size = 1.0f;
        float impostorDistance = 60.0f;
        bool built = false;
        bool failed = false;
    };

    // B6: rebuild drawKeys_ from draws_ (called at the end of BuildDrawList).
    void SyncDrawKeys();
    // G6-2: async-aware item resolution — retries an asyncPending item from the
    // cache when its mesh is ready, else skips it; non-async items resolve
    // synchronously. Called from the draw passes.
    void ResolveOrSkip(DrawItem& item, gfx::Renderer& renderer, ecs::World& world,
                       AnimationSystem& anims);
    // Resolves one item: sprite/decal quads, terrain-chunk LodChain, meshKey
    // through ResolveMeshKey, skinned model + animation state registration.
    void ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer, ecs::World& world,
                         AnimationSystem& anims);
    // Resolves one meshKey ("obj:"/"gltf:" file-backed or a procedural
    // primitive) through the runtime's AssetManager; invalid mesh on failure.
    gfx::Mesh ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
                             const SceneTerrain* terrain = nullptr);
    // Renders every terrain entity's vegetation field: instanced plant meshes
    // for near instances plus yaw-billboard impostors past the swap distance.
    void DrawVegetation(gfx::Renderer& renderer, const gfx::Camera& camera,
                        ecs::World& world, SceneTreeSystem& sceneTree);
    // Resolves a vegetation meshKey ("tree"/"bush"/"rock"/obj:/gltf:) to a mesh.
    gfx::Mesh VegetationMesh(gfx::Renderer& renderer, const std::string& meshKey);

    Content content_;
    std::vector<DrawItem> draws_;
    // B6: alive-entity index over draws_ (EntityKey -> tracked).
    std::unordered_set<uint64_t> drawKeys_;
    // Instanced-batching scratch for opaque static meshes (per-frame reuse).
    std::vector<DrawBatch> drawBatches_;
    std::vector<math::Mat4> batchModels_;
    // G1-2 spatial index: per-frame BVH over batchable draw items, used to
    // pre-cull the camera frustum before instanced draws (id = draw index).
    math::Bvh drawBvh_;
    std::vector<uint8_t> bvhVisible_;
    // Sprite sort scratch (reused instead of a fresh allocation every frame).
    std::vector<size_t> drawOrder_;
    // G2-3 vegetation cache (EntityKey -> VegField); rebuilt lazily per Start.
    std::unordered_map<uint64_t, VegField> vegCache_;
    // 写实天空贴图缓存（SceneLight.skyTexture 配置）：按路径懒加载一次。
    gfx::Texture skyTex_;
    std::string skyTexPath_;
};

} // namespace neon::scene
