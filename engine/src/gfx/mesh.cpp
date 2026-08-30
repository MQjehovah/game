#include "neon/gfx/mesh.hpp"

#include <cmath>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/terrain.hpp"

namespace neon::gfx {
namespace {

void PushFace(std::vector<Vertex3D>& verts, std::vector<uint16_t>& indices,
              const math::Vec3& normal, const math::Vec3& a, const math::Vec3& b,
              const math::Vec3& c, const math::Vec3& d,
              const math::Vec2& uvA = {0, 0}, const math::Vec2& uvB = {1, 0},
              const math::Vec2& uvC = {1, 1}, const math::Vec2& uvD = {0, 1},
              const math::Vec4& color = {1, 1, 1, 1}) {
    uint16_t base = static_cast<uint16_t>(verts.size());
    verts.push_back({a, normal, uvA, color});
    verts.push_back({b, normal, uvB, color});
    verts.push_back({c, normal, uvC, color});
    verts.push_back({d, normal, uvD, color});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

void AppendSphereVerts(std::vector<Vertex3D>& verts, std::vector<uint16_t>& indices,
                       float radius, int slices, int stacks) {
    auto push = [&](float phi, float theta) {
        float x = radius * std::sin(phi) * std::cos(theta);
        float y = radius * std::cos(phi);
        float z = radius * std::sin(phi) * std::sin(theta);
        math::Vec3 n = {x, y, z};
        n = n * (1.0f / radius);
        verts.push_back({n * radius, n, {theta / math::kTwoPi, phi / math::kPi}, {1, 1, 1, 1}});
    };
    for (int lat = 0; lat < stacks; ++lat) {
        float phi0 = static_cast<float>(lat) / stacks * math::kPi;
        float phi1 = static_cast<float>(lat + 1) / stacks * math::kPi;
        for (int lon = 0; lon < slices; ++lon) {
            float theta0 = static_cast<float>(lon) / slices * math::kTwoPi;
            float theta1 = static_cast<float>(lon + 1) / slices * math::kTwoPi;
            uint16_t v00 = static_cast<uint16_t>(verts.size());
            push(phi0, theta0);
            uint16_t v10 = static_cast<uint16_t>(verts.size());
            push(phi1, theta0);
            uint16_t v11 = static_cast<uint16_t>(verts.size());
            push(phi1, theta1);
            uint16_t v01 = static_cast<uint16_t>(verts.size());
            push(phi0, theta1);
            indices.insert(indices.end(), {v00, v10, v11, v00, v11, v01});
        }
    }
}

void AppendCylinderVerts(std::vector<Vertex3D>& verts, std::vector<uint16_t>& indices,
                         float radius, float height, int segments) {
    std::vector<uint16_t> topRing, bottomRing;
    for (int i = 0; i <= segments; ++i) {
        float a = static_cast<float>(i) / segments * math::kTwoPi;
        float x = std::cos(a), z = std::sin(a);
        math::Vec3 n{x, 0, z};
        math::Vec2 uv{static_cast<float>(i) / segments, 0.0f};
        bottomRing.push_back(static_cast<uint16_t>(verts.size()));
        verts.push_back({n * radius + math::Vec3{0, -height * 0.5f, 0}, n, uv, {1, 1, 1, 1}});
        topRing.push_back(static_cast<uint16_t>(verts.size()));
        verts.push_back({n * radius + math::Vec3{0, height * 0.5f, 0}, n, {uv.x, 1.0f}, {1, 1, 1, 1}});
    }
    for (int i = 0; i < segments; ++i) {
        indices.insert(indices.end(),
                       {bottomRing[i], topRing[i], topRing[i + 1],
                        bottomRing[i], topRing[i + 1], bottomRing[i + 1]});
    }

    // Caps
    uint16_t topCenter = static_cast<uint16_t>(verts.size());
    verts.push_back({{0, height * 0.5f, 0}, {0, 1, 0}, {0.5f, 0.5f}, {1, 1, 1, 1}});
    for (int i = 0; i < segments; ++i) {
        indices.insert(indices.end(), {topCenter, topRing[i + 1], topRing[i]});
    }
    uint16_t bottomCenter = static_cast<uint16_t>(verts.size());
    verts.push_back({{0, -height * 0.5f, 0}, {0, -1, 0}, {0.5f, 0.5f}, {1, 1, 1, 1}});
    for (int i = 0; i < segments; ++i) {
        indices.insert(indices.end(), {bottomCenter, bottomRing[i], bottomRing[i + 1]});
    }
}

} // namespace

Mesh Mesh::CreateCube(Renderer& renderer, float w, float h, float d, const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    float sx = w * 0.5f, sy = h * 0.5f, sz = d * 0.5f;
    PushFace(verts, indices, {1, 0, 0}, {sx, -sy, -sz}, {sx, sy, -sz}, {sx, sy, sz}, {sx, -sy, sz});
    PushFace(verts, indices, {-1, 0, 0}, {-sx, -sy, sz}, {-sx, sy, sz}, {-sx, sy, -sz}, {-sx, -sy, -sz});
    PushFace(verts, indices, {0, 1, 0}, {-sx, sy, -sz}, {-sx, sy, sz}, {sx, sy, sz}, {sx, sy, -sz});
    PushFace(verts, indices, {0, -1, 0}, {-sx, -sy, sz}, {-sx, -sy, -sz}, {sx, -sy, -sz}, {sx, -sy, sz});
    PushFace(verts, indices, {0, 0, 1}, {-sx, -sy, sz}, {sx, -sy, sz}, {sx, sy, sz}, {-sx, sy, sz});
    PushFace(verts, indices, {0, 0, -1}, {sx, -sy, -sz}, {-sx, -sy, -sz}, {-sx, sy, -sz}, {sx, sy, -sz});
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreateSphere(Renderer& renderer, float radius, int slices, int stacks,
                        const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    AppendSphereVerts(verts, indices, radius, slices, stacks);
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreatePlane(Renderer& renderer, float width, float depth, int tilesU, int tilesV,
                       const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    float hw = width * 0.5f, hd = depth * 0.5f;
    PushFace(verts, indices, {0, 1, 0},
             {-hw, 0, -hd}, {-hw, 0, hd}, {hw, 0, hd}, {hw, 0, -hd},
             {0, static_cast<float>(tilesV)}, {0, 0},
             {static_cast<float>(tilesU), 0}, {static_cast<float>(tilesU), static_cast<float>(tilesV)});
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreateQuad(Renderer& renderer, float width, float height, const std::string& name) {
    return CreateQuadUv(renderer, width, height, 0.0f, 0.0f, 1.0f, 1.0f, name);
}

Mesh Mesh::CreateQuadUv(Renderer& renderer, float width, float height,
                        float u0, float v0, float u1, float v1, const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    const float hw = width * 0.5f, hh = height * 0.5f;
    // XY plane, normal +Z. v = 0 maps to the TOP edge (+y): textures are
    // uploaded top-down (first row = top of the picture, flipVertically=0), so
    // sampling v=0 at the quad's top edge makes the picture appear upright in
    // the front view. (The previous mapping put v=0 at the bottom, which
    // rendered every top-down texture upside down for sprite entities.)
    PushFace(verts, indices, {0, 0, 1},
             {-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0},
             {u0, v1}, {u1, v1}, {u1, v0}, {u0, v0});
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreateCylinder(Renderer& renderer, float radius, float height, int segments,
                          const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    AppendCylinderVerts(verts, indices, radius, height, segments);
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreateTerrain(Renderer& renderer, int segments, float size,
                         const std::vector<float>& heights, float heightScale,
                         const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    const int cols = segments + 1;
    const float half = size * 0.5f;
    const float cell = size / static_cast<float>(segments);

    auto heightAt = [&](int row, int col) {
        if (heights.empty()) return 0.0f;
        size_t i = static_cast<size_t>(row) * cols + col;
        if (i >= heights.size()) return 0.0f;
        return heights[i] * heightScale;
    };

    for (int row = 0; row < cols; ++row) {
        for (int col = 0; col < cols; ++col) {
            float x = -half + col * cell;
            float z = -half + row * cell;
            float y = heightAt(row, col);
            // Flat normal from the two adjacent edge heights.
            float hR = heightAt(row, col + 1);
            float hD = heightAt(row + 1, col);
            math::Vec3 normal = math::Cross({cell, hR - y, 0.0f}, {0.0f, hD - y, cell}).Normalized();
            if (normal.y < 0.0f) normal = -normal;
            // G2-3: layer-blend by height + slope (grass -> dirt -> rock).
            const float slope = 1.0f - normal.y;
            const math::Vec4 color = TerrainLayerColor(y, heightScale, slope);
            verts.push_back({math::Vec3{x, y, z}, normal,
                             {x / cell, z / cell}, color});
        }
    }
    for (int row = 0; row < segments; ++row) {
        for (int col = 0; col < segments; ++col) {
            uint16_t a = static_cast<uint16_t>(row * cols + col);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>((row + 1) * cols + col + 1);
            uint16_t d = static_cast<uint16_t>((row + 1) * cols + col);
            indices.insert(indices.end(), {a, c, b, a, d, c});
        }
    }
    return CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                          indices.data(), static_cast<uint32_t>(indices.size()), name);
}

Mesh Mesh::CreateFromData(Renderer& renderer, const Vertex3D* vertices, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount,
                          const std::string& name) {
    Mesh mesh;
    mesh.data_ = std::make_shared<MeshData>();
    mesh.data_->name = name;
    mesh.data_->backend = renderer.Backend();
    mesh.data_->handle = renderer.Backend()->CreateMesh(vertices, vertexCount, indices, indexCount);
    mesh.data_->triangleCount = indexCount / 3;
    mesh.data_->cpuVerts.assign(vertices, vertices + vertexCount);
    mesh.data_->cpuIndices.assign(indices, indices + indexCount);
    // If skin data was attached before upload (edge case), bake it into the
    // CPU/GPU vertex copy so the buffer is always consistent.
    if (!mesh.data_->cpuJointIds.empty() && !mesh.data_->cpuVerts.empty())
        mesh.BakeSkinDataIntoVerts();
    math::AABB bounds;
    bounds.min = {1e30f, 1e30f, 1e30f};
    bounds.max = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vertexCount; ++i) bounds.Expand(vertices[i].pos);
    if (vertexCount == 0) bounds = {};
    mesh.data_->bounds = bounds;
    return mesh;
}

Mesh Mesh::CreateFromDataU32(Renderer& renderer, const Vertex3D* vertices,
                             uint32_t vertexCount, const uint32_t* indices,
                             uint32_t indexCount, const std::string& name) {
    Mesh mesh;
    mesh.data_ = std::make_shared<MeshData>();
    mesh.data_->name = name;
    mesh.data_->backend = renderer.Backend();
    mesh.data_->handle =
        renderer.Backend()->CreateMeshU32(vertices, vertexCount, indices, indexCount);
    mesh.data_->indexType = 1;
    mesh.data_->triangleCount = indexCount / 3;
    mesh.data_->cpuVerts.assign(vertices, vertices + vertexCount);
    mesh.data_->cpuIndicesU32.assign(indices, indices + indexCount);
    math::AABB bounds;
    bounds.min = {1e30f, 1e30f, 1e30f};
    bounds.max = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vertexCount; ++i) bounds.Expand(vertices[i].pos);
    if (vertexCount == 0) bounds = {};
    mesh.data_->bounds = bounds;
    return mesh;
}

void Mesh::AttachSkinData(std::vector<uint16_t> jointIds, std::vector<float> jointWeights,
                          int skinIndex) {
    if (!data_) return;
    data_->cpuJointIds = std::move(jointIds);
    data_->cpuJointWeights = std::move(jointWeights);
    data_->skinIndex = skinIndex;
    data_->skinned = !data_->cpuJointIds.empty();
    if (data_->cpuVerts.empty() || !data_->backend) return;
    BakeSkinDataIntoVerts();
    data_->backend->UpdateMeshVertices(data_->handle, data_->cpuVerts.data(),
                                       static_cast<uint32_t>(data_->cpuVerts.size()));
}

void Mesh::BakeSkinDataIntoVerts() {
    if (!data_ || data_->cpuVerts.empty()) return;
    const size_t jointCount = data_->cpuJointIds.size();
    const size_t weightCount = data_->cpuJointWeights.size();
    for (size_t i = 0; i < data_->cpuVerts.size(); ++i) {
        Vertex3D& v = data_->cpuVerts[i];
        for (int c = 0; c < 4; ++c) {
            const size_t idx = i * 4 + static_cast<size_t>(c);
            v.j[c] = idx < jointCount ? static_cast<float>(data_->cpuJointIds[idx]) : 0.0f;
            v.w[c] = idx < weightCount ? data_->cpuJointWeights[idx] : 0.0f;
        }
    }
}

} // namespace neon::gfx
