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
    // GPU skinning data: up to 4 joint indices + 4 weights per vertex. Stored
    // as plain floats (not integer attribs) so one VBO layout serves both
    // static and skinned meshes; the skinned lit shader casts them to int.
    // Zero for non-skinned vertices.
    float j[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Byte stride of the fixed 3D vertex layout. The GL backend mirrors this in
// CreateMesh/UpdateMeshVertices; keep them in sync if Vertex3D changes.
static_assert(sizeof(Vertex3D) == 80, "Vertex3D layout changed; update GL backend stride/offsets");

// Engine skin definition (CPU side): the joint chain of a glTF skin, expressed
// as glTF node indices, plus one inverse-bind matrix per joint.
struct Skin {
    std::vector<uint32_t> joints;
    std::vector<math::Mat4> inverseBind;
};

struct MeshData {
    MeshHandle handle;
    math::AABB bounds;
    std::string name;
    uint32_t triangleCount = 0;
    std::vector<Vertex3D> cpuVerts;
    std::vector<uint16_t> cpuIndices;
    // Skinned-mesh CPU data: 4 joints + 4 weights per vertex, tightly packed.
    bool skinned = false;
    int skinIndex = -1;
    std::vector<uint16_t> cpuJointIds;
    std::vector<float> cpuJointWeights;
    // Backend used to (re)upload vertex data when skin data is attached after
    // CreateFromData. The renderer outlives the meshes in every consumer, so a
    // raw pointer is safe for the lifetime the mesh actually uses it.
    IRenderBackend* backend = nullptr;
};

class Mesh {
public:
    Mesh() = default;

    static Mesh CreateCube(Renderer& renderer, float w, float h, float d, const std::string& name = "cube");
    static Mesh CreateSphere(Renderer& renderer, float radius, int slices = 16, int stacks = 10,
                             const std::string& name = "sphere");
    static Mesh CreatePlane(Renderer& renderer, float width, float depth, int tilesU = 1, int tilesV = 1,
                            const std::string& name = "plane");
    // XY-plane quad facing +Z (the editor's front-ortho camera looks down -Z),
    // unit-sized UVs so an image texture maps 1:1. Used by 2D sprites.
    static Mesh CreateQuad(Renderer& renderer, float width, float height,
                           const std::string& name = "quad");
    // Same quad but samples only the [u0,u1]x[v0,v1] sub-rectangle of the
    // texture (spritesheet atlas frames).
    static Mesh CreateQuadUv(Renderer& renderer, float width, float height,
                             float u0, float v0, float u1, float v1,
                             const std::string& name = "quad_uv");
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
    // Accessors are null-safe: an invalid (default-constructed) mesh returns
    // empty/invalid values instead of crashing, mirroring TriangleCount().
    const MeshHandle& Handle() const {
        static const MeshHandle kInvalid;
        return data_ ? data_->handle : kInvalid;
    }
    const math::AABB& Bounds() const {
        static const math::AABB kEmpty;
        return data_ ? data_->bounds : kEmpty;
    }
    const std::string& Name() const {
        static const std::string kEmpty;
        return data_ ? data_->name : kEmpty;
    }
    uint32_t TriangleCount() const { return data_ ? data_->triangleCount : 0; }
    const std::vector<Vertex3D>& CpuVerts() const {
        static const std::vector<Vertex3D> kEmpty;
        return data_ ? data_->cpuVerts : kEmpty;
    }
    const std::vector<uint16_t>& CpuIndices() const {
        static const std::vector<uint16_t> kEmpty;
        return data_ ? data_->cpuIndices : kEmpty;
    }
    bool Skinned() const { return data_ ? data_->skinned : false; }
    int SkinIndex() const { return data_ ? data_->skinIndex : -1; }
    const std::vector<uint16_t>& CpuJointIds() const {
        static const std::vector<uint16_t> kEmpty;
        return data_ ? data_->cpuJointIds : kEmpty;
    }
    const std::vector<float>& CpuJointWeights() const {
        static const std::vector<float> kEmpty;
        return data_ ? data_->cpuJointWeights : kEmpty;
    }
    // Attaches skinned per-vertex data (4 joints + 4 weights per vertex) to a
    // mesh already built from geometry. Bakes the data into the mesh's CPU
    // vertex copy and re-uploads the vertex buffer so the GPU layout matches;
    // flags the mesh as skinned. See mesh.cpp.
    void AttachSkinData(std::vector<uint16_t> jointIds, std::vector<float> jointWeights,
                        int skinIndex = -1);

private:
    // Copies cpuJointIds/cpuJointWeights into cpuVerts[j]/[w] (4+4 floats per
    // vertex) so the CPU mirror matches the GPU vertex layout exactly.
    void BakeSkinDataIntoVerts();
    std::shared_ptr<MeshData> data_;
};

// Level-of-detail chain for a renderable: `levels[0]` is the highest-detail
// mesh and `levels[i]` a coarser stand-in. `thresholds[i]` is the camera
// distance at which level `i` is swapped out for `i + 1`, so level i is active
// while distance < thresholds[i] and the last level covers everything beyond
// the final threshold. Thresholds are strictly increasing (enforced by the
// scene factory); PickLod falls back to the highest level for any distance.
// An empty `levels` marks a missing chain (PickLod returns -1).
struct LodChain {
    std::vector<Mesh> levels;
    std::vector<float> thresholds;
};

// Returns the active level index for `distance`, or -1 for an empty chain.
// Pure + header-only so renderers and tests share the same selection math.
inline int PickLod(const LodChain& chain, float distance) {
    if (chain.levels.empty()) return -1;
    for (size_t i = 0; i < chain.thresholds.size(); ++i) {
        if (distance < chain.thresholds[i])
            return static_cast<int>(i < chain.levels.size() ? i : chain.levels.size() - 1);
    }
    return static_cast<int>(chain.levels.size() - 1);
}

} // namespace neon::gfx
