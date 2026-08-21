#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Task 3.3: GPU skinning in the lit shader
// ---------------------------------------------------------------------------

namespace {

// NullBackend that records the bone-matrix array uploads issued by
// Renderer::DrawSkinnedMesh, so the headless path can assert on them.
class RecordingBackend : public test::NullBackend {
public:
    int boneArrayCalls = 0;
    int mat4ArrayCalls = 0;
    std::string lastBoneName;
    int lastBoneCount = 0;
    std::vector<float> lastBoneValues;

    void SetUniformMat4Array(const char* name, const float* values, int count) override {
        ++mat4ArrayCalls;
        if (name && std::string(name) == "uBoneMatrices") {
            ++boneArrayCalls;
            lastBoneName = name;
            lastBoneCount = count;
            lastBoneValues.assign(values, values + static_cast<size_t>(count) * 16);
        }
    }
};

// Two-bone flag-style skeleton: root pinned, child rotates around local Z.
anim::Skeleton MakeFlagSkeleton() {
    anim::Skeleton sk;
    sk.bones.resize(2);
    sk.bones[0].name = "flagRoot";
    sk.bones[0].parent = -1;
    sk.bones[0].inverseBind = math::Mat4::Identity();
    sk.bones[1].name = "flagTop";
    sk.bones[1].parent = 0;
    sk.bones[1].inverseBind = math::Mat4::Identity();
    return sk;
}

gfx::Mesh MakeSkinnedMesh(gfx::Renderer& renderer) {
    std::vector<gfx::Vertex3D> verts;
    std::vector<uint16_t> indices;
    std::vector<uint16_t> jointIds;
    std::vector<float> jointWeights;
    const int kCols = 4;
    const int kRows = 4;
    for (int r = 0; r <= kRows; ++r) {
        float v = static_cast<float>(r) / kRows;
        for (int c = 0; c <= kCols; ++c) {
            float u = static_cast<float>(c) / kCols;
            gfx::Vertex3D vert;
            vert.pos = {u, v, 0.0f};
            vert.normal = {0, 0, 1};
            vert.uv = {u, v};
            vert.j[0] = 0.0f;
            vert.j[1] = 1.0f;
            vert.w[0] = 1.0f - v;
            vert.w[1] = v;
            verts.push_back(vert);
            jointIds.insert(jointIds.end(), {0, 1, 0, 0});
            jointWeights.insert(jointWeights.end(), {1.0f - v, v, 0.0f, 0.0f});
        }
    }
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            uint16_t a = static_cast<uint16_t>(r * (kCols + 1) + c);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t d = static_cast<uint16_t>(a + kCols + 1);
            uint16_t e = static_cast<uint16_t>(d + 1);
            indices.insert(indices.end(), {a, b, d, b, e, d});
        }
    }
    gfx::Mesh mesh = gfx::Mesh::CreateFromData(renderer, verts.data(),
                                               static_cast<uint32_t>(verts.size()),
                                               indices.data(),
                                               static_cast<uint32_t>(indices.size()), "flag");
    mesh.AttachSkinData(std::move(jointIds), std::move(jointWeights), 0);
    return mesh;
}

} // namespace

// The fixed 3D vertex layout grew skin data: 80 bytes, joints at 48, weights
// at 64. The GL backend mirrors these offsets/stride in CreateMesh.
TEST(SkinnedVertexLayout) {
    CHECK_EQ(sizeof(gfx::Vertex3D), 80u);
    CHECK_EQ(offsetof(gfx::Vertex3D, pos), 0u);
    CHECK_EQ(offsetof(gfx::Vertex3D, normal), 12u);
    CHECK_EQ(offsetof(gfx::Vertex3D, uv), 24u);
    CHECK_EQ(offsetof(gfx::Vertex3D, color), 32u);
    CHECK_EQ(offsetof(gfx::Vertex3D, j), 48u);
    CHECK_EQ(offsetof(gfx::Vertex3D, w), 64u);
}

// CreateFromData with joint/weight data pre-filled in the vertices, plus
// AttachSkinData, must keep the CPU vertex mirror (what is uploaded) in sync
// with the recorded joint/weight arrays.
TEST(SkinnedCreateFromDataBakesJointsWeights) {
    test::HeadlessAssetFixture fix;
    gfx::Mesh mesh = MakeSkinnedMesh(fix.renderer);

    CHECK(mesh.Valid());
    CHECK(mesh.Skinned());
    CHECK_EQ(mesh.SkinIndex(), 0);
    CHECK_EQ(mesh.CpuVerts().size(), 25u);
    CHECK_EQ(mesh.CpuJointIds().size(), 100u);
    CHECK_EQ(mesh.CpuJointWeights().size(), 100u);

    // A bottom vertex (weight 1 -> joint 0) and a top vertex (weight 1 -> joint 1).
    const gfx::Vertex3D& bottom = mesh.CpuVerts()[0];
    CHECK_EQ(bottom.j[0], 0.0f);
    CHECK_EQ(bottom.j[1], 1.0f);
    CHECK_NEAR(bottom.w[0], 1.0f, 1e-6);
    CHECK_NEAR(bottom.w[1], 0.0f, 1e-6);

    const gfx::Vertex3D& top = mesh.CpuVerts()[20];
    CHECK_NEAR(top.w[0], 0.0f, 1e-6);
    CHECK_NEAR(top.w[1], 1.0f, 1e-6);

    // Mid vertex: 50/50 blend between the two joints, j = [0,1,0,0].
    const gfx::Vertex3D& mid = mesh.CpuVerts()[10];
    CHECK_EQ(mid.j[0], 0.0f);
    CHECK_EQ(mid.j[1], 1.0f);
    CHECK_EQ(mid.j[2], 0.0f);
    CHECK_EQ(mid.j[3], 0.0f);
    CHECK_NEAR(mid.w[0], 0.5f, 1e-6);
    CHECK_NEAR(mid.w[1], 0.5f, 1e-6);

    // The CPU joint/weight arrays round-trip the vertex data.
    CHECK_EQ(mesh.CpuJointIds()[10 * 4 + 0], 0u);
    CHECK_EQ(mesh.CpuJointIds()[10 * 4 + 1], 1u);
    CHECK_NEAR(mesh.CpuJointWeights()[10 * 4 + 0], 0.5f, 1e-6);
    CHECK_NEAR(mesh.CpuJointWeights()[10 * 4 + 1], 0.5f, 1e-6);
}

// Attaching skin data to a mesh that was uploaded with zeroed joint/weight
// slots must bake the data into the CPU mirror (and re-upload the VBO through
// the backend), so the GPU layout always matches.
TEST(SkinnedAttachSkinDataBakesCpuMirror) {
    test::HeadlessAssetFixture fix;
    std::vector<gfx::Vertex3D> verts;
    std::vector<uint16_t> indices;
    for (int i = 0; i < 4; ++i) {
        gfx::Vertex3D v;
        v.pos = {static_cast<float>(i), 0, 0};
        verts.push_back(v);
    }
    indices = {0, 1, 2, 0, 2, 3};
    gfx::Mesh mesh = gfx::Mesh::CreateFromData(fix.renderer, verts.data(),
                                               static_cast<uint32_t>(verts.size()),
                                               indices.data(),
                                               static_cast<uint32_t>(indices.size()), "skin");
    CHECK(!mesh.Skinned());
    CHECK_NEAR(mesh.CpuVerts()[1].w[1], 0.0f, 1e-6); // zeroed before attach

    std::vector<uint16_t> jointIds(4 * 4, 0);
    std::vector<float> jointWeights(4 * 4, 0.0f);
    for (int i = 0; i < 4; ++i) {
        jointIds[static_cast<size_t>(i) * 4 + 0] = 0;
        jointIds[static_cast<size_t>(i) * 4 + 1] = 1;
        jointWeights[static_cast<size_t>(i) * 4 + 0] = 1.0f - i * 0.2f;
        jointWeights[static_cast<size_t>(i) * 4 + 1] = i * 0.2f;
    }
    mesh.AttachSkinData(std::move(jointIds), std::move(jointWeights), 0);

    CHECK(mesh.Skinned());
    CHECK_EQ(mesh.SkinIndex(), 0);
    const gfx::Vertex3D& v1 = mesh.CpuVerts()[1];
    CHECK_EQ(v1.j[0], 0.0f);
    CHECK_EQ(v1.j[1], 1.0f);
    CHECK_NEAR(v1.w[0], 0.8f, 1e-6);
    CHECK_NEAR(v1.w[1], 0.2f, 1e-6);
}

// DrawSkinnedMesh on the headless backend must not crash and must upload the
// bone matrices as a contiguous row-major mat4 array.
TEST(SkinnedDrawUploadsBoneMatrices) {
    gfx::Renderer renderer;
    auto recorder = std::make_unique<RecordingBackend>();
    RecordingBackend* rec = recorder.get();
    renderer.AttachBackendForTesting(std::move(recorder));

    gfx::Mesh mesh = MakeSkinnedMesh(renderer);
    std::vector<math::Mat4> bones(2);
    bones[0] = math::Mat4::Identity();
    bones[1] = math::Mat4::RotationZ(0.5f);

    renderer.DrawSkinnedMesh(mesh, gfx::Material::Lit({}, gfx::Color::White, 16.0f),
                             math::Mat4::Identity(), bones, 2);

    CHECK_EQ(rec->boneArrayCalls, 1);
    CHECK_EQ(rec->lastBoneName, "uBoneMatrices");
    CHECK_EQ(rec->lastBoneCount, 2);
    CHECK_EQ(rec->lastBoneValues.size(), 32u);
    if (rec->lastBoneValues.size() == 32u) {
        // Matrix 0 = identity.
        CHECK_NEAR(rec->lastBoneValues[0], 1.0f, 1e-6);
        CHECK_NEAR(rec->lastBoneValues[5], 1.0f, 1e-6);
        CHECK_NEAR(rec->lastBoneValues[10], 1.0f, 1e-6);
        CHECK_NEAR(rec->lastBoneValues[15], 1.0f, 1e-6);
        // Matrix 1 = RotationZ(0.5), row-major.
        CHECK_NEAR(rec->lastBoneValues[16], std::cos(0.5f), 1e-6);
        CHECK_NEAR(rec->lastBoneValues[16 + 1], -std::sin(0.5f), 1e-6);
        CHECK_NEAR(rec->lastBoneValues[16 + 4], std::sin(0.5f), 1e-6);
        CHECK_NEAR(rec->lastBoneValues[16 + 5], std::cos(0.5f), 1e-6);
    }

    // DrawSkinnedMesh on a non-skinned mesh is a safe no-op-ish call too.
    renderer.DrawSkinnedMesh(mesh, gfx::Material::Lit({}), math::Mat4::Identity(), bones, 0);
}

// The skin-matrix math the shader performs must match anim::Skeleton: given a
// pose, ComputeBoneMatrices yields per-joint matrices that blend vertex
// positions exactly as the GLSL code does (sum over weight_i * uBoneMatrices[id_i]).
TEST(SkinnedShaderMathMatchesAnimSkeleton) {
    anim::Skeleton sk = MakeFlagSkeleton();
    anim::Pose pose = sk.BindPose();
    const float theta = 0.5f;
    // FromEuler(yaw, pitch, roll) = (Z, Y, X); a Z rotation waves the flag
    // side-to-side in the X/Y plane, matching the demo.
    pose.r[1] = math::Quat::FromEuler(theta, 0.0f, 0.0f);
    std::vector<math::Mat4> bones = sk.ComputeBoneMatrices(pose);
    CHECK_EQ(bones.size(), 2u);
    if (bones.size() != 2u) return;

    // Bone 0 stays at the bind identity; bone 1 is the child Z rotation.
    CHECK_NEAR(bones[0].m[0], 1.0f, 1e-5);
    CHECK_NEAR(bones[0].m[5], 1.0f, 1e-5);
    CHECK_NEAR(bones[1].m[0], std::cos(theta), 1e-5);
    CHECK_NEAR(bones[1].m[1], -std::sin(theta), 1e-5);
    CHECK_NEAR(bones[1].m[4], std::sin(theta), 1e-5);
    CHECK_NEAR(bones[1].m[5], std::cos(theta), 1e-5);

    // Top vertex: weight 1 to bone 1 -> pure rotation about the origin.
    math::Vec3 pTop{0.0f, 2.0f, 0.0f};
    math::Vec4 sTop = bones[1].TransformVec4({pTop.x, pTop.y, pTop.z, 1.0f});
    CHECK_NEAR(sTop.x, -2.0f * std::sin(theta), 1e-5);
    CHECK_NEAR(sTop.y, 2.0f * std::cos(theta), 1e-5);
    CHECK_NEAR(sTop.z, 0.0f, 1e-5);

    // Mid vertex: 50/50 blend between the static and rotating bones.
    math::Vec3 pMid{0.0f, 1.0f, 0.0f};
    math::Vec4 w0 = bones[0].TransformVec4({pMid.x, pMid.y, pMid.z, 1.0f});
    math::Vec4 w1 = bones[1].TransformVec4({pMid.x, pMid.y, pMid.z, 1.0f});
    math::Vec4 blended = w0 * 0.5f + w1 * 0.5f;
    CHECK_NEAR(blended.x, -0.5f * std::sin(theta), 1e-5);
    CHECK_NEAR(blended.y, 0.5f * (1.0f + std::cos(theta)), 1e-5);
    CHECK_NEAR(blended.z, 0.0f, 1e-5);

    // Bottom vertex: weight 1 to bone 0 -> unchanged.
    math::Vec4 sBottom = bones[0].TransformVec4({0.0f, 0.0f, 0.0f, 1.0f});
    CHECK_NEAR(sBottom.x, 0.0f, 1e-6);
    CHECK_NEAR(sBottom.y, 0.0f, 1e-6);
}
