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

    // Unload everything that left the window (and release its GPU handle).
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const bool keep = std::find(selected_.begin(), selected_.end(), it->first) !=
                          selected_.end();
        if (!keep) {
            it->second.mesh.Release(renderer);
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

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
        chunk.level = key.level;
        chunk.x = key.x;
        chunk.z = key.z;
        chunks_.emplace(key, std::move(chunk));
        ++built;
    }
}

void TerrainStreamer::Draw(Renderer& renderer, const math::Mat4& viewProj,
                           const Material& terrainMaterial) {
    (void)viewProj;
    if (chunks_.empty()) return;

    Material material = terrainMaterial;
    if (!material.shader.Valid()) material.shader = renderer.TerrainLayersShader();
    material.lit = true;
    material.castShadow = false;
    material.receiveShadow = true;
    material.doubleSided = true;

    const math::Mat4 identity = math::Mat4::Identity();
    for (auto& kv : chunks_) renderer.DrawMesh(kv.second.mesh, material, identity);
}

void TerrainStreamer::Clear(Renderer& renderer) {
    for (auto& kv : chunks_) kv.second.mesh.Release(renderer);
    chunks_.clear();
    selected_.clear();
    missing_.clear();
}

} // namespace neon::gfx
