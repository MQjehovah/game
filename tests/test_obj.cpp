#include <cmath>
#include <string>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// assets::AssetManager::LoadMeshOBJ
// ---------------------------------------------------------------------------

// Self-contained OBJ: MTL Kd vertex colors, two usemtl material groups, and
// flat-normal fallback for faces that carry no vertex normals.
TEST(ObjMaterialGroupsAndFlatNormals) {
    test::TempDir tmp;
    const std::string objPath = tmp.Str() + "/scene.obj";
    const std::string mtlPath = tmp.Str() + "/test.mtl";

    CHECK(test::WriteFileAll(
        mtlPath,
        "newmtl red\n"
        "Kd 0.8 0.1 0.1\n"
        "\n"
        "newmtl blue\n"
        "Kd 0.1 0.2 0.9\n"));
    CHECK(test::WriteFileAll(
        objPath,
        "mtllib test.mtl\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "usemtl red\n"
        "f 1 2 3\n"
        "usemtl blue\n"
        "f 1 3 4\n"));

    test::HeadlessAssetFixture fix;
    gfx::Mesh mesh = fix.assets.LoadMeshOBJ(objPath);
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 6u);
    CHECK_EQ(mesh.CpuIndices().size(), 6u);
    CHECK_EQ(mesh.TriangleCount(), 2u);

    // Group "red": Kd color applied as vertex color.
    const gfx::Vertex3D& vr = mesh.CpuVerts()[0];
    CHECK_NEAR(vr.color.x, 0.8, 1e-5);
    CHECK_NEAR(vr.color.y, 0.1, 1e-5);
    CHECK_NEAR(vr.color.z, 0.1, 1e-5);

    // No vn in the file: flat normal computed from the face (cross of edges).
    CHECK_NEAR(vr.normal.x, 0.0, 1e-5);
    CHECK_NEAR(vr.normal.y, 0.0, 1e-5);
    CHECK_NEAR(vr.normal.z, 1.0, 1e-5);

    // Group "blue": a different material color.
    const gfx::Vertex3D& vb = mesh.CpuVerts()[3];
    CHECK_NEAR(vb.color.x, 0.1, 1e-5);
    CHECK_NEAR(vb.color.y, 0.2, 1e-5);
    CHECK_NEAR(vb.color.z, 0.9, 1e-5);

    // Fan triangulation: face A = 0,1,2 ; face B = 3,4,5.
    CHECK_EQ(mesh.CpuIndices()[0], 0u);
    CHECK_EQ(mesh.CpuIndices()[1], 1u);
    CHECK_EQ(mesh.CpuIndices()[2], 2u);
    CHECK_EQ(mesh.CpuIndices()[3], 3u);
    CHECK_EQ(mesh.CpuIndices()[4], 4u);
    CHECK_EQ(mesh.CpuIndices()[5], 5u);
}

// Vertex normals from vn are used when the face provides them; UVs are mapped.
TEST(ObjVertexNormalsAndUvsUsed) {
    test::TempDir tmp;
    const std::string objPath = tmp.Str() + "/normals.obj";
    CHECK(test::WriteFileAll(
        objPath,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vn 0 0 -1\n"
        "vt 0.5 0.25\n"
        "f 1/1/1 2/1/1 3/1/1\n"));

    test::HeadlessAssetFixture fix;
    gfx::Mesh mesh = fix.assets.LoadMeshOBJ(objPath);
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_EQ(mesh.TriangleCount(), 1u);

    const gfx::Vertex3D& v = mesh.CpuVerts()[0];
    CHECK_NEAR(v.normal.x, 0.0, 1e-5);
    CHECK_NEAR(v.normal.y, 0.0, 1e-5);
    CHECK_NEAR(v.normal.z, -1.0, 1e-5); // from vn, not the flat fallback
    CHECK_NEAR(v.uv.x, 0.5, 1e-5);
    CHECK_NEAR(v.uv.y, 0.25, 1e-5);
    // No mtllib: default white vertex color.
    CHECK_NEAR(v.color.x, 1.0, 1e-6);
    CHECK_NEAR(v.color.y, 1.0, 1e-6);
    CHECK_NEAR(v.color.z, 1.0, 1e-6);
}

// Negative indices resolve relative to the current vertex pool.
TEST(ObjNegativeIndices) {
    test::TempDir tmp;
    const std::string objPath = tmp.Str() + "/neg.obj";
    CHECK(test::WriteFileAll(
        objPath,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "f -3 -2 -1\n"));

    test::HeadlessAssetFixture fix;
    gfx::Mesh mesh = fix.assets.LoadMeshOBJ(objPath);
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_NEAR(mesh.CpuVerts()[0].pos.x, 0.0, 1e-6);
    CHECK_NEAR(mesh.CpuVerts()[1].pos.x, 1.0, 1e-6);
    CHECK_NEAR(mesh.CpuVerts()[2].pos.x, 1.0, 1e-6);
}

// Real Kenney asset: mtllib + two usemtl groups (grass / colorRed), CRLF line
// endings and scientific-notation floats. Verifies both MTL colors appear in
// the imported vertex data.
TEST(ObjKenneyAssetParses) {
    const char* kenney = "assets/kenney_nature/Models/OBJ format/flower_redA.obj";
    test::HeadlessAssetFixture fix;
    gfx::Mesh mesh = fix.assets.LoadMeshOBJ(kenney);
    CHECK(mesh.Valid());
    CHECK(mesh.TriangleCount() > 0u);

    const std::vector<gfx::Vertex3D>& verts = mesh.CpuVerts();
    CHECK_EQ(verts.size(), mesh.CpuIndices().size());
    CHECK_EQ(mesh.CpuIndices().size(), mesh.TriangleCount() * 3u);

    // grass: Kd 0.172549 0.8470588 0.7215686 ; colorRed: Kd 0.8784314 0.2901961 0.3137255
    bool foundGrass = false;
    bool foundRed = false;
    for (const gfx::Vertex3D& v : verts) {
        if (std::fabs(v.color.x - 0.172549f) < 0.02f &&
            std::fabs(v.color.y - 0.8470588f) < 0.02f)
            foundGrass = true;
        if (std::fabs(v.color.x - 0.8784314f) < 0.02f &&
            std::fabs(v.color.y - 0.2901961f) < 0.02f)
            foundRed = true;
    }
    CHECK(foundGrass);
    CHECK(foundRed);
}
