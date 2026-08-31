#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"
#include "neon/gfx/mesh.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

// ufbx: single-file FBX loader (vendored, MIT). Compiled as C in its own
// target; here we only consume its public header.
#include "ufbx.h"

namespace neon::assets {

namespace {
// (ubx transform helpers — ufbx_transform_position / ufbx_transform_direction
// are called directly below; no engine-matrix conversion is needed.)

// FBX stores positions, normals and UVs in independent arrays; every face
// corner points into each array through its own index map. A single interleaved
// Vertex3D is therefore keyed on the (position, normal, uv) index triple of a
// corner: a position shared by two faces with different normals (hard edges) or
// UVs (seams) must split into two vertices or the model tears/shades wrong.
struct CornerKey {
    uint32_t pos = 0;
    uint32_t nrm = 0;
    uint32_t uv = 0;
    bool operator==(const CornerKey& o) const {
        return pos == o.pos && nrm == o.nrm && uv == o.uv;
    }
};
struct CornerKeyHash {
    size_t operator()(const CornerKey& k) const {
        size_t h = std::hash<uint32_t>{}(k.pos);
        h = h * 0x9e3779b97f4a7c15ull ^ std::hash<uint32_t>{}(k.nrm);
        h = h * 0x9e3779b97f4a7c15ull ^ std::hash<uint32_t>{}(k.uv);
        return h;
    }
};
} // namespace

// AssetManager::LoadFBX: load an FBX via ufbx and merge every mesh-bearing node
// into ONE gfx::Mesh. The engine's render model is one entity = one mesh, so a
// multi-part FBX (e.g. an ancient village) is baked into a single geometry by
// applying each node's world transform and concatenating verts/indices. The
// merged mesh uses 32-bit indices when it exceeds 65535 vertices.
//
// Materials are not fully recovered (per-part FBX materials/textures would need
// a per-part entity path); a single white lit material is produced and the
// scene's mesh.material can override the tint/texture per entity.
FbxAsset AssetManager::LoadFBX(const std::string& path) {
    {
        auto it = fbxCache_.find(path);
        if (it != fbxCache_.end()) return it->second;
    }

    const core::Result<std::vector<uint8_t>> fileBytes = IoRead(path);
    if (!fileBytes.Ok()) {
        NEON_LOG_ERROR("Asset: failed to open FBX '%s'", path.c_str());
        return {};
    }

    ufbx_error err;
    ufbx_load_opts opts;
    std::memset(&opts, 0, sizeof(opts));
    ufbx_scene* scene =
        ufbx_load_memory(fileBytes.Value().data(), fileBytes.Value().size(), &opts, &err);
    if (!scene) {
        NEON_LOG_ERROR("FBX '%s': %s", path.c_str(), err.description.data);
        return {};
    }

    FbxAsset out;
    std::vector<gfx::Vertex3D> allVerts;
    std::vector<uint32_t> allIndices;
    allVerts.reserve(1024);
    bool anyUfbxNormal = false;

    // Walk every mesh-bearing node (a mesh can be shared by several nodes, that
    // is fine: each becomes a copy of the geometry at its transform).
    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        if (!node) continue;
        const ufbx_mesh* m = node->mesh;
        if (!m || m->num_vertices == 0 || m->num_indices == 0) continue;

        // Dedup (position, normal, uv) triples so adjacent triangles share a
        // single interleaved vertex and split only where the attributes do.
        std::unordered_map<CornerKey, uint32_t, CornerKeyHash> remap;
        remap.reserve(m->num_indices * 2);

        // Emit one merged Vertex3D for a mesh corner index (`ci` indexes the
        // mesh's combined corner array, i.e. `ufbx_mesh.num_indices`).
        auto emit = [&](uint32_t ci) -> uint32_t {
            const uint32_t pi = m->vertex_indices.data[ci]; // corner -> position
            const uint32_t ni = m->vertex_normal.exists
                                    ? m->vertex_normal.indices.data[ci]
                                    : UINT32_MAX;
            const uint32_t ui = m->vertex_uv.exists
                                    ? m->vertex_uv.indices.data[ci]
                                    : UINT32_MAX;

            const CornerKey key{pi, ni, ui};
            auto it = remap.find(key);
            if (it != remap.end()) return it->second;

            // Bake the node's geometry->world transform into the vertex. ufbx
            // computes geometry_to_world as the correct matrix (it accounts for
            // geometric transforms); node_to_world does NOT and mis-places /
            // stretches multi-mesh models. Use ufbx's own transform helpers so
            // the axis conversion + matrix layout are handled correctly rather
            // than reimplemented with a Mat4 reinterpret.
            const ufbx_vec3& p = m->vertices.data[pi];
            const ufbx_vec3 wp = ufbx_transform_position(&node->geometry_to_world, p);

            gfx::Vertex3D v{};
            v.pos = {static_cast<float>(wp.x), static_cast<float>(wp.y),
                     static_cast<float>(wp.z)};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {0.0f, 0.0f};
            if (ni != UINT32_MAX) {
                const ufbx_vec3& nn = m->vertex_normal.values.data[ni];
                const ufbx_vec3 n0 = {(float)nn.x, (float)nn.y, (float)nn.z};
                const ufbx_vec3 wn = ufbx_transform_direction(&node->geometry_to_world, n0);
                v.normal = {static_cast<float>(wn.x), static_cast<float>(wn.y),
                            static_cast<float>(wn.z)};
                anyUfbxNormal = true;
            }
            if (ui != UINT32_MAX) {
                const ufbx_vec2& tuv = m->vertex_uv.values.data[ui];
                v.uv = {static_cast<float>(tuv.x), static_cast<float>(tuv.y)};
            }

            const uint32_t out = static_cast<uint32_t>(allVerts.size());
            allVerts.push_back(v);
            remap.emplace(key, out);
            return out;
        };

        // Triangulate every face. ufbx_triangulate_face writes ABSOLUTE corner
        // indices (already offset by face.index_begin), so they are fed straight
        // into `emit`. The previous code added face.index_begin a second time
        // and skipped the corner->position map, which read indices past the
        // face and mis-connected triangles — the source of torn faces and
        // stretched triangles on quad/ngon meshes.
        std::vector<uint32_t> triBuf(m->max_face_triangles * 3);
        for (size_t fi = 0; fi < m->faces.count; ++fi) {
            const ufbx_face face = m->faces.data[fi];
            if (face.num_indices < 3) continue;
            const uint32_t triCount =
                ufbx_triangulate_face(triBuf.data(), triBuf.size(), m, face);
            for (uint32_t ti = 0; ti < triCount * 3; ++ti)
                allIndices.push_back(emit(triBuf[ti]));
        }
    }

    if (allVerts.empty() || allIndices.size() < 3) {
        ufbx_free_scene(scene);
        return out;
    }

    // Trust the source vertex normals when the FBX provides them (they encode
    // the author's smoothed shading + winding). Only fall back to computing
    // flat normals when the file has no normal stream — a recomputed average
    // over merged, possibly-mixed-winding meshes flips faces and shows as
    // torn/black patches.
    if (!anyUfbxNormal) {
        std::vector<neon::math::Vec3> accum(allVerts.size(), math::Vec3{0, 0, 0});
        for (size_t ii = 0; ii + 2 < allIndices.size(); ii += 3) {
            uint32_t a = allIndices[ii], b = allIndices[ii + 1], c = allIndices[ii + 2];
            if (a >= allVerts.size() || b >= allVerts.size() || c >= allVerts.size()) continue;
            const neon::math::Vec3& pa = allVerts[a].pos;
            const neon::math::Vec3& pb = allVerts[b].pos;
            const neon::math::Vec3& pc = allVerts[c].pos;
            neon::math::Vec3 n = neon::math::Cross(pb - pa, pc - pa);
            accum[a] += n;
            accum[b] += n;
            accum[c] += n;
        }
        for (size_t i = 0; i < allVerts.size(); ++i) {
            if (accum[i].Length() > 1e-6f) allVerts[i].normal = accum[i].Normalized();
        }
    }

    // Normalize to a canonical unit footprint: FBX files come from many tools
    // with wildly different source units (mm vs cm vs m — a model can span
    // 1.5M units). Fit the merged geometry into a ~1-unit box centred at the
    // origin so the scene's scale controls the real displayed size and the
    // model viewer frames it consistently. Bounds are computed on the world-
    // transformed positions already applied above.
    {
        neon::math::Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
        for (const gfx::Vertex3D& v : allVerts) {
            bmin.x = std::min(bmin.x, v.pos.x); bmin.y = std::min(bmin.y, v.pos.y);
            bmin.z = std::min(bmin.z, v.pos.z);
            bmax.x = std::max(bmax.x, v.pos.x); bmax.y = std::max(bmax.y, v.pos.y);
            bmax.z = std::max(bmax.z, v.pos.z);
        }
        const neon::math::Vec3 ext = bmax - bmin;
        const float maxDim = std::max({ext.x, ext.y, ext.z, 1e-6f});
        const float s = 1.0f / maxDim;
        const neon::math::Vec3 center = (bmin + bmax) * 0.5f;
        for (gfx::Vertex3D& v : allVerts) v.pos = (v.pos - center) * s;
    }

    gfx::Mesh mesh = gfx::Mesh::CreateFromDataU32(
        *renderer_, allVerts.data(), static_cast<uint32_t>(allVerts.size()), allIndices.data(),
        static_cast<uint32_t>(allIndices.size()), path);
    ufbx_free_scene(scene);
    if (!mesh.Valid()) return out;

    gfx::Material mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
    mat.doubleSided = true;

    FbxMeshNode node;
    node.transform = neon::math::Mat4::Identity();
    node.mesh = std::move(mesh);
    node.material = std::move(mat);
    node.name = path;
    out.nodes.push_back(std::move(node));

    fbxCache_[path] = out;
    NEON_LOG_INFO("Asset: loaded FBX '%s' (merged %zu verts, %zu indices)",
                  path.c_str(), allVerts.size(), allIndices.size());
    return out;
}

} // namespace neon::assets
