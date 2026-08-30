#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"
#include "neon/gfx/mesh.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

// ufbx: single-file FBX loader (vendored, MIT). Compiled as C in its own
// target; here we only consume its public header.
#include "ufbx.h"

namespace neon::assets {

namespace {
// Convert a ufbx 3x4 column-major double matrix (v[12], no perspective row)
// into the engine's 4x4 row-storage float Mat4 (m[16], translation in the last
// column). They share the "translation in the last column" convention, so the
// copy is a straight per-column fill; only the width (3x4 vs 4x4) and element
// type (double vs float) differ.
neon::math::Mat4 UfbxMatToEngine(const ufbx_matrix& M) {
    neon::math::Mat4 out = neon::math::Mat4::Identity();
    const double* v = M.v;
    // Column 0 (X axis).
    out.m[0] = static_cast<float>(v[0]);
    out.m[4] = static_cast<float>(v[1]);
    out.m[8] = static_cast<float>(v[2]);
    // Column 1 (Y axis).
    out.m[1] = static_cast<float>(v[3]);
    out.m[5] = static_cast<float>(v[4]);
    out.m[9] = static_cast<float>(v[5]);
    // Column 2 (Z axis).
    out.m[2] = static_cast<float>(v[6]);
    out.m[6] = static_cast<float>(v[7]);
    out.m[10] = static_cast<float>(v[8]);
    // Column 3 (translation).
    out.m[3] = static_cast<float>(v[9]);
    out.m[7] = static_cast<float>(v[10]);
    out.m[11] = static_cast<float>(v[11]);
    return out;
}
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

    // Walk every mesh-bearing node (a mesh can be shared by several nodes, that
    // is fine: each becomes a copy of the geometry at its transform).
    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        if (!node) continue;
        const ufbx_mesh* m = node->mesh;
        if (!m || m->vertices.count == 0 || m->vertex_indices.count == 0) continue;

        const uint32_t base = static_cast<uint32_t>(allVerts.size());
        for (size_t vi = 0; vi < m->vertices.count; ++vi) {
            const ufbx_vec3& p = m->vertices.data[vi];
            gfx::Vertex3D v;
            v.pos = {static_cast<float>(p.x), static_cast<float>(p.y),
                     static_cast<float>(p.z)};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {0.0f, 0.0f};
            // Per-vertex normal/uv are stored as a typed list with a per-vertex
            // index map; fall back to flat normals below when absent.
            if (m->vertex_normal.exists) {
                size_t mv = m->vertex_normal.indices.count > vi
                                ? m->vertex_normal.indices.data[vi]
                                : vi;
                if (mv < m->vertex_normal.values.count) {
                    const ufbx_vec3& nn = m->vertex_normal.values.data[mv];
                    v.normal = {static_cast<float>(nn.x), static_cast<float>(nn.y),
                                static_cast<float>(nn.z)};
                }
            }
            if (m->vertex_uv.exists) {
                size_t mu = m->vertex_uv.indices.count > vi ? m->vertex_uv.indices.data[vi] : vi;
                if (mu < m->vertex_uv.values.count) {
                    const ufbx_vec2& tuv = m->vertex_uv.values.data[mu];
                    v.uv = {static_cast<float>(tuv.x), static_cast<float>(tuv.y)};
                }
            }
            // Bake the node's world transform into the vertex position. The
            // ufbx matrix is a 3x4 double column-major array with translation
            // in the last column — convert it to the engine's Mat4 rather than
            // reinterpreting the memory (a reinterpret of a double/3x4 array
            // as a float/4x4 would scramble the transform).
            const neon::math::Mat4 nm = UfbxMatToEngine(node->node_to_world);
            v.pos = nm.TransformPoint(v.pos);
            allVerts.push_back(v);
        }
        // Copy triangle indices (sanitize strip-restart sentinels).
        for (size_t ii = 0; ii < m->vertex_indices.count; ++ii) {
            uint32_t idx = m->vertex_indices.data[ii];
            if (idx != 0xFFFFFFFFu && idx != 0xFFFFFFFEu) allIndices.push_back(base + idx);
        }
    }

    // Transform normals correctly: apply the normal matrix (upper-3x3 of the
    // node matrix) to each vertex's stored normal. Revisit per-vertex.
    // (Kept simple: recompute flat normals below from the triangles.)

    if (allVerts.empty() || allIndices.size() < 3) {
        ufbx_free_scene(scene);
        return out;
    }

    // Recompute flat normals (robust regardless of the source normal stream):
    // for each triangle accumulate its edge-cross into the three verts.
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
