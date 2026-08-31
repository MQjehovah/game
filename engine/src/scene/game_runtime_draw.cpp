// C1: GameRuntime draw-list subsystem: BuildDrawList / SyncDrawKeys /
// ResolveOrSkip / ResolveDrawItem / ResolveMeshKey / VegetationMesh /
// DrawVegetation. Split out of game_runtime.cpp; owns draw-list building and
// per-draw mesh/LOD resolution.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <cmath>

#include "neon/assets/asset_manager.hpp"
#include "neon/assets/mesh_format.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/gfx/terrain.hpp"

namespace neon::scene {
using namespace detail; // SameMaterial / SelectLodMesh / EntityKey

void GameRuntime::BuildDrawList() {
    // Synchronize with the live entity set: scripts can Spawn/Despawn entities
    // (SpawnSprite, Despawn) while running, so drop dead draws and append new
    // mesh/sprite entities each call while keeping resolved items cached.
    draws_.erase(std::remove_if(draws_.begin(), draws_.end(),
                                [this](const DrawItem& d) { return !world_.Alive(d.ent); }),
                 draws_.end());
    // B6: rebuild the alive-entity index once (draws_ shrinks above); the
    // contains() checks below become O(1) lookups instead of O(N) scans.
    drawKeys_.clear();
    for (const DrawItem& d : draws_) drawKeys_.insert(EntityKey(d.ent));
    // M1: sync per-entity animation overrides into their draw items (existing
    // items get name updates; resolved clip pointers re-resolve on change).
    for (DrawItem& d : draws_) {
        const SceneAnimOverride* ov = world_.Get<SceneAnimOverride>(d.ent);
        if (!ov || !ov->active) {
            d.animHasOverride = false;
            d.animClip = nullptr;
            d.animName.clear();
            continue;
        }
        if (d.animName != ov->clip) {
            d.animName = ov->clip;
            d.animClip = nullptr; // re-resolve in TickAnimations
            d.animLoop = ov->loop;
            d.animSpeed = ov->speed;
            d.animFadeTotal = ov->crossFade;
        }
        d.animHasOverride = true;
    }
    auto contains = [this](ecs::Entity e) { return drawKeys_.count(EntityKey(e)) != 0; };
    auto view = world_.ViewAll<SceneMesh>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneMesh>(i);
        if (contains(ent)) continue; // already tracked (resolved state kept)
        const SceneMesh* m = world_.Get<SceneMesh>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!m || !t) continue; // a mesh without a transform draws nothing
        // G2-3 chunked-LOD terrain: a terrain entity with chunkGridDiv > 0
        // becomes gridDiv x gridDiv patch draw items (each with its own LodChain
        // + per-patch distance LOD), instead of one monolithic terrain mesh.
        const SceneTerrain* terr = world_.Get<SceneTerrain>(ent);
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
                            cfg_.assets
                                ->LoadTexture(FullAssetPath(m->albedoTex), opts)
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
        const bool gltfBase = m->meshKey.compare(0, 5, "gltf:") == 0 && cfg_.assets;
        if (gltfBase) {
            assets::GltfAsset gltf =
                cfg_.assets->LoadGLTF(FullAssetPath(m->meshKey.substr(5)));
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
        if (cfg_.assets) {
            // UV tiling: when uvRepeat > 1 the sampler must use REPEAT, else
            // clamp pulls edge pixels and the tiling collapses into streaks.
            assets::TextureLoadOptions opts;
            if (m->uvRepeat > 1.01f) opts.wrap = gfx::Wrap::Repeat;
            if (!m->albedoTex.empty())
                item.mat.albedo =
                    cfg_.assets->LoadTexture(FullAssetPath(m->albedoTex), opts).Handle();
            if (!m->mrTex.empty())
                item.mat.metallicRoughness =
                    cfg_.assets->LoadTexture(FullAssetPath(m->mrTex), opts).Handle();
            if (!m->aoTex.empty())
                item.mat.occlusion =
                    cfg_.assets->LoadTexture(FullAssetPath(m->aoTex), opts).Handle();
            if (!m->emissiveTex.empty())
                item.mat.emissive =
                    cfg_.assets->LoadTexture(FullAssetPath(m->emissiveTex), opts).Handle();
        }
        // M1: carry a live animation override onto a newly-tracked item.
        if (const SceneAnimOverride* ov = world_.Get<SceneAnimOverride>(ent);
            ov && ov->active) {
            item.animHasOverride = true;
            item.animName = ov->clip;
            item.animLoop = ov->loop;
            item.animSpeed = ov->speed;
            item.animFadeTotal = ov->crossFade;
        }
        draws_.push_back(std::move(item));
    }
    auto spriteView = world_.ViewAll<SceneSprite>();
    for (size_t i = 0; i < spriteView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneSprite>(i);
        if (contains(ent)) continue;
        const SceneSprite* s = world_.Get<SceneSprite>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
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
    auto tileView = world_.ViewAll<SceneTilemap>();
    for (size_t i = 0; i < tileView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneTilemap>(i);
        if (contains(ent)) continue;
        const SceneTilemap* tm = world_.Get<SceneTilemap>(ent);
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
    auto decalView = world_.ViewAll<SceneDecal>();
    for (size_t i = 0; i < decalView.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneDecal>(i);
        if (contains(ent)) continue;
        const SceneDecal* d = world_.Get<SceneDecal>(ent);
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
void GameRuntime::SyncDrawKeys() {
    drawKeys_.clear();
    for (const DrawItem& d : draws_) drawKeys_.insert(EntityKey(d.ent));
}

void GameRuntime::ResolveOrSkip(DrawItem& item, gfx::Renderer& renderer) {
    if (item.resolved) return;
    if (item.asyncPending) {
        // Async mesh load still in flight: probe the cache and resolve from it
        // the frame it becomes ready; otherwise leave unresolved (skipped).
        if (cfg_.assets) {
            const std::string& key = item.meshKey;
            bool ready = true;
            if (key.compare(0, 4, "obj:") == 0)
                ready = cfg_.assets->HasMesh(FullAssetPath(key.substr(4)));
            else if (key.compare(0, 5, "gltf:") == 0)
                ready = cfg_.assets->HasGLTF(FullAssetPath(key.substr(5)));
            if (ready) {
                item.asyncPending = false;
                ResolveDrawItem(item, renderer);
            }
        }
        return; // still loading -> skip this frame
    }
    ResolveDrawItem(item, renderer);
}

void GameRuntime::ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer) {
    if (item.resolved || item.failed || !cfg_.assets) return;

    if (item.isSprite) {
        gfx::Texture tex = cfg_.assets->LoadTexture(FullAssetPath(item.spriteTex));
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
        gfx::Texture tex = cfg_.assets->LoadTexture(FullAssetPath(item.spriteTex));
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
        const SceneTerrain* terr = world_.Get<SceneTerrain>(item.ent);
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
        if (const SceneMesh* sm = world_.Get<SceneMesh>(item.ent); sm) {
            if (!sm->albedoTex.empty()) {
                assets::TextureLoadOptions opts;
                opts.wrap = gfx::Wrap::Repeat; // terrain UV spans > [0,1]
                item.mat.grassTex =
                    cfg_.assets->LoadTexture(FullAssetPath(sm->albedoTex), opts).Handle();
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
    if (cfg_.asyncMeshLoad && cfg_.assets &&
        (key.compare(0, 4, "obj:") == 0 || key.compare(0, 5, "gltf:") == 0)) {
        const bool isObj = key.compare(0, 4, "obj:") == 0;
        // Use the project-relative virtual path (assets/...), NOT a project-dir-
        // absolute one: glTF's external buffers/images resolve relative to the
        // .gltf via the VFS, and an absolute path is rejected by it.
        const std::string full = FullAssetPath(isObj ? key.substr(4) : key.substr(5));
        const bool cached = isObj ? cfg_.assets->HasMesh(full) : cfg_.assets->HasGLTF(full);
        if (!cached) {
            auto noop = [](bool) {};
            if (isObj) cfg_.assets->LoadMeshOBJAsync(full, noop);
            else cfg_.assets->LoadGLTFAsync(full, noop);
            item.asyncPending = true;
            return;
        }
    }
    const SceneTerrain* terr = key == "terrain" ? world_.Get<SceneTerrain>(item.ent) : nullptr;
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

    // Animated skinned glTF: resolve the full model (all skinned mesh parts +
    // skeleton + clips) so Draw() can use bone matrices. LOD chains are not
    // supported for skinned models (the file's parts are the model).
    if (mesh.Skinned()) {
        core::Result<SkinnedModel> sm =
            LoadSkinnedModel(*cfg_.assets, FullAssetPath(key.substr(5)));
        if (!sm.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                         "runtime: skinned model '%s' failed to resolve: %s", key.c_str(),
                         sm.Error().c_str());
            item.failed = true;
            return;
        }
        item.skinned = std::make_shared<SkinnedModel>(std::move(sm.Value()));
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

gfx::Mesh GameRuntime::ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
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
        const std::string resolved = FullAssetPath(path);
        assets::MeshLoadResult res =
            assets::MeshFormatRegistry::Instance().Load(*cfg_.assets, prefix + ":" + resolved);
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

gfx::Mesh GameRuntime::VegetationMesh(gfx::Renderer& renderer, const std::string& meshKey) {
    return ResolveMeshKey(renderer, meshKey);
}

void GameRuntime::DrawVegetation(gfx::Renderer& renderer, const gfx::Camera& camera) {
    if (!cfg_.assets) return;
    // Prune cache entries whose terrain entity has despawned since the last
    // frame (script Spawn/Despawn).
    for (auto it = vegCache_.begin(); it != vegCache_.end();) {
        if (!world_.Alive(it->second.ent)) {
            it = vegCache_.erase(it);
        } else {
            ++it;
        }
    }

    auto view = world_.ViewAll<SceneTerrain>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneTerrain>(i);
        const SceneTerrain* terr = world_.Get<SceneTerrain>(ent);
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

        const math::Mat4 terrainModel = CachedLocalToWorld(ent);
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

} // namespace neon::scene
