#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/math/math.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

class Renderer;

struct Vertex3D {
    math::Vec3 pos;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct MeshData {
    MeshHandle handle;
    math::AABB bounds;
    std::string name;
    uint32_t triangleCount = 0;
    std::vector<Vertex3D> cpuVerts;
    std::vector<uint16_t> cpuIndices;
};

class Mesh {
public:
    Mesh() = default;

    static Mesh CreateCube(Renderer& renderer, float w, float h, float d, const std::string& name = "cube");
    static Mesh CreateSphere(Renderer& renderer, float radius, int slices = 16, int stacks = 10,
                             const std::string& name = "sphere");
    static Mesh CreatePlane(Renderer& renderer, float width, float depth, int tilesU = 1, int tilesV = 1,
                            const std::string& name = "plane");
    static Mesh CreateCylinder(Renderer& renderer, float radius, float height, int segments = 20,
                               const std::string& name = "cylinder");
    // Heightfield terrain: segments x segments cells spanning size x size,
    // heights[row * (segments + 1) + col], scaled by heightScale, Y up.
    static Mesh CreateTerrain(Renderer& renderer, int segments, float size,
                              const std::vector<float>& heights, float heightScale,
                              const std::string& name = "terrain");
    static Mesh CreateFromData(Renderer& renderer, const Vertex3D* vertices, uint32_t vertexCount,
                               const uint16_t* indices, uint32_t indexCount,
                               const std::string& name = "mesh");

    bool Valid() const { return data_ && data_->handle.Valid(); }
    const MeshHandle& Handle() const { return data_->handle; }
    const math::AABB& Bounds() const { return data_->bounds; }
    const std::string& Name() const { return data_->name; }
    uint32_t TriangleCount() const { return data_ ? data_->triangleCount : 0; }
    const std::vector<Vertex3D>& CpuVerts() const { return data_->cpuVerts; }
    const std::vector<uint16_t>& CpuIndices() const { return data_->cpuIndices; }

private:
    std::shared_ptr<MeshData> data_;
};

} // namespace neon::gfx
