#include "neon/gfx/scene_props.hpp"

#include <cmath>

#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {
namespace {

void RecolorVerts(std::vector<Vertex3D>& verts, const math::Vec4& c) {
    for (auto& v : verts) v.color = c;
}

void AppendMesh(std::vector<Vertex3D>& verts, std::vector<uint16_t>& idx,
                const std::vector<Vertex3D>& add, const std::vector<uint16_t>& addIdx) {
    const uint16_t base = static_cast<uint16_t>(verts.size());
    verts.insert(verts.end(), add.begin(), add.end());
    for (uint16_t i : addIdx) idx.push_back(static_cast<uint16_t>(base + i));
}

// Y-up cone with apex at y=height and base ring at y=0 (OpenGL-style winding).
std::vector<Vertex3D> ConeVerts(float radius, float height, int seg, const math::Vec4& c) {
    std::vector<Vertex3D> v;
    v.reserve(static_cast<size_t>(seg) + 1);
    const float l = std::sqrt(radius * radius + height * height);
    const float nY = radius / l;   // side-normal Y (up)
    const float nXZ = height / l;  // side-normal horizontal magnitude
    v.push_back({{0.0f, height, 0.0f}, {0.0f, nY, 0.0f}, {0.5f, 1.0f}, c});
    for (int i = 0; i < seg; ++i) {
        const float a = static_cast<float>(i) / seg * math::kTwoPi;
        const float x = std::cos(a) * radius;
        const float z = std::sin(a) * radius;
        v.push_back({{x, 0.0f, z}, {std::cos(a) * nXZ, nY, std::sin(a) * nXZ},
                     {a / math::kTwoPi, 0.0f}, c});
    }
    return v;
}

std::vector<uint16_t> ConeIndices(int seg) {
    std::vector<uint16_t> idx;
    idx.reserve(static_cast<size_t>(seg) * 3);
    for (int i = 0; i < seg; ++i)
        idx.insert(idx.end(), {0, static_cast<uint16_t>(1 + i),
                               static_cast<uint16_t>(1 + (i + 1) % seg)});
    return idx;
}

} // namespace

Mesh MakeTerrainMesh(Renderer& renderer, int segments, float size, const std::string& name) {
    std::vector<float> heights(static_cast<size_t>(segments + 1) * (segments + 1), 0.0f);
    const float half = size * 0.5f;
    const float cell = size / static_cast<float>(segments);
    for (int row = 0; row <= segments; ++row) {
        for (int col = 0; col <= segments; ++col) {
            const float x = -half + col * cell;
            const float z = -half + row * cell;
            float h = std::sin(x * 0.11f) * std::cos(z * 0.13f) * 0.8f +
                      std::sin(x * 0.31f + z * 0.27f) * 0.35f;
            const float d = std::sqrt(x * x + z * z);
            h *= math::Saturate((d - 6.0f) / 10.0f); // flatten the centre
            // Shallow lake in the SW corner.
            const float lakeD = std::sqrt((x + 18.0f) * (x + 18.0f) + (z + 18.0f) * (z + 18.0f));
            if (lakeD < 10.0f) {
            const float t = 1.0f - lakeD / 10.0f;
            h = std::fmin(h, -1.6f * t * t);
            }
            heights[static_cast<size_t>(row) * (segments + 1) + col] = h;
        }
    }
    return Mesh::CreateTerrain(renderer, segments, size, heights, 1.0f, name);
}

Mesh MakeTreeMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    Mesh trunk = Mesh::CreateCube(renderer, 0.30f, 1.3f, 0.30f, "trunk");
    std::vector<Vertex3D> tv = trunk.CpuVerts();
    RecolorVerts(tv, {0.45f, 0.32f, 0.20f, 1.0f});
    for (auto& vv : tv) vv.pos.y += 0.65f; // trunk base sits on the ground
    AppendMesh(v, idx, tv, trunk.CpuIndices());
    auto c1 = ConeVerts(1.15f, 1.5f, 14, {0.16f, 0.48f, 0.16f, 1.0f});
    for (auto& vv : c1) vv.pos.y += 1.3f;
    AppendMesh(v, idx, c1, ConeIndices(14));
    auto c2 = ConeVerts(0.80f, 1.15f, 14, {0.20f, 0.55f, 0.19f, 1.0f});
    for (auto& vv : c2) vv.pos.y += 2.35f;
    AppendMesh(v, idx, c2, ConeIndices(14));
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeHouseMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    Mesh body = Mesh::CreateCube(renderer, 3.4f, 2.0f, 2.6f, "body");
    std::vector<Vertex3D> bv = body.CpuVerts();
    RecolorVerts(bv, {0.72f, 0.62f, 0.48f, 1.0f});
    for (auto& vv : bv) vv.pos.y += 1.0f;
    AppendMesh(v, idx, bv, body.CpuIndices());
    auto roof = ConeVerts(2.6f, 1.5f, 4, {0.55f, 0.32f, 0.22f, 1.0f});
    for (auto& vv : roof) vv.pos.y += 2.0f;
    AppendMesh(v, idx, roof, ConeIndices(4));
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeNPCMesh(Renderer& renderer, const math::Vec4& tunic, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    Mesh body = Mesh::CreateCube(renderer, 0.6f, 0.9f, 0.4f, "npc_body");
    std::vector<Vertex3D> bv = body.CpuVerts();
    RecolorVerts(bv, tunic);
    for (auto& vv : bv) vv.pos.y += 0.45f;
    AppendMesh(v, idx, bv, body.CpuIndices());
    Mesh head = Mesh::CreateSphere(renderer, 0.26f, 12, 8, "npc_head");
    std::vector<Vertex3D> hv = head.CpuVerts();
    RecolorVerts(hv, {0.85f, 0.72f, 0.60f, 1.0f});
    for (auto& vv : hv) vv.pos.y += 1.35f;
    AppendMesh(v, idx, hv, head.CpuIndices());
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeBushMesh(Renderer& renderer, const std::string& name) {
    Mesh bush = Mesh::CreateSphere(renderer, 0.7f, 12, 8, "bush");
    std::vector<Vertex3D> v = bush.CpuVerts();
    RecolorVerts(v, {0.22f, 0.50f, 0.20f, 1.0f});
    for (auto& vv : v) vv.pos.y *= 0.7f; // squash into a low bush
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                bush.CpuIndices().data(),
                                static_cast<uint32_t>(bush.CpuIndices().size()), name);
}

} // namespace neon::gfx
