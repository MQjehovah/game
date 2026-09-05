// C1: DrawSystem（Task 16）：GameRuntime 绘制子系统的独立实现。BuildDrawList /
// SyncDrawKeys / ResolveOrSkip / ResolveDrawItem / ResolveMeshKey / VegetationMesh /
// DrawVegetation 从原 game_runtime_draw.cpp 迁入；整个 Draw 方法体从
// game_runtime.cpp 迁入。纯机械拆分：方法体逐行保留，仅把
// world_/cfg_/FullAssetPath/animations_/hud_/sceneTree_/scriptCtx_/hosts_/
// hiddenEntities_/uiScale_/uiOffset_/postFx 等 GameRuntime 状态换成参数/注入。
#include "neon/scene/systems/draw_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "neon/assets/asset_manager.hpp"
#include "neon/assets/mesh_format.hpp"
#include "neon/core/log.hpp"
#include "neon/core/profiler.hpp"
#include "neon/core/result.hpp"
#include "neon/core/rng.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/gfx/terrain.hpp"
#include "neon/scene/render_stack.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/skinned_model.hpp"
#include "neon/scene/systems/animation_system.hpp"
#include "neon/scene/systems/hud_system.hpp"
#include "neon/scene/systems/projectile_system.hpp"
#include "neon/scene/systems/scene_particle_system.hpp"
#include "neon/scene/systems/scene_tree_system.hpp"
#include "neon/scene/systems/script_canvas.hpp"
#include "neon/script/bindings.hpp"
#include "../game_runtime_priv.hpp"

namespace neon::scene {
using namespace detail; // SameMaterial / SelectLodMesh / EntityKey

void DrawSystem::Configure(Content content) { content_ = std::move(content); }

DrawSystem::~DrawSystem() = default;

void DrawSystem::Build(ecs::World& world, AnimationSystem& anims) {
    // Synchronize with the live entity set: scripts can Spawn/Despawn entities
    // (SpawnSprite, Despawn) while running, so drop dead draws and append new
    // mesh/sprite entities each call while keeping resolved items cached.
    draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                [&](const DrawItem& d) { return !world.Alive(d.ent); }),
                 draws_.end());
    // B6: rebuild the alive-entity index once (draws_ shrinks above); the
    // contains() checks below become O(1) lookups instead of O(N) scans.
    drawKeys_.clear();
    for (const DrawItem& d : draws_) drawKeys_.insert(EntityKey(d.ent));
    // Task 12: per-entity animation state lives in AnimationSystem (not the
    // draw items). Drop the state+binding of every entity whose draw item is
    // gone - the binding points at the item's SkinnedModel, which is freed
    // with the item above (a stale binding would dangle on the next Tick).
    anims.Prune([this](uint64_t key) { return drawKeys_.count(key) != 0; });
    // M1: mirror each entity's SceneAnimOverride component into its animation
    // state (clip-name changes re-resolve; a deactivated override is dropped).
    // Entities without a registered state (unresolved skinned items) no-op;
    // ResolveDrawItem seeds their state from the component when it resolves.
    for (const DrawItem& d : draws_) {
        const SceneAnimOverride* ov = world.Get<SceneAnimOverride>(d.ent);
        if (!ov || !ov->active) {
            anims.SyncOverride(EntityKey(d.ent), {});
            continue;
        }
        anims.SyncOverride(EntityKey(d.ent),
                           {ov->clip, ov->loop, ov->speed, ov->crossFade, true});
    }
    auto contains = [this](ecs::Entity e) { return drawKeys_.count(EntityKey(e)) != 0; };
    auto view = world.ViewAll<SceneMesh>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneMesh>(i);
        if (contains(ent)) continue; // already tracked (resolved state kept)
        const SceneMesh* m = world.Get<SceneMesh>(ent);
        const SceneTransform* t = world.Get<SceneTransform>(ent);
        if (!m || !t) continue; // a mesh without a transform draws nothing
        // G2-3 chunked-LOD terrain: a terrain entity with chunkGridDiv > 0
        // becomes gridDiv x gridDiv patch draw items (each with its own LodChain
        // + per-patch distance LOD), instead of one monolithic terrain mesh.
        const SceneTerrain* terr = world.Get<SceneTerrain>(ent);
        if (m->meshKey == "terrain" && terr && terr->chunkGridDiv > 0 &&
            !terr->heights.empty()) {
            const int gd = math::IClamp(terr->chunkGridDiv, 1, 16);
            const float half = terr->size * 0.5f;
            const float chunkSize = terr->size / static_cast<float>(gd);
            for (int gz = 0; gz < gd; ++gz) {
                for (int gx = 0; gx < gd; ++gx) {
                    DrawItem c;
                    c.ent = ent;
                    c.meshKey = "terrain";
                    c.isTerrainChunk = true;
                    c.chunkGridX = gx;
                    c.chunkGridZ = gz;
                    c.chunkGridDiv = gd;
                    c.chunkCenterLocal = {-half + (gx + 0.5f) * chunkSize, 0.0f,
                                          -half + (gz + 0.5f) * chunkSize};
                    c.mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
                    c.mat.doubleSided = true; // hide the LOD-crack skirt
                    // G4: overlay a material's albedo texture on the terrain.
                    // The terrain mesh carves its UV in world units and bakes
                    // grass/dirt/rock vertex colors; sampling the scene's albedoTex
                    // and multiplying (albedo *= uTint * vColor) layers the
                    // realistic texture on top of the layer-blended tint.
                    if (!m->albedoTex.empty()) {
                        assets::TextureLoadOptions opts;
                        opts.wrap = gfx::Wrap::Repeat; // terrain UV spans > [0,1]
                        c.mat.albedo =
                            content_.assets
                                ->LoadTexture(content_.fullAssetPath(m->albedoTex), opts)
                                .Handle();
                        c.mat.uvRepeat = std::max(m->uvRepeat, 1.0f);
                    }
                    draws_.push_back(std::move(c));
                }
            }
            continue;
        }
        DrawItem item;
        item.ent = ent;
        item.meshKey = m->meshKey;
        item.lod = m->lod; // data-driven LOD chain spec; resolved at Draw time
        // Base material. A "gltf:" entity inherits the glTF node's baked PBR
        // material (albedo/metal-roughness/AO/emissive textures) so playtest
        // matches what the editor shows; otherwise start from a plain Lit
        // material. The entity's own color/metallic/roughness/texture fields
        // are then applied on top (mirroring EditorApp::ApplyMaterialParams),
        // so explicit material edits still win over the file's defaults.
        const bool gltfBase = m->meshKey.compare(0, 5, "gltf:") == 0 && content_.assets;
        if (gltfBase) {
            assets::GltfAsset gltf =
                content_.assets->LoadGLTF(content_.fullAssetPath(m->meshKey.substr(5)));
            if (!gltf.nodes.empty())
                item.mat = gltf.nodes[0].material;
            else
                item.mat = gfx::Material::Lit({}, ParseColorHex(m->colorHex), 24.0f);
        } else {
            item.mat = gfx::Material::Lit({}, ParseColorHex(m->colorHex), 24.0f);
        }
        // Props that bake colors into vertex data keep a white material tint so
        // the baked colors show through (mirrors EditorApp::ApplyMaterialParams).
        const bool bakedColor = m->meshKey == "terrain" || m->meshKey == "tree" ||
                                m->meshKey == "house" || m->meshKey == "bush" ||
                                m->meshKey == "hero" || m->meshKey == "wolf" ||
                                m->meshKey == "npc" || m->meshKey.compare(0, 4, "npc:") == 0;
        item.mat.tint = bakedColor ? gfx::Color::White : ParseColorHex(m->colorHex);
        item.mat.metallic = m->metallic;
        item.mat.roughness = m->roughness;
        item.mat.uvRepeat = m->uvRepeat;
        item.mat.aoStrength = m->ao;
        item.mat.emissiveIntensity = m->emissiveIntensity;
        item.mat.castShadow = m->castShadow;
        if (content_.assets) {
            // UV tiling: when uvRepeat > 1 the sampler must use REPEAT, else
            // clamp pulls edge pixels and the tiling collapses into streaks.
            assets::TextureLoadOptions opts;
            if (m->uvRepeat > 1.01f) opts.wrap = gfx::Wrap::Repeat;
            if (!m->albedoTex.empty())
                item.mat.albedo =
                    content_.assets->LoadTexture(content_.fullAssetPath(m->albedoTex), opts)
                        .Handle();
            if (!m->mrTex.empty())
                item.mat.metallicRoughness =
                    content_.assets->LoadTexture(content_.fullAssetPath(m->mrTex), opts)
                        .Handle();
            if (!m->aoTex.empty())
                item.mat.occlusion =
                    content_.assets->LoadTexture(content_.fullAssetPath(m->aoTex), opts)
                        .Handle();
            if (!m->emissiveTex.empty())
                item.mat.emissive =
                    content_.assets->LoadTexture(content_.fullAssetPath(m->emissiveTex), opts)
                        .Handle();
            // A2 normal map: uses the DEFAULT clamp wrap (no tiling) since a
            // normal map shouldn't repeat like the base texture. The lit shader
            // perturbs N in tangent space; normalScale is authored per material.
            if (!m->normalTex.empty())
                item.mat.normalMap =
                    content_.assets
                        ->LoadTexture(content_.fullAssetPath(m->normalTex),
                                      assets::TextureLoadOptions{})
                        .Handle();
            item.mat.normalScale = m->normalScale;
        }
        // M1: the animation override lives on the SceneAnimOverride component;
        // ResolveDrawItem seeds the entity's AnimationSystem state from it, so
        // a newly-tracked item needs no animation fields on the draw item.
        draws_.push_back(std::move(item));
    }
    auto spriteView = world.ViewAll<SceneSprite>();
    for (size_t i = 0; i < spriteView.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneSprite>(i);
        if (contains(ent)) continue;
        const SceneSprite* s = world.Get<SceneSprite>(ent);
        const SceneTransform* t = world.Get<SceneTransform>(ent);
        if (!s || !t) continue; // a sprite without a transform draws nothing
        DrawItem item;
        item.ent = ent;
        item.isSprite = true;
        item.billboard = s->billboard;
        item.spriteTex = s->texture;
        item.flipX = s->flipX;
        item.flipY = s->flipY;
        // Sequence-frame animation: keep the frame list on the draw item so
        // Draw can advance the clock and swap the texture each frame.
        item.spriteFrames = s->frames;
        item.spriteFps = s->fps;
        item.spriteLoop = s->loop;
        if (!s->frames.empty() && !s->frames[0].empty()) item.spriteTex = s->frames[0];
        // Spritesheet atlas wins over per-file frames (single texture).
        if (!s->sheet.empty()) {
            item.sheetTex = s->sheet;
            item.sheetFrames = s->sheetFrames;
            item.spriteTex = s->sheet;
            item.spriteFps = s->fps;
            item.spriteLoop = s->loop;
            item.spriteFrames.clear();
        }
        item.spriteFrame = -1;
        // 2D sprites are lit so the scene's ambient/sun/lights affect them.
        item.mat = gfx::Material::Lit({}, ParseColorHex(s->colorHex), 8.0f);
        draws_.push_back(std::move(item));
    }
    // P1-1 tilemap: every non-empty cell becomes a sprite draw item offset by
    // its cell position (the entity scale sets the cell size).
    auto tileView = world.ViewAll<SceneTilemap>();
    for (size_t i = 0; i < tileView.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneTilemap>(i);
        if (contains(ent)) continue;
        const SceneTilemap* tm = world.Get<SceneTilemap>(ent);
        if (!tm) continue;
        for (int r = 0; r < tm->rows; ++r) {
            for (int c = 0; c < tm->cols; ++c) {
                const std::string& tex = tm->tiles[static_cast<size_t>(r) * tm->cols + c];
                if (tex.empty()) continue;
                DrawItem item;
                item.ent = ent;
                item.isSprite = true;
                item.spriteTex = tex;
                item.mat = gfx::Material::Unlit({});
                item.tileOffset = {static_cast<float>(c) + 0.5f,
                                   static_cast<float>(r) + 0.5f, 0.0f};
                draws_.push_back(std::move(item));
            }
        }
    }
    // P2-1 ground decals: a flat textured quad on the XZ plane.
    auto decalView = world.ViewAll<SceneDecal>();
    for (size_t i = 0; i < decalView.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneDecal>(i);
        if (contains(ent)) continue;
        const SceneDecal* d = world.Get<SceneDecal>(ent);
        if (!d || d->texture.empty()) continue;
        DrawItem item;
        item.ent = ent;
        item.isDecal = true;
        item.spriteTex = d->texture;
        item.decalSize = d->size;
        item.mat = gfx::Material::Unlit({});
        item.mat.transparent = true;
        item.mat.tint = {1, 1, 1, d->alpha};
        draws_.push_back(std::move(item));
    }
    SyncDrawKeys();
}

// B6: re-sync the alive-entity index after every append site in BuildDrawList
// (keys repeat across terrain chunks/tiles; the set dedupes by construction).
void DrawSystem::SyncDrawKeys() {
    drawKeys_.clear();
    for (const DrawItem& d : draws_) drawKeys_.insert(EntityKey(d.ent));
}

void DrawSystem::Resolve(ecs::World& world, gfx::Renderer& renderer, AnimationSystem& anims) {
    for (DrawItem& item : draws_) ResolveOrSkip(item, renderer, world, anims);
}

void DrawSystem::ResolveOrSkip(DrawItem& item, gfx::Renderer& renderer, ecs::World& world,
                               AnimationSystem& anims) {
    if (item.resolved) return;
    if (item.asyncPending) {
        // Async mesh load still in flight: probe the cache and resolve from it
        // the frame it becomes ready; otherwise leave unresolved (skipped).
        if (content_.assets) {
            const std::string& key = item.meshKey;
            bool ready = true;
            if (key.compare(0, 4, "obj:") == 0)
                ready = content_.assets->HasMesh(content_.fullAssetPath(key.substr(4)));
            else if (key.compare(0, 5, "gltf:") == 0)
                ready = content_.assets->HasGLTF(content_.fullAssetPath(key.substr(5)));
            if (ready) {
                item.asyncPending = false;
                ResolveDrawItem(item, renderer, world, anims);
            }
        }
        return; // still loading -> skip this frame
    }
    ResolveDrawItem(item, renderer, world, anims);
}

void DrawSystem::ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer, ecs::World& world,
                                 AnimationSystem& anims) {
    if (item.resolved || item.failed || !content_.assets) return;

    if (item.isSprite) {
        gfx::Texture tex = content_.assets->LoadTexture(content_.fullAssetPath(item.spriteTex));
        if (!tex.Valid()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: sprite texture '%s' failed to load (skipped)",
                         item.spriteTex.c_str());
            item.failed = true;
            return;
        }
        // Spritesheet atlas: the quad samples the current frame's sub-rect.
        if (!item.sheetTex.empty() && item.sheetFrames > 0) {
            const float fw = 1.0f / static_cast<float>(item.sheetFrames);
            const int f = item.spriteFrame >= 0 ? item.spriteFrame : 0;
            const float u0 = fw * static_cast<float>(f);
            item.mesh = gfx::Mesh::CreateQuadUv(renderer, 1.0f, 1.0f, u0, 0.0f,
                                                u0 + fw, 1.0f, "sprite_sheet");
        } else {
            item.mesh = gfx::Mesh::CreateQuad(renderer, 1.0f, 1.0f, "sprite");
        }
        item.mat.albedo = tex.Handle();
        item.mat.transparent = true; // PNG sprites keep their alpha
        // flipX/flipY mirror the quad via a NEGATIVE local scale, which flips
        // the triangle winding; with back-face culling on, a flipped sprite
        // would be invisible. Sprite quads are flat billboards -- render both
        // sides so a flipped sprite stays visible.
        item.mat.doubleSided = true;
        item.resolved = true;
        return;
    }
    if (item.isDecal) {
        gfx::Texture tex = content_.assets->LoadTexture(content_.fullAssetPath(item.spriteTex));
        if (!tex.Valid()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: decal texture '%s' failed to load (skipped)",
                         item.spriteTex.c_str());
            item.failed = true;
            return;
        }
        item.mesh = gfx::Mesh::CreatePlane(renderer, item.decalSize, item.decalSize, 1, 1,
                                           "decal");
        item.mat.albedo = tex.Handle();
        item.resolved = true;
        return;
    }

    // G2-3 chunked-LOD terrain patch: build that chunk's LodChain and remember
    // its local centre for per-patch distance LOD selection in Draw().
    if (item.isTerrainChunk) {
        const SceneTerrain* terr = world.Get<SceneTerrain>(item.ent);
        if (!terr || terr->heights.empty()) {
            item.failed = true;
            return;
        }
        gfx::TerrainChunkMesh chunk = gfx::BuildTerrainChunk(
            renderer, terr->heights, terr->segments, terr->size, terr->heightScale,
            item.chunkGridDiv, item.chunkGridX, item.chunkGridZ, terr->chunkLodLevels,
            terr->chunkBaseSubdiv);
        if (chunk.chain.levels.empty()) {
            item.failed = true;
            return;
        }
        item.mesh = chunk.chain.levels[0];
        item.chain = chunk.chain;
        item.mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
        item.mat.doubleSided = true; // hide the LOD-crack skirt
        // G4 splatmap terrain: select the terrain shader and seed the grass
        // texture (realistic) + dirt/rock colors (procedural). The vertex splat
        // weights (r=grass, g=dirt, b=rock) blend them in the fragment shader.
        item.mat.shader = renderer.TerrainShader();
        if (const SceneMesh* sm = world.Get<SceneMesh>(item.ent); sm) {
            if (!sm->albedoTex.empty()) {
                assets::TextureLoadOptions opts;
                opts.wrap = gfx::Wrap::Repeat; // terrain UV spans > [0,1]
                item.mat.grassTex =
                    content_.assets->LoadTexture(content_.fullAssetPath(sm->albedoTex), opts)
                        .Handle();
                item.mat.uvRepeat = std::max(sm->uvRepeat, 1.0f);
            }
            if (!sm->dirtColorHex.empty())
                item.mat.dirtColor = ParseColorHex(sm->dirtColorHex);
            if (!sm->rockColorHex.empty())
                item.mat.rockColor = ParseColorHex(sm->rockColorHex);
        }
        item.resolved = true;
        return;
    }

    const std::string& key = item.meshKey;
    // G6-2: async mesh streaming. When enabled, file-backed meshes (obj:/gltf:)
    // are loaded off the main thread and the item resolves from the cache the
    // frame it is ready (Draw retries via asyncPending). Until then the item is
    // skipped �?no per-draw hitch.
    if (content_.asyncMeshLoad && content_.assets &&
        (key.compare(0, 4, "obj:") == 0 || key.compare(0, 5, "gltf:") == 0)) {
        const bool isObj = key.compare(0, 4, "obj:") == 0;
        // Use the project-relative virtual path (assets/...), NOT a project-dir-
        // absolute one: glTF's external buffers/images resolve relative to the
        // .gltf via the VFS, and an absolute path is rejected by it.
        const std::string full = content_.fullAssetPath(isObj ? key.substr(4) : key.substr(5));
        const bool cached = isObj ? content_.assets->HasMesh(full) : content_.assets->HasGLTF(full);
        if (!cached) {
            auto noop = [](bool) {};
            if (isObj) content_.assets->LoadMeshOBJAsync(full, noop);
            else content_.assets->LoadGLTFAsync(full, noop);
            item.asyncPending = true;
            return;
        }
    }
    const SceneTerrain* terr = key == "terrain" ? world.Get<SceneTerrain>(item.ent) : nullptr;
    gfx::Mesh mesh = ResolveMeshKey(renderer, key, terr);
    if (!mesh.Valid()) {
        const bool knownPrefix =
            assets::MeshFormatRegistry::Instance().HasPrefix(key) || key == "cube" ||
            key == "sphere" || key == "plane" ||
                                 key == "terrain";
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                     knownPrefix ? "runtime: mesh '%s' failed to load (skipped)"
                                 : "runtime: meshKey '%s' has no known loader/procedural "
                                   "prefix (skipped)",
                     key.c_str());
        item.failed = true;
        return;
    }
    item.mesh = mesh;

    // 多 mesh glTF 场景（C15 延伸）：`gltf:` 实体的第 2+ 个 mesh 节点（自带
    // 累积变换 + 材质）作为子项存储，Draw 时整体渲染（Sponza 类建筑场景）。
    if (!mesh.Skinned() && key.compare(0, 5, "gltf:") == 0 && content_.assets) {
        assets::GltfAsset g = content_.assets->LoadGLTF(content_.fullAssetPath(key.substr(5)));
        if (g.nodes.size() > 1) {
            item.gltfSubNodes.assign(g.nodes.begin() + 1, g.nodes.end());
        }
        // 多 mesh 场景合并 AABB（主 mesh + 全部子节点的世界包围盒），供视锥
        // 剔除使用。单个 nodes[0] 的 bounds 只覆盖场景一角，用它剔除会把
        // 面向场景中心的相机错误剔除（Sponza draws=1 的根因）。
        item.hasGltfBounds = false;
        if (mesh.Valid()) {
            item.gltfBounds = mesh.Bounds();
            item.hasGltfBounds = true;
        }
        for (const assets::GltfMeshNode& sub : item.gltfSubNodes) {
            if (!sub.mesh.Valid()) continue;
            const math::AABB sb = TransformAABB(sub.mesh.Bounds(), sub.transform);
            if (!item.hasGltfBounds) {
                item.gltfBounds = sb;
                item.hasGltfBounds = true;
            } else {
                item.gltfBounds.Expand(sb.min);
                item.gltfBounds.Expand(sb.max);
            }
        }
    }

    // Animated skinned glTF: resolve the full model (all skinned mesh parts +
    // skeleton + clips) so Draw() can use bone matrices. LOD chains are not
    // supported for skinned models (the file's parts are the model).
    if (mesh.Skinned()) {
        core::Result<SkinnedModel> sm =
            LoadSkinnedModel(*content_.assets, content_.fullAssetPath(key.substr(5)));
        if (!sm.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: skinned model '%s' failed to resolve: %s", key.c_str(),
                         sm.Error().c_str());
            item.failed = true;
            return;
        }
        item.skinned = std::make_shared<SkinnedModel>(std::move(sm.Value()));
        // Task 12: register the entity's animation state in AnimationSystem
        // (entityKey -> AnimState). Seeded from the SceneAnimOverride component
        // so a freshly-resolved item plays its override from the first Tick.
        // The state lives OUTSIDE the draw item, so Draw and Tick share it via
        // the entity key while DrawItem stays a pure render reference (C2).
        {
            const SceneAnimOverride* ov = world.Get<SceneAnimOverride>(item.ent);
            AnimationSystem::OverrideSpec spec;
            if (ov && ov->active) {
                spec.clip = ov->clip;
                spec.loop = ov->loop;
                spec.speed = ov->speed;
                spec.crossFade = ov->crossFade;
                spec.active = true;
            }
            anims.InitState(EntityKey(item.ent),
                            AnimationSystem::ModelBinding{item.skinned.get()}, spec);
        }
        item.resolved = true;
        return;
    }

    // LOD chain: level 0 is the base mesh; each entry resolves into a lower-
    // detail level at its distance. A level that fails to load is logged and
    // dropped �?the chain degrades to the levels that resolved.
    if (!item.lod.empty()) {
        item.chain.levels.push_back(item.mesh);
        for (const LodEntry& e : item.lod) {
            gfx::Mesh levelMesh = ResolveMeshKey(renderer, e.meshKey);
            if (!levelMesh.Valid()) {
                NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn,
                             "runtime: LOD mesh '%s' failed to load (skipped; level dropped)",
                             e.meshKey.c_str());
                continue;
            }
            item.chain.levels.push_back(levelMesh);
            item.chain.thresholds.push_back(e.distance);
        }
    }
    item.resolved = true;
}

gfx::Mesh DrawSystem::ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
                                     const SceneTerrain* terrain) {
    gfx::Mesh mesh;
    // File-backed formats (obj/gltf/fbx/...) resolve through the mesh-format
    // registry, so adding a format only needs a Register() call. `renderer` is
    // passed to procedural primitives below.
    if (assets::MeshFormatRegistry::Instance().HasPrefix(key)) {
        // Apply the same path normalization + variant resolution the async
        // loader path uses, so "assets:/x" and variant-mapped mesh keys resolve
        // identically on the sync path.
        std::string path;
        const std::string prefix = assets::MeshFormatRegistry::Instance().MatchPrefix(key, &path);
        const std::string resolved = content_.fullAssetPath(path);
        assets::MeshLoadResult res =
            assets::MeshFormatRegistry::Instance().Load(*content_.assets, prefix + ":" + resolved);
        mesh = res.mesh;
    } else if (key == "cube") {
        mesh = gfx::Mesh::CreateCube(renderer, 1.0f, 1.0f, 1.0f, "cube");
    } else if (key == "sphere") {
        mesh = gfx::Mesh::CreateSphere(renderer, 1.0f, 16, 10, "sphere");
    } else if (key == "plane") {
        mesh = gfx::Mesh::CreatePlane(renderer, 10.0f, 10.0f, 4, 4, "plane");
    } else if (key == "terrain") {
        if (terrain && terrain->heights.size() ==
                           static_cast<size_t>(terrain->segments + 1) *
                               (terrain->segments + 1)) {
            mesh = gfx::Mesh::CreateTerrain(renderer, terrain->segments, terrain->size,
                                            terrain->heights, terrain->heightScale, "terrain");
        } else {
            mesh = gfx::MakeTerrainMesh(renderer);
        }
    } else if (key == "tree") {
        mesh = gfx::MakeTreeMesh(renderer);
    } else if (key == "house") {
        mesh = gfx::MakeHouseMesh(renderer);
    } else if (key == "bush") {
        mesh = gfx::MakeBushMesh(renderer);
    } else if (key == "grass") {
        mesh = gfx::MakeGrassMesh(renderer);
    } else if (key == "hero") {
        mesh = gfx::MakeHeroMesh(renderer);
    } else if (key == "wolf") {
        mesh = gfx::MakeWolfMesh(renderer);
    } else if (key.compare(0, 4, "npc:") == 0) {
        // "npc:r,g,b" encodes the villager's tunic tint (0-255 channels).
        int r = 128, g = 128, b = 128;
        std::sscanf(key.c_str() + 4, "%d,%d,%d", &r, &g, &b);
        mesh = gfx::MakeNPCMesh(renderer, {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f});
    } else if (key == "npc") {
        mesh = gfx::MakeNPCMesh(renderer, {0.5f, 0.5f, 0.6f, 1.0f});
    } else if (key == "rock") {
        mesh = gfx::Mesh::CreateSphere(renderer, 0.8f, 10, 7, "rock");
    } else if (key == "water") {
        mesh = gfx::Mesh::CreatePlane(renderer, 20.0f, 20.0f, 8, 8, "water");
    } else if (key == "road") {
        mesh = gfx::Mesh::CreatePlane(renderer, 1.0f, 1.0f, 1, 1, "road");
    }
    return mesh;
}

gfx::Mesh DrawSystem::VegetationMesh(gfx::Renderer& renderer, const std::string& meshKey) {
    return ResolveMeshKey(renderer, meshKey);
}

void DrawSystem::DrawVegetation(gfx::Renderer& renderer, const gfx::Camera& camera,
                                ecs::World& world, SceneTreeSystem& sceneTree) {
    if (!content_.assets) return;
    // Prune cache entries whose terrain entity has despawned since the last
    // frame (script Spawn/Despawn).
    for (auto it = vegCache_.begin(); it != vegCache_.end();) {
        if (!world.Alive(it->second.ent)) {
            it = vegCache_.erase(it);
        } else {
            ++it;
        }
    }

    auto view = world.ViewAll<SceneTerrain>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneTerrain>(i);
        const SceneTerrain* terr = world.Get<SceneTerrain>(ent);
        if (!terr || terr->vegMeshKey.empty() || terr->vegCount == 0 || terr->heights.empty())
            continue;

        const uint64_t key = EntityKey(ent);
        auto it = vegCache_.find(key);
        VegField* f = nullptr;
        if (it != vegCache_.end()) {
            f = &it->second;
        } else {
            f = &(vegCache_[key] = VegField{});
            f->ent = ent;
        }

        if (!f->built) {
            f->mesh = ResolveMeshKey(renderer, terr->vegMeshKey);
            f->size = std::max(terr->vegSize, 0.05f);
            f->impostorDistance = std::max(terr->vegImpostorDistance, 1.0f);
            f->mat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
            f->mat.doubleSided = true;
            f->impostorMat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
            f->impostorMat.doubleSided = true;
            if (f->mesh.Valid()) {
                // Size the billboard card from the plant mesh's bounds.
                const math::AABB& b = f->mesh.Bounds();
                float bw = std::max(b.max.x - b.min.x, b.max.z - b.min.z) * f->size;
                float bh = (b.max.y - b.min.y) * f->size;
                if (bw < 0.1f) bw = f->size;
                if (bh < 0.1f) bh = f->size * 2.0f;
                f->impostor = gfx::MakeImpostorQuad(renderer, bw, bh, {0.16f, 0.48f, 0.16f, 1.0f});
            }
            core::Rng rng(terr->vegSeed ? terr->vegSeed : 1u);
            gfx::VegetationConfig vcfg;
            vcfg.count = terr->vegCount;
            vcfg.minHeight = terr->vegMinHeight;
            vcfg.maxHeight = terr->vegMaxHeight;
            vcfg.maxSlope = terr->vegMaxSlope;
            vcfg.size = f->size;
            f->positions = gfx::ScatterVegetation(terr->heights, terr->segments, terr->size,
                                                  terr->heightScale, vcfg, rng);
            f->built = true;
        }
        if (f->failed || !f->mesh.Valid() || f->positions.empty()) continue;

        const math::Mat4 terrainModel = sceneTree.CachedLocalToWorld(ent);
        std::vector<math::Mat4> plantModels;
        std::vector<math::Mat4> impostorModels;
        plantModels.reserve(f->positions.size());
        impostorModels.reserve(f->positions.size() / 4 + 1);
        for (const math::Vec3& local : f->positions) {
            const math::Vec3 p = terrainModel.TransformPoint(local);
            const float dist = math::Distance(p, camera.position);
            if (dist > f->impostorDistance) {
                // Y-yaw billboard so the card faces the camera on the XZ plane.
                const float yaw = std::atan2(camera.position.x - p.x, camera.position.z - p.z);
                impostorModels.push_back(math::Mat4::Translation(p) *
                                         math::Quat::FromEuler(0.0f, yaw, 0.0f).ToMat4());
            } else {
                plantModels.push_back(math::Mat4::Translation(p) *
                                      math::Mat4::Scale({f->size, f->size, f->size}));
            }
        }
        if (!plantModels.empty())
            renderer.DrawMeshInstanced(f->mesh, f->mat, plantModels.data(),
                                       static_cast<uint32_t>(plantModels.size()));
        if (!impostorModels.empty())
            renderer.DrawMeshInstanced(f->impostor, f->impostorMat, impostorModels.data(),
                                       static_cast<uint32_t>(impostorModels.size()));
    }
}

void DrawSystem::Draw(gfx::Renderer& renderer, const gfx::Camera& camera, const DrawParams& params,
                      ecs::World& world, script::ScriptContext& scriptCtx,
                      script::IScriptHost* luaHost, script::IScriptHost* jsHost,
                      const std::set<uint64_t>& hiddenEntities,
                      HudSystem& hud, SceneTreeSystem& sceneTree, AnimationSystem& anims,
                      ProjectileSystem& projectiles, SceneParticleSystem& particles,
                      ScriptCanvas& canvas, float& uiScale, math::Vec2& uiOffset) {
    if (!content_.assets) return; // sim-only runtime draws nothing (running_ checked by caller)
    core::ScopedTimer drawTimer("runtime.draw");
    // Post-process FX overrides (mirrors the editor toggles so play matches).
    renderer.SetSsaoEnabled(params.ssao);
    renderer.SetSsaoIntensity(params.ssaoIntensity);
    renderer.SetVolumetricEnabled(params.volumetric);
    renderer.SetVolumetricIntensity(params.volumetricIntensity);
    renderer.SetSsrEnabled(params.ssr);
    renderer.SetSsrIntensity(params.ssrIntensity);
    // A/RenderStack: a scene can carry one scene-level RenderStack component that
    // overrides the FX toggles + bloom/tonemap/exposure/color-grade. This is the
    // runtime application point that was missing (SetBloomParams etc. existed but
    // nothing fed them) — now the data-driven stack actually drives the renderer.
    {
        const RenderStack* stack = nullptr;
        world.ViewAll<RenderStack>().ForEach(
            [&](ecs::Entity, const RenderStack& s) { if (!stack) stack = &s; });
        if (stack) {
            renderer.SetSsaoEnabled(stack->ssao);
            renderer.SetSsaoIntensity(stack->ssaoIntensity);
            renderer.SetVolumetricEnabled(stack->volumetric);
            renderer.SetVolumetricIntensity(stack->volumetricStrength);
            renderer.SetSsrEnabled(stack->ssr);
            renderer.SetSsrIntensity(stack->ssrStrength);
            renderer.SetBloomEnabled(stack->bloom);
            renderer.SetBloomParams(stack->bloomThreshold, stack->bloomStrength);
            renderer.SetTonemapEnabled(stack->tonemap);
            renderer.SetExposure(stack->exposure);
            // RenderStack fog is the density-based volumetric fog (composite
            // pass), not the lit shader's linear near/far fog (SceneLight owns
            // that). fogColor feeds the composite through FogColor().
            renderer.SetFog(stack->fogColor, 0.0f, 0.0f);
            renderer.SetVolumetricFogEnabled(stack->fog);
            renderer.SetVolumetricFogDensity(stack->fogDensity);
            renderer.SetColorGrade({stack->grade, stack->gradeSaturation, stack->gradeContrast,
                                    stack->gradeGain, stack->gradeGamma, stack->gradeLift,
                                    {stack->gradeTint.r, stack->gradeTint.g, stack->gradeTint.b}});
            renderer.SetAutoExposure({stack->autoExposure, stack->autoExposureKey, 0.05f, 20.0f,
                                      0.5f});
            renderer.SetVignette({stack->vignette, stack->vignetteRadius, 0.5f,
                                  stack->vignetteIntensity});
        }
    }
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    gfx::Camera cam = camera;
    bool usedCameraEntity = false;
    // Script-driven FPS game camera: while the "cameraMouseLock" GameVar is
    // truthy, the script owns the rendered view through cameraFocus (placed at
    // eye + viewDir * cameraDist by the controller) plus cameraYaw/cameraPitch/
    // cameraDist �?the same GameVars the host orbit cameras publish. This
    // overrides any scene Camera3D entity, so the runtime renders through the
    // player's eye in both the standalone player and the editor playtest.
    const script::Value fpsLock = scriptCtx.gameVars.Get("cameraMouseLock");
    const bool fpsCam = (fpsLock.type == script::Value::Type::Number &&
                         fpsLock.number != 0.0) ||
                        (fpsLock.type == script::Value::Type::Bool && fpsLock.boolean);
    if (fpsCam) {
        math::Vec3 focus;
        const script::Value focusVar = scriptCtx.gameVars.Get("cameraFocus");
        if (focusVar.type == script::Value::Type::Table && focusVar.table) {
            for (const auto& kv : focusVar.table->fields) {
                if (kv.second.type != script::Value::Type::Number) continue;
                if (kv.first == "x") focus.x = static_cast<float>(kv.second.number);
                else if (kv.first == "y") focus.y = static_cast<float>(kv.second.number);
                else if (kv.first == "z") focus.z = static_cast<float>(kv.second.number);
            }
        }
        float yaw = 0.0f, pitch = 0.0f, dist = 2.0f;
        const script::Value yawVar = scriptCtx.gameVars.Get("cameraYaw");
        if (yawVar.type == script::Value::Type::Number) yaw = static_cast<float>(yawVar.number);
        const script::Value pitchVar = scriptCtx.gameVars.Get("cameraPitch");
        if (pitchVar.type == script::Value::Type::Number)
            pitch = math::Clamp(static_cast<float>(pitchVar.number), -1.3f, 1.3f);
        const script::Value distVar = scriptCtx.gameVars.Get("cameraDist");
        if (distVar.type == script::Value::Type::Number && distVar.number > 0.0)
            dist = math::Clamp(static_cast<float>(distVar.number), 2.0f, 80.0f);
        math::Vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                          std::cos(yaw) * std::cos(pitch)};
        cam.position = focus + offset * dist;
        cam.target = focus;
        cam.up = {0, 1, 0};
        cam.ortho = false;
        usedCameraEntity = true;
    }
    // P2-3 scene camera: when the world contains a camera entity, its transform
    // + camera component become the active view (Godot Camera3D-style).
    world.ViewAll<SceneCamera, SceneTransform>().ForEach(
        [&](ecs::Entity, const SceneCamera& c, const SceneTransform& t) {
            if (usedCameraEntity) return;
            usedCameraEntity = true;
            cam.position = t.pos;
            cam.target = t.pos + t.rot.Rotate({0, 0, -1});
            cam.up = {0, 1, 0};
            cam.ortho = c.ortho;
            cam.fovY = c.fov * math::kDegToRad;
            cam.orthoSize = c.orthoSize > 0.0f ? c.orthoSize : 10.0f;
            // Editor whole-view zoom: shrink the ortho size so sprites grow
            // with the same factor the host applies to the 2D UI overlay.
            if (cam.ortho && params.previewZoom > 0.0f)
                cam.orthoSize /= params.previewZoom;
        });
    // The camera is FAITHFUL: the scene camera's orthoSize/fov is exactly
    // what the scene authored (no fit-outside overrides); the aspect follows
    // the active scene viewport, so the world fills the dock.
    const float drawAspect = renderer.SceneAspect();
    // Snapshot the resolved camera + viewport pixels: WorldToScreen/
    // ScreenToWorld and GetViewportSize answer script queries between renders
    // from this state. UI/world space is plain viewport PIXELS (no design
    // resolution - relative layout adapts, px stays px).
    const math::Rect2& sceneVp = renderer.SceneViewport();
    hud.CaptureView(cam, drawAspect,
                    sceneVp.w > 0.0f ? renderer.UIDesignSize().x : 1280.0f,
                    sceneVp.h > 0.0f ? renderer.UIDesignSize().y : 720.0f);
    // Project at the ACTIVE scene viewport's aspect (a dock sub-rect in the
    // editor, the full target in the standalone player) so the runtime render
    // matches whatever rasterization rect the host set up - otherwise the
    // playtest FOV would differ from the edit-mode viewport.
    renderer.SetCamera(cam, drawAspect);
    // G3: the editor may have already run the cascade shadow pass with its free
    // orbit camera this frame (before play resolved its game camera). Re-run it
    // for the RESOLVED camera so the shadow frusta track the game view, not the
    // editor's; otherwise orbiting the editor camera slides the shadows.
    renderer.RefreshShadowPass();
    // Data-driven scene environment: apply the scene's DirectionalLight +
    // AmbientLight objects (Unity-style) so every host renders the same scene
    // the same way (the editor's playtest and the standalone player both go
    // through Draw). Fog is pushed far for ortho/2D cameras so the flat sprites
    // are never tinted by a depth gradient.
    {
        const scene::SceneLight* directional = nullptr;
        const scene::SceneLight* ambient = nullptr;
        world.ViewAll<scene::SceneLight>().ForEach(
            [&](ecs::Entity, const scene::SceneLight& l) {
                if (l.type == "directional" && !directional) directional = &l;
                else if (l.type == "ambient" && !ambient) ambient = &l;
            });
        // 自定义氛围（useAtmosphere）：showcase 场景在 scene JSON 里用
        // skyTop/skyHorizon 渐变 + fogColor/fogNear/fogFar 表达整场色调
        // （黄昏/沙漠/雪地/极光夜）。未开时保持默认白天天空 + 灰蓝雾。
        // 写实天空贴图（SceneLight.skyTexture）：非空时 DrawSky 用 HDRI
        // 当全屏背景，且 IBL 优先从照片 CPU 采样真实天顶/地平色喂 SetSky
        // （物体受光跟随照片天空，而非手填色/默认渐变——HDRI 之前只当背景、
        //  光照仍偏蓝，是"贴图感/假"的最大来源）。无 skyTexture 时才用
        // useAtmosphere 手动渐变。
        if (content_.assets) {
            const std::string skyPath =
                directional && !directional->skyTexture.empty() ? directional->skyTexture
                                                                : std::string();
            if (!skyPath.empty() && skyPath != skyTexPath_) {
                skyTex_ = content_.assets->LoadTexture(content_.fullAssetPath(skyPath));
                skyTexPath_ = skyPath;
                skyIblFromTex_ = false;  // re-extract on texture change
            }
            if (skyTex_.Valid()) renderer.SetSkyTexture(skyTex_.Handle());
            if (!skyTexPath_.empty() && !skyIblFromTex_) {
                const assets::DecodedImage img = assets::DecodeImageFile(
                    content_.fullAssetPath(skyTexPath_), /*compressBc1=*/false);
                if (img.channels > 0 && img.width > 0 && img.height > 0) {
                    gfx::Color z, h;
                    gfx::ibl::SkyDominantColors(img.rgba.data(), img.width, img.height, z, h);
                    renderer.SetSky(z, h);
                    skyIblFromTex_ = true;
                }
            }
        }
        if (!skyIblFromTex_) {
            if (directional && directional->useAtmosphere) {
                renderer.SetSky(directional->skyTop, directional->skyHorizon);
            } else {
                renderer.SetSky({0.28f, 0.38f, 0.58f, 1.0f}, {0.55f, 0.65f, 0.8f, 1.0f});
            }
        }
        // 曝光：scene 显式设置时覆盖宿主默认（夜晚 >1 提亮，黄昏微调）。
        if (directional && directional->useAtmosphere && directional->exposure >= 0.0f)
            renderer.SetExposure(directional->exposure);
        if (cam.ortho) {
            renderer.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 1e9f, 1e10f);
        } else if (directional && directional->useAtmosphere) {
            renderer.SetFog(directional->fogColor, directional->fogNear, directional->fogFar);
        } else {
            renderer.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 60.0f, 220.0f);
        }
        if (directional) {
            const gfx::Color sun{directional->color.r * directional->intensity,
                                 directional->color.g * directional->intensity,
                                 directional->color.b * directional->intensity,
                                 directional->color.a};
            renderer.SetDirectionalLight(directional->sunDir, sun,
                                         directional->ambientStrength);
            // A4: a useAtmosphere scene can opt into the procedural view-ray
            // skybox (sun/moon/clouds), aimed from the directional-light sun dir.
            if (directional->useAtmosphere && directional->skybox)
                renderer.EnableSkyBox(directional->sunDir, /*clouds=*/true);
        } else {
            renderer.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {0.8f, 0.8f, 0.8f}, 0.0f);
        }
        if (ambient) renderer.SetAmbientLight(ambient->color, ambient->ambientStrength);
        // PointLight objects (Unity-style) drive the renderer's point lights, so
        // campfire/firefly glows authored in a scene show up in the standalone
        // player too (the editor viewport already fed them). Up to
        // kMaxPointLights, position from the entity's transform.
        int plIndex = 0;
        world.ViewAll<scene::SceneLight, SceneTransform>().ForEach(
            [&](ecs::Entity, const scene::SceneLight& pl, const SceneTransform& pt) {
                if (pl.type != "point") return;
                if (plIndex >= gfx::Renderer::kMaxPointLights) return;
                const gfx::Color pc{pl.color.r * pl.intensity, pl.color.g * pl.intensity,
                                    pl.color.b * pl.intensity, pl.color.a};
                renderer.SetPointLight(plIndex++, pt.pos, pc, pl.radius);
            });
        for (; plIndex < gfx::Renderer::kMaxPointLights; ++plIndex)
            renderer.SetPointLight(plIndex, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
        renderer.DrawSky();
    }
    // Scripts may have spawned/despawned sprite entities since the last frame.
    Build(world, anims);
    // G1-3: refresh the world-transform cache (parent-before-child, arbitrary
    // depth) once per frame; the BVH pass and the draw loop read it instead of
    // re-walking parent chains per entity.
    sceneTree.Rebuild(world);
    // M1 HUD anchors: project every drawn entity's world position (plus a
    // per-plate head offset) into design units for on_render scripts. Cached
    // once per frame; WorldToScreen() below uses the same matrices. The
    // world-position pass (plate AABB head offsets) stays here; HudSystem
    // projects the resulting entity+world pairs into screen anchors.
    {
        std::vector<std::pair<ecs::Entity, math::Vec3>> anchorEnts;
        anchorEnts.reserve(draws_.size());
        for (const DrawItem& d : draws_) {
            const math::Mat4 model = sceneTree.CachedLocalToWorld(d.ent);
            math::Vec3 wp{model.m[3], model.m[7], model.m[11]};
            // Head offset: lift the anchor above the model bounds when the
            // entity carries a plate (the script stamps names via
            // SetEntityPlate; default 0 keeps the raw position).
            auto pit = hud.EntityPlates().find(EntityKey(d.ent));
            if (pit != hud.EntityPlates().end() && pit->second.hpFrac >= 0.0f) {
                const SceneTransform* tr = world.Get<SceneTransform>(d.ent);
                if (tr) {
                    // Plate tracks the RENDERED mesh, which for a skinned rig
                    // can sit off the entity pivot (the wolf's bones place the
                    // body away from its origin). Compute the world AABB with
                    // the same transform chain Draw() uses �?model *
                    // part.localTransform * bone matrix �?and center the bar on
                    // it, just above the top.
                    math::AABB wb;
                    bool have = false;
                    auto expand = [&](const math::Vec3& p) {
                        if (!have) {
                            wb.min = wb.max = p;
                            have = true;
                        } else {
                            wb.min.x = std::fmin(wb.min.x, p.x);
                            wb.min.y = std::fmin(wb.min.y, p.y);
                            wb.min.z = std::fmin(wb.min.z, p.z);
                            wb.max.x = std::fmax(wb.max.x, p.x);
                            wb.max.y = std::fmax(wb.max.y, p.y);
                            wb.max.z = std::fmax(wb.max.z, p.z);
                        }
                    };
                    auto expandBox = [&](const math::AABB& lb, const math::Mat4& m) {
                        const math::Vec3 corners[8] = {
                            {lb.min.x, lb.min.y, lb.min.z}, {lb.max.x, lb.min.y, lb.min.z},
                            {lb.min.x, lb.max.y, lb.min.z}, {lb.max.x, lb.max.y, lb.min.z},
                            {lb.min.x, lb.min.y, lb.max.z}, {lb.max.x, lb.min.y, lb.max.z},
                            {lb.min.x, lb.max.y, lb.max.z}, {lb.max.x, lb.max.y, lb.max.z}};
                        for (const math::Vec3& c : corners) {
                            const math::Vec4 q = m.TransformVec4({c.x, c.y, c.z, 1.0f});
                            if (q.w != 0.0f) expand({q.x / q.w, q.y / q.w, q.z / q.w});
                        }
                    };
                    if (d.skinned && d.skinned->Valid()) {
                        // Mirror Draw()'s bone selection (override clip vs the
                        // model's default) so the anchor matches this frame.
                        // Task 12: the override pose now comes from the
                        // AnimationSystem state table (PoseFor); the default
                        // path falls back to the model's own BoneMatrices().
                        std::vector<math::Mat4> bones;
                        if (!anims.PoseFor(EntityKey(d.ent), d.skinned->skeleton, bones))
                            bones = d.skinned->BoneMatrices();
                        if (!bones.empty()) {
                            // CPU-skin the actual vertices so the plate hugs the
                            // RENDERED mesh (a rig can place the body off the
                            // entity pivot; box-corner transforms over all bones
                            // inflate the bounds, so exact per-vertex is safest).
                            for (const auto& part : d.skinned->parts) {
                                const std::vector<gfx::Vertex3D>& verts = part.mesh.CpuVerts();
                                if (verts.empty()) continue;
                                const math::Mat4 m = model * part.localTransform;
                                for (const gfx::Vertex3D& v : verts) {
                                    math::Vec4 sk{0.0f, 0.0f, 0.0f, 0.0f};
                                    for (int k = 0; k < 4; ++k) {
                                        if (v.w[k] <= 0.0f) continue;
                                        const int ji = static_cast<int>(v.j[k]);
                                        if (ji < 0 ||
                                            ji >= static_cast<int>(bones.size()))
                                            continue;
                                        const math::Vec4 q =
                                            bones[static_cast<size_t>(ji)].TransformVec4(
                                                {v.pos.x, v.pos.y, v.pos.z, 1.0f});
                                        sk.x += v.w[k] * q.x;
                                        sk.y += v.w[k] * q.y;
                                        sk.z += v.w[k] * q.z;
                                        sk.w += v.w[k] * q.w;
                                    }
                                    if (sk.w == 0.0f) continue;
                                    const math::Vec4 worldPt = m.TransformVec4(
                                        {sk.x / sk.w, sk.y / sk.w, sk.z / sk.w, 1.0f});
                                    if (worldPt.w != 0.0f)
                                        expand({worldPt.x / worldPt.w, worldPt.y / worldPt.w,
                                                worldPt.z / worldPt.w});
                                }
                            }
                        } else {
                            for (const auto& part : d.skinned->parts) {
                                const math::AABB lb = part.mesh.Bounds();
                                if (lb.max.y <= lb.min.y) continue;
                                expandBox(lb, model * part.localTransform);
                            }
                        }
                    } else if (d.mesh.Valid()) {
                        const math::AABB lb = d.mesh.Bounds();
                        if (lb.max.y > lb.min.y) expandBox(lb, model);
                    }
                    if (have) {
                        wp.x = (wb.min.x + wb.max.x) * 0.5f;
                        wp.z = (wb.min.z + wb.max.z) * 0.5f;
                        wp.y = wb.max.y + 0.2f * tr->scale.y;
                    } else {
                        wp.y = wp.y + 1.9f * tr->scale.y;
                    }
                }
            }
            anchorEnts.emplace_back(d.ent, wp);
        }
        hud.UpdateAnchors(anchorEnts);
    }
    // P2-3: sprites render back-to-front by their sortOrder component (2D
    // games); 3D depth-tested meshes are unaffected by the stable order.
    drawOrder_.resize(draws_.size());
    for (size_t i = 0; i < drawOrder_.size(); ++i) drawOrder_[i] = i;
    std::stable_sort(drawOrder_.begin(), drawOrder_.end(), [&](size_t a, size_t b) {
        const SceneSortOrder* sa = world.Get<SceneSortOrder>(draws_[a].ent);
        const SceneSortOrder* sb = world.Get<SceneSortOrder>(draws_[b].ent);
        return (sa ? sa->z : 0.0f) < (sb ? sb->z : 0.0f);
    });
    // Re-run the cascade shadow pass NOW: the earlier SetCamera/RefreshShadowPass
    // ran BEFORE Build recorded this frame's casters, so the shadow maps were
    // rendered empty and every receiver sampled factor=1 (no tree shadows on
    // the ground in play). After Build the casters exist; re-render the maps so
    // the main draw loop below samples this frame's real occluders.
    renderer.RefreshShadowPass();
    // Instanced batching: opaque static meshes with the same mesh + material
    // group into one instanced draw call. Only when the depth buffer works -
    // the no-depth fallback relies on painter's order, which batching would
    // change. Flush whenever a non-batchable item interrupts the run so the
    // relative order of opaque vs transparent/skinned draws never changes.
    const bool canBatch = renderer.DepthTestAvailable();
    // G1-2: build a per-frame BVH of batchable items and pre-cull the camera
    // frustum, so instanced draws only receive visible instances (the
    // renderer then skips its own per-instance test). Uses the renderer's own
    // Frustum::Intersects test, so the visible set is identical to before.
    drawBvh_.Clear();
    bvhVisible_.assign(draws_.size(), 0);
    if (canBatch) {
        for (size_t idx : drawOrder_) {
            DrawItem& item = draws_[idx];
            if (!world.Alive(item.ent)) continue;
            if (hiddenEntities.count(EntityKey(item.ent)) != 0) continue;
            ResolveOrSkip(item, renderer, world, anims);
            if (!item.resolved || item.failed) continue;
            if (!world.Get<SceneTransform>(item.ent)) continue;
            if (item.skinned || item.isSprite || item.isDecal || item.mat.transparent ||
                item.mat.shader.Valid() || !item.mesh.Valid())
                continue;
            const math::Mat4 model = sceneTree.CachedLocalToWorld(item.ent);
            // Column-vector convention: the translation is the last COLUMN of
            // the row-major matrix (m[3], m[7], m[11]), not the last row.
            // G2-3: a terrain chunk uses its patch centre (not the terrain
            // origin) so distance LOD is chosen per patch.
            const math::Vec3 worldPos = item.isTerrainChunk
                                            ? model.TransformPoint(item.chunkCenterLocal)
                                            : math::Vec3{model.m[3], model.m[7], model.m[11]};
            const gfx::Mesh drawMesh =
                SelectLodMesh(item.mesh, item.chain, worldPos, cam.position);
            if (!drawMesh.Valid()) continue;
            drawBvh_.Insert(static_cast<math::Bvh::Id>(idx),
                            math::TransformAABB(item.hasGltfBounds ? item.gltfBounds
                                                                   : drawMesh.Bounds(),
                                                model));
        }
        if (!drawBvh_.Empty())
            drawBvh_.QueryFrustum(renderer.ViewFrustum(),
                                  [&](math::Bvh::Id id) { bvhVisible_[id] = 1; });
    }
    drawBatches_.clear();
    batchModels_.clear();
    auto flushBatches = [&]() {
        if (drawBatches_.empty()) return;
        for (const DrawBatch& b : drawBatches_) {
            if (b.count == 0) continue;
            renderer.DrawMeshInstanced(b.mesh, b.mat, batchModels_.data() + b.start, b.count,
                                       /*frustumCull=*/false);
        }
        drawBatches_.clear();
        batchModels_.clear();
    };
    size_t dead = 0;
    for (size_t idx : drawOrder_) {
        DrawItem& item = draws_[idx];
        if (!world.Alive(item.ent)) {
            ++dead; // scripts can Despawn entities mid-playtest
            continue;
        }
        if (hiddenEntities.count(EntityKey(item.ent)) != 0) continue; // SetVisible(false)
        ResolveOrSkip(item, renderer, world, anims);
        if (!item.resolved || item.failed) continue;
        if (!world.Get<SceneTransform>(item.ent)) continue;
        math::Mat4 model = sceneTree.CachedLocalToWorld(item.ent);
        if (item.tileOffset.LengthSq() > 0.0f)
            model = model * math::Mat4::Translation(item.tileOffset);
        if (item.isDecal) {
            // Lift the quad a hair above the surface it projects onto so depth
            // testing keeps it visible (no z-fighting on flat ground).
            model = model * math::Mat4::Translation({0.0f, 0.02f, 0.0f});
        }
        // Column-vector convention: translation lives in the last column
        // (m[3], m[7], m[11]); reading m[12..14] returned ~0 and broke LOD
        // distance selection + decal placement.
        // G2-3: a terrain chunk uses its patch centre (not the terrain origin)
        // so distance LOD is chosen per patch.
        const math::Vec3 worldPos = item.isTerrainChunk
                                        ? model.TransformPoint(item.chunkCenterLocal)
                                        : math::Vec3{model.m[3], model.m[7], model.m[11]};
        // Batchable: opaque static mesh with the built-in shader. Skinned
        // (per-entity bone matrices), sprites, decals, transparent materials
        // and custom shaders keep the per-entity path.
        const bool batchable = canBatch && !item.skinned && !item.isSprite && !item.isDecal &&
                               !item.mat.transparent && !item.mat.shader.Valid() &&
                               item.mesh.Valid() && item.gltfSubNodes.empty();
        if (batchable) {
            if (!bvhVisible_.empty() && bvhVisible_[idx] == 0) continue; // pre-culled
            gfx::Mesh drawMesh = SelectLodMesh(item.mesh, item.chain, worldPos, cam.position);
            if (!drawMesh.Valid()) continue;
            int batchIndex = -1;
            for (size_t bi = 0; bi < drawBatches_.size(); ++bi) {
                if (drawBatches_[bi].mesh.Handle().vao == drawMesh.Handle().vao &&
                    SameMaterial(drawBatches_[bi].mat, item.mat)) {
                    batchIndex = static_cast<int>(bi);
                    break;
                }
            }
            if (batchIndex < 0) {
                DrawBatch b;
                b.mesh = drawMesh;
                b.mat = item.mat;
                b.start = static_cast<uint32_t>(batchModels_.size());
                batchIndex = static_cast<int>(drawBatches_.size());
                drawBatches_.push_back(b);
            }
            batchModels_.push_back(model);
            drawBatches_[static_cast<size_t>(batchIndex)].count++;
            continue;
        }
        flushBatches(); // keep relative order with non-batched draws
        if (item.skinned && item.skinned->Valid()) {
            // Task 12: the override pose comes from the AnimationSystem state
            // table (PoseFor); the default path falls back to the model's own
            // BoneMatrices(), matching the old DrawItem override branch.
            std::vector<math::Mat4> bones;
            if (!anims.PoseFor(EntityKey(item.ent), item.skinned->skeleton, bones))
                bones = item.skinned->BoneMatrices();
            for (const auto& part : item.skinned->parts)
                renderer.DrawSkinnedMesh(part.mesh, part.material, model,
                                         bones, static_cast<int>(bones.size()));
        } else if (item.isSprite) {
            if (item.billboard) {
                // Camera-facing quad (world-space VFX): rebuild the model
                // basis from the view vector each frame. A 2D front-ortho
                // camera degenerates to the identity basis, so 2D sprites are
                // unaffected.
                math::Vec3 fwd = cam.position - worldPos;
                const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
                if (fl > 0.0001f) fwd = fwd * (1.0f / fl);
                math::Vec3 right = math::Cross(math::Vec3{0.0f, 1.0f, 0.0f}, fwd);
                const float rl = std::sqrt(right.x * right.x + right.y * right.y +
                                           right.z * right.z);
                if (rl > 0.0001f) right = right * (1.0f / rl);
                math::Vec3 up = math::Cross(fwd, right);
                // Scale magnitude recovered from the composed model matrix.
                const float sx = std::sqrt(model.m[0] * model.m[0] + model.m[4] * model.m[4] +
                                           model.m[8] * model.m[8]);
                const float sy = std::sqrt(model.m[1] * model.m[1] + model.m[5] * model.m[5] +
                                           model.m[9] * model.m[9]);
                math::Mat4 bb;
                bb.m[0] = right.x * sx; bb.m[4] = right.y * sx; bb.m[8] = right.z * sx;
                bb.m[1] = up.x * sy;    bb.m[5] = up.y * sy;    bb.m[9] = up.z * sy;
                bb.m[2] = fwd.x;        bb.m[6] = fwd.y;        bb.m[10] = fwd.z;
                bb.m[12] = worldPos.x;  bb.m[13] = worldPos.y;  bb.m[14] = worldPos.z;
                renderer.DrawMesh(item.mesh, item.mat, bb);
            } else {
                // Flip mirrors the quad around its center: a negative local scale
                // keeps the texture upright and needs no UV/shader changes.
                if (item.flipX || item.flipY)
                    model = model * math::Mat4::Scale({item.flipX ? -1.0f : 1.0f,
                                                       item.flipY ? -1.0f : 1.0f, 1.0f});
                renderer.DrawMesh(item.mesh, item.mat, model);
            }
        } else {
            renderer.DrawMesh(SelectLodMesh(item.mesh, item.chain, worldPos, cam.position),
                              item.mat, model);
        }
        // 多 mesh glTF 场景的子节点（第 2+ mesh，自带累积变换 + 材质）。
        for (const assets::GltfMeshNode& sub : item.gltfSubNodes) {
            if (!sub.mesh.Valid()) continue;
            renderer.DrawMesh(sub.mesh, sub.material, model * sub.transform);
        }
    }
    flushBatches();
    // Skill projectiles (fireballs): bright glowing orbs (ProjectileSystem
    // lazily builds the shared fireball mesh on first use).
    projectiles.Draw(renderer);
    // G2-3 vegetation: instanced plant meshes + far yaw-billboard impostors.
    // Use the RESOLVED scene camera (`cam`, which may have been overridden by a
    // scene Camera3D entity driven by the game script) rather than the raw
    // fallback `camera` param. Otherwise the far-tree impostor cards (which
    // yaw to face the camera) would follow the host's free/orbit camera, so
    // right-drag orbits the foliage billboards while the world stays put.
    DrawVegetation(renderer, cam, world, sceneTree);
    // Script VFX particles: world-space camera-facing billboards (additive +
    // alpha batches), drawn INSIDE the HDR target so bright particles bloom.
    particles.Draw(renderer);
    // Compact when a fifth of the draw list belongs to dead entities.
    if (dead && dead * 5 > draws_.size()) {
        draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                    [&](const DrawItem& i) { return !world.Alive(i.ent); }),
                     draws_.end());
    }

    // 2D script canvas: a global on_render() handler draws the game with the
    // DrawRect/DrawRectOutline/DrawText bindings (design units 1280x720). The
    // runtime flushes the buffer into the renderer's 2D overlay so 2D games
    // (e.g. the PvZ project) need zero C++ gameplay code.
    if (luaHost || jsHost) {
        scriptCtx.draw2d = canvas.Commands();
        // Snapshot the host's live 2D mapping (Set2DViewport) so on_update's
        // InputMousePos() and UI hit-tests keep design coordinates between
        // renders (the renderer resets its 2D viewport after the frame).
        uiScale = renderer.UIScale();
        uiOffset = renderer.UI2DOffset();
        scriptCtx.screenToUi = [&](const math::Vec2& p) {
            return (p - uiOffset) / uiScale;
        };
        canvas.Begin();
        scriptCtx.currentEntity = {};
        // Global handlers can be defined by either backend; Lua wins ties
        // (deterministic order).
        for (script::IScriptHost* h : {luaHost, jsHost}) {
            if (!h || !h->HasFunction("on_render")) continue;
            const core::Result<script::Value> res = h->Call("on_render", {});
            if (!res.Ok()) {
                NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Error,
                             "runtime: on_render() failed: %s",
                             h->LastError().message.c_str());
            }
        }
        scriptCtx.draw2d = nullptr;
        // G5-4-4: the on_render canvas is HUD �?the host flushes it AFTER the
        // scene is composited (FlushCanvas), so its colors stay exactly as
        // authored instead of being tone-mapped/bloomed with the 3D scene.
        // ScriptCanvas::Flush is called from FlushCanvas (post-EndScene).
    }
}

gfx::Mesh DrawSystem::MeshForEntity(const ecs::Entity& ent, const gfx::Camera& camera,
                                    const ecs::World& world) const {
    for (const DrawItem& item : draws_) {
        if (item.ent != ent || !item.resolved || item.failed) continue;
        const SceneTransform* t = world.Get<SceneTransform>(ent);
        if (!t) return gfx::Mesh{};
        return SelectLodMesh(item.mesh, item.chain, t->pos, camera.position);
    }
    return gfx::Mesh{};
}

void DrawSystem::SetSpriteFrames(ecs::Entity e, const std::vector<std::string>& frames,
                                 float fps) {
    for (DrawItem& d : draws_) {
        if (d.ent != e) continue;
        d.spriteFrames = frames;
        d.spriteFps = fps;
        d.spriteLoop = true;
        d.spriteAnimTime = 0.0f;
        d.spriteFrame = -1;
        d.spriteTex = frames.empty() || frames[0].empty() ? std::string() : frames[0];
        d.sheetTex.clear();
        d.sheetFrames = 0;
        d.resolved = false;
        break;
    }
}

void DrawSystem::SetSpriteSheet(ecs::Entity e, const std::string& sheet, int count, float fps) {
    for (DrawItem& d : draws_) {
        if (d.ent != e) continue;
        d.sheetTex = sheet;
        d.sheetFrames = count;
        d.spriteFps = fps;
        d.spriteLoop = true;
        d.spriteAnimTime = 0.0f;
        d.spriteFrame = -1;
        d.spriteTex = sheet;
        d.spriteFrames.clear();
        d.resolved = false;
        break;
    }
}

void DrawSystem::AdvanceSprites(float dt) {
    if (draws_.empty()) return;
    for (DrawItem& d : draws_) {
        if (!d.isSprite) continue;
        const bool sheet = !d.sheetTex.empty() && d.sheetFrames > 0;
        if (d.spriteFps <= 0.0f) continue;
        if (!sheet && d.spriteFrames.empty()) continue;
        d.spriteAnimTime += dt;
        int frame = static_cast<int>(d.spriteAnimTime * d.spriteFps);
        const int n = sheet ? d.sheetFrames : static_cast<int>(d.spriteFrames.size());
        if (d.spriteLoop) {
            frame = frame % n;
        } else {
            frame = frame < n ? frame : n - 1;
        }
        if (frame != d.spriteFrame) {
            d.spriteFrame = frame;
            if (sheet) {
                d.spriteTex = d.sheetTex; // texture unchanged; UV window changes
            } else {
                d.spriteTex = d.spriteFrames[static_cast<size_t>(frame)];
            }
            d.resolved = false; // re-resolve (new frame texture / UV quad)
        }
    }
}

bool DrawSystem::HasSkinned(ecs::Entity e) const {
    for (const DrawItem& d : draws_)
        if (d.ent == e && d.skinned && d.skinned->Valid()) return true;
    return false;
}

void DrawSystem::Clear() {
    draws_.clear();
    drawKeys_.clear();
    drawBatches_.clear();
    batchModels_.clear();
    drawBvh_.Clear();
    bvhVisible_.clear();
    drawOrder_.clear();
    // vegCache_ intentionally NOT cleared here: the pre-split Stop() only
    // cleared draws_, and stale entries are pruned by DrawVegetation's
    // per-frame alive check (same Start/Stop semantics preserved).
}

} // namespace neon::scene
