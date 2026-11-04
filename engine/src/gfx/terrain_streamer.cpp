#include "neon/gfx/terrain_streamer.hpp"

#include <algorithm>

namespace neon::gfx {

size_t TerrainStreamer::KeyHash::operator()(const world::TerrainChunkKey& key) const {
    size_t h = static_cast<size_t>(key.level) * 73856093u;
    h ^= static_cast<size_t>(key.x) * 19349663u;
    h ^= static_cast<size_t>(key.z) * 83492791u;
    return h;
}

void TerrainStreamer::Configure(const world::TerrainWorld* world,
                                const TerrainStreamerConfig& config) {
    // Configure runs at world creation, before any chunk exists; the caller is
    // responsible for Clear()ing a live streamer if it reconfigures mid-run.
    chunks_.clear();
    selected_.clear();
    missing_.clear();
    world_ = world;
    config_ = config;
}

void TerrainStreamer::Update(Renderer& renderer, const math::Vec3& focus, float dt) {
    (void)dt;
    if (world_ == nullptr) return;

    world::SelectTerrainChunks({focus.x, focus.z}, config_.viewDistance, config_.maxLevel,
                               selected_);
    missing_.clear();
    for (const auto& key : selected_)
        if (chunks_.find(key) == chunks_.end()) missing_.push_back(key);

    // Nearest chunks first: if the budget caps residency, the player sees the
    // ground under their feet before the horizon fills in.
    std::sort(missing_.begin(), missing_.end(),
              [focus](const world::TerrainChunkKey& a, const world::TerrainChunkKey& b) {
                  auto dist2 = [&focus](const world::TerrainChunkKey& k) {
                      const float size = world::TerrainChunkSize(k.level);
                      const float dx = (static_cast<float>(k.x) + 0.5f) * size - focus.x;
                      const float dz = (static_cast<float>(k.z) + 0.5f) * size - focus.z;
                      return dx * dx + dz * dz;
                  };
                  return dist2(a) < dist2(b);
              });

    int built = 0;
    for (const auto& key : missing_) {
        if (chunks_.size() >= config_.maxResident) break;
        if (built >= config_.buildsPerFrame) break;

        world::TerrainChunkMeshData data;
        world::BuildTerrainChunkData(*world_, key, config_.cells, config_.skirtDepth, data);
        if (data.vertices.empty() || data.indices.empty()) continue;

        std::vector<Vertex3D> verts;
        verts.reserve(data.vertices.size());
        for (const auto& v : data.vertices) {
            Vertex3D out;
            out.pos = v.position;
            out.normal = v.normal;
            out.uv = v.uv;
            out.color = v.weights;
            verts.push_back(out);
        }

        Mesh mesh = Mesh::CreateFromDataU32(
            renderer, verts.data(), static_cast<uint32_t>(verts.size()),
            data.indices.data(), static_cast<uint32_t>(data.indices.size()), "terrain_chunk");
        if (!mesh.Valid()) continue;

        Chunk chunk;
        chunk.mesh = std::move(mesh);
        chunk.tint = data.tint;
        chunk.level = key.level;
        chunk.x = key.x;
        chunk.z = key.z;
        chunks_.emplace(key, std::move(chunk));
        ++built;
    }

    // Evict AFTER building, and keep a parent that covers a selected child we
    // have not built yet. Evicting first left a hole for the frames the
    // buildsPerFrame budget needed to fill the children.
    auto chunk_covers = [](const world::TerrainChunkKey& parent,
                           const world::TerrainChunkKey& child) {
        const float ps = world::TerrainChunkSize(parent.level);
        const float cs = world::TerrainChunkSize(child.level);
        const float px0 = static_cast<float>(parent.x) * ps;
        const float pz0 = static_cast<float>(parent.z) * ps;
        const float cx0 = static_cast<float>(child.x) * cs;
        const float cz0 = static_cast<float>(child.z) * cs;
        return cx0 >= px0 - 0.01f && cx0 + cs <= px0 + ps + 0.01f &&
               cz0 >= pz0 - 0.01f && cz0 + cs <= pz0 + ps + 0.01f;
    };
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        bool keep = std::find(selected_.begin(), selected_.end(), it->first) !=
                    selected_.end();
        if (!keep) {
            for (const auto& pending : missing_) {
                if (chunks_.find(pending) != chunks_.end()) continue;  // built this frame
                if (chunk_covers(it->first, pending)) {
                    keep = true;
                    break;
                }
            }
        }
        if (keep) {
            ++it;
        } else {
            it->second.mesh.Release(renderer);
            it = chunks_.erase(it);
        }
    }
}

void TerrainStreamer::Draw(Renderer& renderer, const math::Mat4& viewProj,
                           const Material& terrainMaterial) {
    (void)viewProj;
    if (chunks_.empty()) return;

    Material material = terrainMaterial;
    // The 2K texture-array path currently uploads/samples the wrong slice on
    // this GL backend (the ground renders as a flat tan sheet). Use the
    // shader's procedural grass/rock/snow palette until the array slice upload
    // is fixed; this keeps the world readable and skips ~400 MB of array VRAM.
    material.terrainAlbedoArray = {};
    material.terrainNormalArray = {};
    material.terrainOrmArray = {};
    if (!material.shader.Valid()) material.shader = renderer.TerrainLayersShader();
    material.lit = true;
    material.castShadow = false;
    // Large terrain-shadow patches at low sun angles read as rendering bugs on
    // the low-poly ground (the terrain has no fine geometry to justify them).
    // Keep object shadows, but let the ground itself stay evenly lit.
    material.receiveShadow = false;
    material.doubleSided = true;

    const math::Mat4 identity = math::Mat4::Identity();
    const math::Frustum& viewFrustum = renderer.ViewFrustum();
    drawnLastFrame_ = 0;
    for (auto& kv : chunks_) {
        // Chunk meshes are already in world space (identity transform), so the
        // mesh bounds double as a world AABB. Cull chunks behind / beside the
        // camera: the streamer keeps a square window around the focus, but only
        // the view frustum needs to be submitted. The 8 m margin keeps a chunk
        // whose bounds are just outside the frustum from popping while the
        // camera turns (the skirt can still be visible at the edge).
        math::AABB bounds = kv.second.mesh.Bounds();
        bounds.min.x -= 2.0f;
        bounds.min.y -= 2.0f;
        bounds.min.z -= 2.0f;
        bounds.max.x += 2.0f;
        bounds.max.y += 2.0f;
        bounds.max.z += 2.0f;
        if (!viewFrustum.Intersects(bounds)) continue;
        Material chunkMaterial = material;
        chunkMaterial.tint = Color{kv.second.tint.x, kv.second.tint.y,
                                   kv.second.tint.z, 1.0f};
        renderer.DrawMesh(kv.second.mesh, chunkMaterial, identity);
        ++drawnLastFrame_;
    }
}

void TerrainStreamer::Clear(Renderer& renderer) {
    for (auto& kv : chunks_) kv.second.mesh.Release(renderer);
    chunks_.clear();
    selected_.clear();
    missing_.clear();
}

} // namespace neon::gfx
