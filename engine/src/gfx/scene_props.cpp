#include "neon/gfx/scene_props.hpp"

#include <array>
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
        // Front face = CCW when viewed from outside: apex -> base[i+1] -> base[i].
        // The previous order was clockwise from outside, so every cone side
        // (roofs, tree crowns) was back-face culled and only the apex showed.
        idx.insert(idx.end(), {0, static_cast<uint16_t>(1 + (i + 1) % seg),
                               static_cast<uint16_t>(1 + i)});
    return idx;
}

// Appends a translated/recolored copy of `src`'s CPU verts to `v`/`idx`.
void AppendMeshAt(std::vector<Vertex3D>& v, std::vector<uint16_t>& idx, const Mesh& src,
                  const math::Vec3& offset, const math::Vec4& color) {
    std::vector<Vertex3D> verts = src.CpuVerts();
    RecolorVerts(verts, color);
    for (auto& vv : verts) vv.pos += offset;
    AppendMesh(v, idx, verts, src.CpuIndices());
}

// Appends a flat quad from four world-space corners with explicit UVs. The
// corner order is p0,p1,p2,p3 counter-clockwise when viewed from the +normal
// side; the two triangles are (p0,p1,p2),(p0,p2,p3). Used for the gable-roof
// sloped panels so their UVs follow the roof slope for tiled texture tiling.
void AppendQuad(std::vector<Vertex3D>& v, std::vector<uint16_t>& idx,
                const math::Vec3& p0, const math::Vec3& p1, const math::Vec3& p2,
                const math::Vec3& p3, const math::Vec2& uv0, const math::Vec2& uv1,
                const math::Vec2& uv2, const math::Vec2& uv3) {
    const math::Vec3 normal =
        math::Cross(p1 - p0, p2 - p0).LengthSq() > 1e-8f
            ? math::Cross(p1 - p0, p2 - p0).Normalized()
            : math::Vec3{0.0f, 1.0f, 0.0f};
    const uint16_t base = static_cast<uint16_t>(v.size());
    v.push_back({p0, normal, uv0, {1.0f, 1.0f, 1.0f, 1.0f}});
    v.push_back({p1, normal, uv1, {1.0f, 1.0f, 1.0f, 1.0f}});
    v.push_back({p2, normal, uv2, {1.0f, 1.0f, 1.0f, 1.0f}});
    v.push_back({p3, normal, uv3, {1.0f, 1.0f, 1.0f, 1.0f}});
    idx.push_back(base + 0);
    idx.push_back(base + 1);
    idx.push_back(base + 2);
    idx.push_back(base + 0);
    idx.push_back(base + 2);
    idx.push_back(base + 3);
}

// Appends a white box segment (used to carve the door/window openings out of
// the stone wall). Boxes carry per-face 0..1 UVs so the wall albedo tiles.
void AppendBoxSeg(std::vector<Vertex3D>& v, std::vector<uint16_t>& idx, Renderer& renderer,
                  float cx, float cy, float cz, float sx, float sy, float sz) {
    Mesh seg = Mesh::CreateCube(renderer, sx, sy, sz, "house_seg");
    AppendMeshAt(v, idx, seg, {cx, cy, cz}, {1.0f, 1.0f, 1.0f, 1.0f});
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
    // Textured stone farmhouse. Wall segments are white-baked so an entity's
    // albedo (stone tiles) shows through at full colour; openings are literally
    // holes (door front, two windows on the side walls) so the house reads as
    // enterable rather than a solid box.
    const float W = 4.2f, D = 3.4f, H = 2.6f, tw = 0.26f;
    const float hx = W * 0.5f, hz = D * 0.5f;
    // Four corner pillars (a touch of relief around the openings).
    const float ph = H, ps = 0.34f;
    for (const float sx : {-hx, hx}) {
        for (const float sz : {-hz, hz}) AppendBoxSeg(v, idx, renderer, sx, H * 0.5f, sz, ps, ph, ps);
    }
    // Front wall (+Z) with a centred door opening (1.1 x 2.0).
    const float dw = 0.55f, dh = 2.0f; // door half-width / height
    AppendBoxSeg(v, idx, renderer, -(hx + dw) * 0.5f, dh * 0.5f, hz, hx - dw, dh, tw);
    AppendBoxSeg(v, idx, renderer, (hx + dw) * 0.5f, dh * 0.5f, hz, hx - dw, dh, tw);
    AppendBoxSeg(v, idx, renderer, 0.0f, (dh + H) * 0.5f, hz, dw * 2.0f, H - dh, tw);
    // Back wall (-Z): solid.
    AppendBoxSeg(v, idx, renderer, 0.0f, H * 0.5f, -hz, W, H, tw);
    // Side walls (+/-X) with a window opening (0.95 wide in z, 1.1..2.0 high).
    const float wz = 0.475f, wy0 = 1.1f, wy1 = 2.0f;
    for (const float sx : {-hx, hx}) {
        AppendBoxSeg(v, idx, renderer, sx, H * 0.5f, -(hz + wz) * 0.5f, tw, H, hz - wz);
        AppendBoxSeg(v, idx, renderer, sx, H * 0.5f, (hz + wz) * 0.5f, tw, H, hz - wz);
        AppendBoxSeg(v, idx, renderer, sx, wy0 * 0.5f, 0.0f, tw, wy0, wz * 2.0f);
        AppendBoxSeg(v, idx, renderer, sx, (wy1 + H) * 0.5f, 0.0f, tw, H - wy1, wz * 2.0f);
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeRoofMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    // Tiled gable roof for a MakeHouseMesh: ridge along X at y=H+rise, two
    // sloped panels dropping to the eaves at y=H, closed with end triangles and
    // a chimney. Uses AppendQuad so the roof panels tile their UVs with the
    // slope (a per-metre uvRepeat), giving crisp roof tiles.
    const float H = 2.6f, hx = 2.1f, hz = 1.7f;
    const float rise = 1.5f, over = 0.30f; // roof rise / eave overhang
    const float ex = hx + over, ez = hz + over; // eave extent
    const float ridgeY = H + rise;
    // UV scale: roughly one tile per metre, so roof.png tiles ~ (2*ex) x slope.
    const float uScale = 1.0f / 1.1f;
    const float slopeLen = std::sqrt(rise * rise + ez * ez);
    // Front (+Z) sloped panel.
    AppendQuad(v, idx, {-ex, H, ez}, {-ex, ridgeY, 0.0f}, {ex, ridgeY, 0.0f}, {ex, H, ez},
               {0.0f, slopeLen * uScale}, {0.0f, 0.0f}, {2.0f * ex * uScale, 0.0f},
               {2.0f * ex * uScale, slopeLen * uScale});
    // Back (-Z) sloped panel (mirrored -> its winding faces outward correctly).
    AppendQuad(v, idx, {ex, H, -ez}, {ex, ridgeY, 0.0f}, {-ex, ridgeY, 0.0f}, {-ex, H, -ez},
               {0.0f, slopeLen * uScale}, {0.0f, 0.0f}, {2.0f * ex * uScale, 0.0f},
               {2.0f * ex * uScale, slopeLen * uScale});
    // End gable triangles (+X and -X) closing the roof volume.
    auto gable = [&](float x) {
        const uint16_t b = static_cast<uint16_t>(v.size());
        const math::Vec3 normal{x > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
        v.push_back({{x, H, -ez}, normal, {0.0f, 0.0f}, {1, 1, 1, 1}});
        v.push_back({{x, H, ez}, normal, {2.0f * ez * uScale, 0.0f}, {1, 1, 1, 1}});
        v.push_back({{x, ridgeY, 0.0f}, normal, {ez * uScale, rise * 2 * uScale}, {1, 1, 1, 1}});
        idx.insert(idx.end(), {b, static_cast<uint16_t>(b + 1), static_cast<uint16_t>(b + 2)});
    };
    gable(ex);
    gable(-ex);
    // Chimney: a small stone box rising from the ridge toward the +X end.
    const float cw = 0.5f, ch = 1.3f, cx = 1.15f;
    AppendBoxSeg(v, idx, renderer, cx, ridgeY - 0.15f + ch * 0.5f, 0.0f, cw, ch, cw);
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

Mesh MakeHeroMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    // Blue armored tunic.
    Mesh torso = Mesh::CreateCube(renderer, 0.62f, 0.9f, 0.42f, "torso");
    AppendMeshAt(v, idx, torso, {0.0f, 1.05f, 0.0f}, {0.22f, 0.36f, 0.75f, 1.0f});
    // Gold belt.
    Mesh belt = Mesh::CreateCube(renderer, 0.66f, 0.14f, 0.46f, "belt");
    AppendMeshAt(v, idx, belt, {0.0f, 0.62f, 0.0f}, {0.85f, 0.68f, 0.25f, 1.0f});
    // Legs.
    Mesh leg = Mesh::CreateCube(renderer, 0.26f, 0.62f, 0.28f, "leg");
    AppendMeshAt(v, idx, leg, {-0.16f, 0.31f, 0.0f}, {0.30f, 0.32f, 0.40f, 1.0f});
    AppendMeshAt(v, idx, leg, {0.16f, 0.31f, 0.0f}, {0.30f, 0.32f, 0.40f, 1.0f});
    // Skin head.
    Mesh head = Mesh::CreateSphere(renderer, 0.26f, 12, 8, "head");
    AppendMeshAt(v, idx, head, {0.0f, 1.65f, 0.0f}, {0.88f, 0.75f, 0.62f, 1.0f});
    // Sword at the hip (thin bright blade).
    Mesh blade = Mesh::CreateCube(renderer, 0.10f, 1.05f, 0.05f, "blade");
    AppendMeshAt(v, idx, blade, {0.42f, 0.85f, 0.0f}, {0.85f, 0.92f, 1.0f, 1.0f});
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

// Painterly tree billboard volume: three crossed quads carrying a hand-painted
// whole-tree texture. Far cheaper than leaf geometry and, unlike stacked cones,
// keeps a soft irregular silhouette at every viewing angle.
Mesh MakeTreeCardMesh(Renderer& renderer, const std::string& name, float width,
                      float height) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    for (int plane = 0; plane < 3; ++plane) {
        const float a = static_cast<float>(plane) * 1.0471976f;
        const math::Vec3 right{std::cos(a) * width * 0.5f, 0.0f,
                               std::sin(a) * width * 0.5f};
        const math::Vec3 n{std::sin(a), 0.0f, -std::cos(a)};
        const uint16_t base = static_cast<uint16_t>(v.size());
        v.push_back({-right, n, {0.0f, 0.0f}, math::Vec4{1.0f, 1.0f, 1.0f, 1.0f}});
        v.push_back({right, n, {1.0f, 0.0f}, math::Vec4{1.0f, 1.0f, 1.0f, 1.0f}});
        v.push_back({right + math::Vec3{0.0f, height, 0.0f}, n, {1.0f, 1.0f},
                     math::Vec4{1.0f, 1.0f, 1.0f, 1.0f}});
        v.push_back({-right + math::Vec3{0.0f, height, 0.0f}, n, {0.0f, 1.0f},
                     math::Vec4{1.0f, 1.0f, 1.0f, 1.0f}});
        idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 1),
                               static_cast<uint16_t>(base + 2), base,
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 3)});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakePineMesh(Renderer& renderer, const std::string& name) {
    return MakeTreeCardMesh(renderer, name, 3.6f, 7.6f);
}

Mesh MakeCloudPlaneMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v = {
        {{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {0, 0}, {1, 1, 1, 1}},
        {{-0.5f, 0.0f, 0.5f}, {0, 1, 0}, {0, 1}, {1, 1, 1, 1}},
        {{0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 1}, {1, 1, 1, 1}},
        {{0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 0}, {1, 1, 1, 1}},
    };
    std::vector<uint16_t> idx = {0, 1, 2, 0, 2, 3};
    return Mesh::CreateFromData(renderer, v.data(), 4, idx.data(), 6, name);
}

Mesh MakeCliffFaceMesh(Renderer& renderer, const std::string& name) {
    constexpr int kZ = 124;
    constexpr int kY = 58;
    constexpr float kZStep = 0.58f;
    constexpr float kYStep = 0.56f;
    auto heightAt = [](float z, float y) {
        const float n = std::sin(z * 0.33f + y * 0.27f) +
                        0.55f * std::sin(z * 0.91f - y * 0.53f + 1.7f) +
                        0.28f * std::sin(z * 2.10f + y * 1.20f) +
                        0.14f * std::sin(z * 4.70f + y * 3.90f);
        const float ledge = 0.18f * std::sin(y * 1.70f + z * 0.22f) +
                            0.12f * std::sin(y * 2.80f + 0.6f);
        return 5.00f + 0.34f * y + 0.55f * n + ledge;
    };
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    v.reserve(static_cast<size_t>(kZ) * kY);
    for (int iz = 0; iz < kZ; ++iz) {
        const float z = -52.0f + iz * kZStep;
        for (int iy = 0; iy < kY; ++iy) {
            const float y = iy * kYStep;
            const float e = 0.18f;
            const float dyd = (heightAt(z, y + e) - heightAt(z, y - e)) / (2.0f * e);
            const float dzd = (heightAt(z + e, y) - heightAt(z - e, y)) / (2.0f * e);
            const math::Vec3 n = math::Vec3{-1.0f, dyd, dzd}.Normalized();
            v.push_back({math::Vec3{heightAt(z, y), y, z}, n,
                         {z * 0.34f + y * 0.10f, y * 0.40f - z * 0.08f}, {1, 1, 1, 1}});
        }
    }
    for (int iz = 0; iz + 1 < kZ; ++iz) {
        for (int iy = 0; iy + 1 < kY; ++iy) {
            const uint16_t a = static_cast<uint16_t>(iz * kY + iy);
            const uint16_t b = static_cast<uint16_t>(a + kY);
            const uint16_t c = static_cast<uint16_t>(b + 1);
            const uint16_t d = static_cast<uint16_t>(a + 1);
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}
Mesh MakeStylizedTreeMesh(Renderer& renderer, const std::string& name, bool pink) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    auto tri = [&](const math::Vec3& a, const math::Vec3& b, const math::Vec3& c,
                   const math::Vec4& col) {
        const math::Vec3 n = math::Cross(b - a, c - a).Normalized();
        const uint16_t base = static_cast<uint16_t>(v.size());
        v.push_back({a, n, {0, 0}, col});
        v.push_back({b, n, {0, 0}, col});
        v.push_back({c, n, {0, 0}, col});
        idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
    };
    auto cylinder = [&](const math::Vec3& a, const math::Vec3& b, float ra, float rb,
                        const math::Vec4& col) {
        const math::Vec3 axis = (b - a).Normalized();
        const math::Vec3 up = std::abs(axis.y) > 0.9f ? math::Vec3{1, 0, 0}
                                                       : math::Vec3{0, 1, 0};
        const math::Vec3 u = math::Cross(axis, up).Normalized();
        const math::Vec3 w = math::Cross(axis, u).Normalized();
        constexpr int S = 6;
        for (int i = 0; i < S; ++i) {
            const float a0 = 6.2831853f * i / S;
            const float a1 = 6.2831853f * (i + 1) / S;
            const math::Vec3 d0 = u * std::cos(a0) + w * std::sin(a0);
            const math::Vec3 d1 = u * std::cos(a1) + w * std::sin(a1);
            tri(a + d0 * ra, b + d0 * rb, b + d1 * rb, col);
            tri(a + d0 * ra, b + d1 * rb, a + d1 * ra, col);
        }
    };
    const std::array<math::Vec3, 12> ico = {
        math::Vec3{-1, 1.618f, 0}, math::Vec3{1, 1.618f, 0},
        math::Vec3{-1, -1.618f, 0}, math::Vec3{1, -1.618f, 0},
        math::Vec3{0, -1, 1.618f}, math::Vec3{0, 1, 1.618f},
        math::Vec3{0, -1, -1.618f}, math::Vec3{0, 1, -1.618f},
        math::Vec3{1.618f, 0, -1}, math::Vec3{1.618f, 0, 1},
        math::Vec3{-1.618f, 0, -1}, math::Vec3{-1.618f, 0, 1}};
    const std::array<std::array<int, 3>, 20> faces = {{
        {{0, 11, 5}}, {{0, 5, 1}}, {{0, 1, 7}}, {{0, 7, 10}}, {{0, 10, 11}},
        {{1, 5, 9}}, {{5, 11, 4}}, {{11, 10, 2}}, {{10, 7, 6}}, {{7, 1, 8}},
        {{3, 9, 4}}, {{3, 4, 2}}, {{3, 2, 6}}, {{3, 6, 8}}, {{3, 8, 9}},
        {{4, 9, 5}}, {{2, 4, 11}}, {{6, 2, 10}}, {{8, 6, 7}}, {{9, 8, 1}}}};
    auto blob = [&](const math::Vec3& c, float r, const math::Vec4& col, float seed) {
        std::array<math::Vec3, 12> p{};
        for (size_t i = 0; i < p.size(); ++i) p[i] = c + ico[i].Normalized() * r;
        for (size_t f = 0; f < faces.size(); ++f) {
            const auto& q = faces[f];
            const float t = 0.86f + 0.14f * std::sin(seed + f * 1.7f);
            const math::Vec4 cc{col.x * t, col.y * t, col.z * t, 1.0f};
            tri(p[q[0]], p[q[1]], p[q[2]], cc);
        }
    };
    const math::Vec4 bark{0.31f, 0.22f, 0.15f, 1.0f};
    const math::Vec4 leafA = pink ? math::Vec4{0.83f, 0.38f, 0.54f, 1}
                                  : math::Vec4{0.22f, 0.38f, 0.18f, 1};
    const math::Vec4 leafB = pink ? math::Vec4{0.95f, 0.62f, 0.72f, 1}
                                  : math::Vec4{0.31f, 0.48f, 0.23f, 1};
    const math::Vec4 leafC = pink ? math::Vec4{0.67f, 0.25f, 0.40f, 1}
                                  : math::Vec4{0.17f, 0.30f, 0.14f, 1};
    cylinder({0, 0, 0}, {0.05f, 4.2f, 0}, 0.30f, 0.16f, bark);
    const std::array<math::Vec3, 5> tips = {
        math::Vec3{-1.5f, 3.9f, 0.2f}, math::Vec3{1.35f, 4.0f, 0.5f},
        math::Vec3{0.2f, 3.7f, -1.45f}, math::Vec3{-0.2f, 3.9f, 1.4f},
        math::Vec3{0.0f, 5.4f, 0.0f}};
    for (size_t i = 0; i < tips.size(); ++i) {
        cylinder({0, 2.1f + 0.25f * i, 0}, tips[i], 0.10f, 0.06f, bark);
    }
    blob({-1.55f, 4.7f, 0.25f}, 1.35f, leafA, 1.0f);
    blob({1.45f, 4.8f, 0.55f}, 1.28f, leafB, 2.0f);
    blob({0.25f, 4.6f, -1.55f}, 1.22f, leafC, 3.0f);
    blob({-0.15f, 5.9f, 0.25f}, 1.40f, leafB, 4.0f);
    blob({0.9f, 5.2f, 1.15f}, 1.05f, leafA, 5.0f);
    blob({-1.1f, 5.5f, -0.9f}, 1.0f, leafB, 6.0f);
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}
Mesh MakeBroadleafMesh(Renderer& renderer, const std::string& name) {
    return MakeTreeCardMesh(renderer, name, 5.4f, 6.8f);
}

Mesh MakeBlossomMesh(Renderer& renderer, const std::string& name) {
    return MakeTreeCardMesh(renderer, name, 4.4f, 5.4f);
}

Mesh MakeLeafCanopyMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    auto hash = [](float n) {
        const float s = std::sin(n * 127.1f + 311.7f) * 43758.5453f;
        return s - std::floor(s);
    };
    // Dense alpha-tested leaf cards fill the sculpted Poly Haven boughs. The
    // cards are real geometry (not billboards) so the canopy keeps parallax
    // and dappled shading while the high-poly trunk supplies the silhouette.
    for (int i = 0; i < 4200; ++i) {
        const float a = hash(i * 1.31f) * 6.2831853f;
        const float rr = std::sqrt(hash(i * 2.17f)) * 1.72f;
        const float yy = 2.72f + (hash(i * 4.73f) - 0.5f) * 1.95f;
        const math::Vec3 c{std::cos(a) * rr, yy, std::sin(a) * rr};
        const float yaw = hash(i * 11.3f) * 6.2831853f;
        const float pitch = (hash(i * 5.9f) - 0.5f) * 1.15f;
        const math::Vec3 n{std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                           std::cos(pitch) * std::sin(yaw)};
        math::Vec3 t = math::Cross(n, math::Vec3{0.0f, 1.0f, 0.0f});
        if (t.LengthSq() < 1e-5f) t = math::Vec3{1.0f, 0.0f, 0.0f};
        t = t.Normalized();
        math::Vec3 b = math::Cross(n, t).Normalized();
        const float grow = 0.62f + 0.28f * hash(i * 7.07f);
        const float h = 0.30f * grow;
        const float w = 0.26f * grow;
        const float roll = (hash(i * 17.1f) - 0.5f) * 0.7f;
        const math::Vec3 tw = t * std::cos(roll) + b * std::sin(roll);
        const math::Vec3 bw = b * std::cos(roll) - t * std::sin(roll);
        const float hue = hash(i * 13.7f);
        const math::Vec4 col =
            hue > 0.75f ? math::Vec4{0.80f, 0.86f, 0.52f, 1.0f}
                        : (hue > 0.30f ? math::Vec4{0.66f, 0.78f, 0.46f, 1.0f}
                                       : math::Vec4{0.50f, 0.66f, 0.36f, 1.0f});
        const uint16_t base = static_cast<uint16_t>(v.size());
        v.push_back({c - tw * w - bw * h, n, {0.0f, 0.0f}, col});
        v.push_back({c + tw * w - bw * h, n, {1.0f, 0.0f}, col});
        v.push_back({c + tw * w + bw * h, n, {1.0f, 1.0f}, col});
        v.push_back({c - tw * w + bw * h, n, {0.0f, 1.0f}, col});
        idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 1),
                               static_cast<uint16_t>(base + 2), base,
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 3)});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeMountainRidgeMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    auto hash = [](float n) {
        const float s = std::sin(n * 127.1f + 311.7f) * 43758.5453f;
        return s - std::floor(s);
    };
    constexpr int kSeg = 72;
    const math::Vec4 low{0.49f, 0.58f, 0.64f, 1.0f};
    const math::Vec4 high{0.73f, 0.80f, 0.85f, 1.0f};
    for (int i = 0; i <= kSeg; ++i) {
        const float x = -1.0f + 2.0f * static_cast<float>(i) / kSeg;
        // Broad massifs with sharper secondary peaks; the absolute sine folds
        // turn the profile into ridge crests instead of rolling hills.
        const float massif = 0.48f + 0.26f * std::sin(x * 4.1f + 1.3f);
        const float peaks = 0.22f * std::fabs(std::sin(x * 9.7f + 0.4f)) +
                            0.10f * std::fabs(std::sin(x * 23.0f + 2.2f));
        const float h = massif + peaks + 0.05f * hash(i * 3.1f);
        v.push_back({{x, -0.18f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, low});
        v.push_back({{x, h, 0.0f}, {0.0f, 0.0f, 1.0f},
                     {0.0f, h}, h > 0.72f ? high : low});
    }
    for (int i = 0; i < kSeg; ++i) {
        const uint16_t a = static_cast<uint16_t>(i * 2);
        idx.insert(idx.end(), {a, static_cast<uint16_t>(a + 2),
                               static_cast<uint16_t>(a + 1),
                               static_cast<uint16_t>(a + 1),
                               static_cast<uint16_t>(a + 2),
                               static_cast<uint16_t>(a + 3)});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeRockPillarMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const math::Vec4 stone{0.34f, 0.33f, 0.31f, 1.0f};
    const math::Vec4 moss{0.24f, 0.31f, 0.21f, 1.0f};
    // Irregular stacked rings read as a weathered cliff spire instead of a
    // smooth cone: the silhouette steps inward and the rings are offset.
    struct Ring {
        float y, r, ox, oz;
    };
    const Ring rings[] = {
        {0.0f, 1.75f, 0.00f, 0.00f},
        {2.2f, 1.58f, 0.10f, -0.05f},
        {4.8f, 1.28f, -0.08f, 0.10f},
        {7.4f, 0.98f, 0.06f, -0.09f},
        {10.1f, 0.60f, -0.02f, 0.03f},
        {11.4f, 0.34f, 0.03f, -0.02f},
    };
    const int seg = 9;
    const int ringCount = static_cast<int>(sizeof(rings) / sizeof(rings[0]));
    for (int ri = 0; ri + 1 < ringCount; ++ri) {
        const Ring& lo = rings[ri];
        const Ring& hi = rings[ri + 1];
        for (int i = 0; i < seg; ++i) {
            const float a0 = static_cast<float>(i) / seg * math::kTwoPi;
            const float a1 = static_cast<float>(i + 1) / seg * math::kTwoPi;
            auto radiusAt = [&](int ringIdx, int segIdx) {
                const Ring& ring = rings[ringIdx];
                return ring.r * (0.90f + 0.13f * std::sin(segIdx * 2.1f + ringIdx * 1.4f));
            };
            auto point = [](const Ring& ring, float a, float r) {
                return math::Vec3{ring.ox + std::cos(a) * r, ring.y,
                                  ring.oz + std::sin(a) * r};
            };
            const math::Vec3 p0 = point(lo, a0, radiusAt(ri, i));
            const math::Vec3 p1 = point(lo, a1, radiusAt(ri, i + 1));
            const math::Vec3 p2 = point(hi, a1, radiusAt(ri + 1, i + 1));
            const math::Vec3 p3 = point(hi, a0, radiusAt(ri + 1, i));
            math::Vec3 n = math::Cross(p1 - p0, p2 - p0).Normalized();
            const math::Vec3 mid{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f,
                                 (p0.z + p2.z) * 0.5f};
            const math::Vec3 axis{(lo.ox + hi.ox) * 0.5f, mid.y,
                                  (lo.oz + hi.oz) * 0.5f};
            if (math::Dot(n, mid - axis) < 0.0f)
                n = math::Vec3{-n.x, -n.y, -n.z};
            const uint16_t base = static_cast<uint16_t>(v.size());
            const math::Vec4 c = ((ri == 0 && i % 3 == 0) || (ri == 2 && i % 5 == 0)) ? moss : stone;
            v.push_back({p0, n, {a0 / math::kTwoPi, lo.y * 0.1f}, c});
            v.push_back({p1, n, {a1 / math::kTwoPi, lo.y * 0.1f}, c});
            v.push_back({p2, n, {a1 / math::kTwoPi, hi.y * 0.1f}, c});
            v.push_back({p3, n, {a0 / math::kTwoPi, hi.y * 0.1f}, c});
            if (math::Dot(math::Cross(p1 - p0, p2 - p0), mid - axis) >= 0.0f) {
                idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 1),
                                       static_cast<uint16_t>(base + 2), base,
                                       static_cast<uint16_t>(base + 2),
                                       static_cast<uint16_t>(base + 3)});
            } else {
                idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 2),
                                       static_cast<uint16_t>(base + 1), base,
                                       static_cast<uint16_t>(base + 3),
                                       static_cast<uint16_t>(base + 2)});
            }
        }
    }
    Mesh boulder = Mesh::CreateSphere(renderer, 1.0f, 10, 7, "pillar_boulder");
    AppendMeshAt(v, idx, boulder, {-1.25f, 0.35f, 0.25f}, stone);
    AppendMeshAt(v, idx, boulder, {1.10f, 0.28f, -0.35f}, moss);
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeBoulderMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    Mesh sphere = Mesh::CreateSphere(renderer, 1.0f, 14, 9, "boulder_base");
    std::vector<Vertex3D> sv = sphere.CpuVerts();
    for (auto& vv : sv) {
        const float warp = 1.0f + 0.13f * std::sin(vv.pos.x * 7.0f + vv.pos.z * 4.0f) +
                           0.08f * std::cos(vv.pos.y * 9.0f - vv.pos.x * 3.0f);
        vv.pos.x *= 1.18f * warp;
        vv.pos.z *= 1.05f * warp;
        vv.pos.y *= 0.64f;
        if (vv.pos.y < -0.36f) vv.pos.y = -0.36f + (vv.pos.y + 0.36f) * 0.12f;
        const math::Vec4 stone{0.56f, 0.55f, 0.51f, 1.0f};
        const math::Vec4 moss{0.34f, 0.43f, 0.29f, 1.0f};
        vv.color = (vv.pos.y > 0.20f &&
                    std::sin(vv.pos.x * 8.0f + vv.pos.z * 6.0f) > 0.15f)
                       ? moss
                       : stone;
    }
    AppendMesh(v, idx, sv, sphere.CpuIndices());
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeStoneLanternMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const math::Vec4 stone{0.23f, 0.23f, 0.21f, 1.0f};
    const math::Vec4 light{1.0f, 0.70f, 0.30f, 1.0f};
    Mesh base = Mesh::CreateCube(renderer, 0.82f, 0.22f, 0.82f, "lantern_base");
    AppendMeshAt(v, idx, base, {0.0f, 0.11f, 0.0f}, stone);
    Mesh post = Mesh::CreateCube(renderer, 0.24f, 1.05f, 0.24f, "lantern_post");
    AppendMeshAt(v, idx, post, {0.0f, 0.74f, 0.0f}, stone);
    Mesh box = Mesh::CreateCube(renderer, 0.66f, 0.50f, 0.66f, "lantern_box");
    AppendMeshAt(v, idx, box, {0.0f, 1.48f, 0.0f}, light);
    auto roof = ConeVerts(0.78f, 0.48f, 4, stone);
    for (auto& vv : roof) vv.pos.y += 1.72f;
    AppendMesh(v, idx, roof, ConeIndices(4));
    Mesh finial = Mesh::CreateCube(renderer, 0.16f, 0.24f, 0.16f, "lantern_finial");
    AppendMeshAt(v, idx, finial, {0.0f, 2.30f, 0.0f}, stone);
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeCultivatorMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const math::Vec4 robe{0.075f, 0.085f, 0.11f, 1.0f};
    const math::Vec4 trim{0.48f, 0.38f, 0.22f, 1.0f};
    const math::Vec4 skin{0.84f, 0.70f, 0.56f, 1.0f};
    const math::Vec4 hair{0.58f, 0.61f, 0.67f, 1.0f};
    const math::Vec4 under{0.52f, 0.52f, 0.48f, 1.0f};
    // Layered robe: a short pale under-robe hem under the dark outer skirt.
    auto underskirt = ConeVerts(0.62f, 0.22f, 12, under);
    for (auto& vv : underskirt) vv.pos.y -= 0.02f;
    AppendMesh(v, idx, underskirt, ConeIndices(12));
    auto skirt = ConeVerts(0.56f, 1.32f, 12, robe);
    AppendMesh(v, idx, skirt, ConeIndices(12));
    Mesh torso = Mesh::CreateCube(renderer, 0.46f, 0.56f, 0.30f, "cultivator_torso");
    AppendMeshAt(v, idx, torso, {0.0f, 1.22f, 0.0f}, robe);
    Mesh collar = Mesh::CreateCube(renderer, 0.36f, 0.10f, 0.30f, "cultivator_collar");
    AppendMeshAt(v, idx, collar, {0.0f, 1.43f, 0.0f}, under);
    Mesh sash = Mesh::CreateCube(renderer, 0.50f, 0.10f, 0.32f, "cultivator_sash");
    AppendMeshAt(v, idx, sash, {0.0f, 1.00f, 0.0f}, trim);
    Mesh sleeve = Mesh::CreateCube(renderer, 0.20f, 0.54f, 0.22f, "cultivator_sleeve");
    AppendMeshAt(v, idx, sleeve, {-0.31f, 1.24f, 0.0f}, robe);
    AppendMeshAt(v, idx, sleeve, {0.31f, 1.24f, 0.0f}, robe);
    Mesh hand = Mesh::CreateSphere(renderer, 0.075f, 8, 6, "cultivator_hand");
    AppendMeshAt(v, idx, hand, {-0.33f, 0.98f, 0.0f}, skin);
    AppendMeshAt(v, idx, hand, {0.33f, 0.98f, 0.0f}, skin);
    Mesh head = Mesh::CreateSphere(renderer, 0.22f, 12, 8, "cultivator_head");
    AppendMeshAt(v, idx, head, {0.0f, 1.66f, 0.0f}, skin);
    Mesh backHair = Mesh::CreateCube(renderer, 0.24f, 0.86f, 0.09f, "cultivator_hair");
    AppendMeshAt(v, idx, backHair, {0.0f, 1.40f, 0.17f}, hair);
    Mesh lock = Mesh::CreateCube(renderer, 0.055f, 0.55f, 0.06f, "cultivator_lock");
    AppendMeshAt(v, idx, lock, {-0.15f, 1.34f, 0.16f}, hair);
    AppendMeshAt(v, idx, lock, {0.15f, 1.34f, 0.16f}, hair);
    Mesh topHair = Mesh::CreateSphere(renderer, 0.22f, 10, 7, "cultivator_hair_top");
    AppendMeshAt(v, idx, topHair, {0.0f, 1.81f, 0.03f}, hair);
    Mesh pin = Mesh::CreateCube(renderer, 0.06f, 0.06f, 0.30f, "cultivator_pin");
    AppendMeshAt(v, idx, pin, {0.13f, 1.90f, 0.02f}, trim);
    Mesh jade = Mesh::CreateSphere(renderer, 0.065f, 8, 6, "cultivator_jade");
    AppendMeshAt(v, idx, jade, {0.18f, 1.22f, -0.17f}, {0.20f, 0.55f, 0.40f, 1.0f});
    Mesh sword = Mesh::CreateCube(renderer, 0.07f, 1.15f, 0.04f, "cultivator_sword");
    AppendMeshAt(v, idx, sword, {0.29f, 0.82f, 0.20f}, {0.72f, 0.76f, 0.82f, 1.0f});
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeFernMesh(Renderer& renderer, const std::string& name) {
    (void)renderer;
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const int fronds = 9;
    for (int f = 0; f < fronds; ++f) {
        const float a = static_cast<float>(f) / fronds * math::kTwoPi +
                        (f % 2 ? 0.17f : -0.11f);
        const float dirX = std::cos(a);
        const float dirZ = std::sin(a);
        const float sideX = -dirZ;
        const float sideZ = dirX;
        const float len = 0.55f + (f % 3) * 0.10f;
        const float midY = 0.34f + (f % 2) * 0.06f;
        const math::Vec3 tip{dirX * len, 0.22f, dirZ * len};
        const math::Vec3 mid{dirX * len * 0.55f, midY, dirZ * len * 0.55f};
        const float w0 = 0.075f, w1 = 0.105f, w2 = 0.012f;
        const math::Vec3 baseL{-sideX * w0, 0.01f, -sideZ * w0};
        const math::Vec3 baseR{sideX * w0, 0.01f, sideZ * w0};
        const math::Vec3 midL{mid.x - sideX * w1, mid.y, mid.z - sideZ * w1};
        const math::Vec3 midR{mid.x + sideX * w1, mid.y, mid.z + sideZ * w1};
        const math::Vec3 tipL{tip.x - sideX * w2, tip.y, tip.z - sideZ * w2};
        const math::Vec3 tipR{tip.x + sideX * w2, tip.y, tip.z + sideZ * w2};
        const math::Vec4 dark{0.075f, 0.23f, 0.10f, 1.0f};
        const math::Vec4 light{0.18f, 0.42f, 0.17f, 1.0f};
        const math::Vec3 n{0.0f, 1.0f, 0.0f};
        const uint16_t base = static_cast<uint16_t>(v.size());
        auto add = [&](const math::Vec3& p, const math::Vec4& c, float u, float vv) {
            v.push_back({p, n, {u, vv}, c});
        };
        add(baseL, dark, 0.0f, 0.0f);
        add(baseR, dark, 1.0f, 0.0f);
        add(midL, light, 0.0f, 0.55f);
        add(midR, light, 1.0f, 0.55f);
        add(tipL, light, 0.0f, 1.0f);
        add(tipR, light, 1.0f, 1.0f);
        // Two-sided leaf strip: front and back faces so the fan reads from any
        // camera angle without a dedicated alpha-card material.
        idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 1),
                               static_cast<uint16_t>(base + 3), base,
                               static_cast<uint16_t>(base + 3),
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 3),
                               static_cast<uint16_t>(base + 5), base,
                               static_cast<uint16_t>(base + 5),
                               static_cast<uint16_t>(base + 4)});
        idx.insert(idx.end(), {base, static_cast<uint16_t>(base + 3),
                               static_cast<uint16_t>(base + 1), base,
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 3),
                               static_cast<uint16_t>(base + 2),
                               static_cast<uint16_t>(base + 5),
                               static_cast<uint16_t>(base + 3), base,
                               static_cast<uint16_t>(base + 4),
                               static_cast<uint16_t>(base + 5)});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeRedShrubMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    Mesh trunk = Mesh::CreateCube(renderer, 0.16f, 0.55f, 0.16f, "shrub_trunk");
    AppendMeshAt(v, idx, trunk, {0.0f, 0.27f, 0.0f}, {0.28f, 0.18f, 0.12f, 1.0f});
    Mesh leaf = Mesh::CreateSphere(renderer, 0.45f, 12, 8, "shrub_leaf");
    AppendMeshAt(v, idx, leaf, {-0.28f, 0.62f, 0.10f}, {0.46f, 0.12f, 0.08f, 1.0f});
    AppendMeshAt(v, idx, leaf, {0.26f, 0.70f, -0.08f}, {0.62f, 0.20f, 0.10f, 1.0f});
    AppendMeshAt(v, idx, leaf, {0.05f, 0.88f, 0.12f}, {0.72f, 0.28f, 0.12f, 1.0f});
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeStonePathMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const int slabs = 34;
    for (int i = 0; i < slabs; ++i) {
        const float z = -37.4f + i * 2.2f;
        const float w = 4.15f + 0.09f * std::sin(i * 2.1f);
        Mesh slab = Mesh::CreateCube(renderer, w, 0.12f, 2.34f, "path_slab");
        const float shade = 0.68f + 0.05f * std::sin(i * 1.7f);
        AppendMeshAt(v, idx, slab,
                     {0.05f * std::sin(i * 1.3f), 0.06f, z},
                     {shade, shade, shade * 0.96f, 1.0f});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeWolfMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const math::Vec4 fur{0.55f, 0.55f, 0.58f, 1.0f};
    // Horizontal body.
    Mesh body = Mesh::CreateCube(renderer, 1.5f, 0.6f, 0.6f, "wolf_body");
    AppendMeshAt(v, idx, body, {0.0f, 0.7f, 0.0f}, fur);
    // Head (front, +Z).
    Mesh head = Mesh::CreateCube(renderer, 0.5f, 0.45f, 0.5f, "wolf_head");
    AppendMeshAt(v, idx, head, {0.0f, 0.85f, 0.55f}, {0.48f, 0.48f, 0.52f, 1.0f});
    // Snout.
    Mesh snout = Mesh::CreateCube(renderer, 0.24f, 0.2f, 0.3f, "snout");
    AppendMeshAt(v, idx, snout, {0.0f, 0.78f, 0.92f}, {0.40f, 0.40f, 0.45f, 1.0f});
    // Four legs.
    Mesh leg = Mesh::CreateCube(renderer, 0.18f, 0.7f, 0.18f, "wolf_leg");
    AppendMeshAt(v, idx, leg, {-0.5f, 0.35f, -0.25f}, {0.42f, 0.42f, 0.46f, 1.0f});
    AppendMeshAt(v, idx, leg, {0.5f, 0.35f, -0.25f}, {0.42f, 0.42f, 0.46f, 1.0f});
    AppendMeshAt(v, idx, leg, {-0.5f, 0.35f, 0.25f}, {0.42f, 0.42f, 0.46f, 1.0f});
    AppendMeshAt(v, idx, leg, {0.5f, 0.35f, 0.25f}, {0.42f, 0.42f, 0.46f, 1.0f});
    // Tail (back, -Z).
    Mesh tail = Mesh::CreateCube(renderer, 0.2f, 0.5f, 0.2f, "tail");
    AppendMeshAt(v, idx, tail, {0.0f, 1.0f, -0.55f}, fur);
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

Mesh MakeFireballMesh(Renderer& renderer, const std::string& name) {
    Mesh ball = Mesh::CreateSphere(renderer, 0.28f, 10, 7, "fireball");
    std::vector<Vertex3D> v = ball.CpuVerts();
    RecolorVerts(v, {1.0f, 0.55f, 0.18f, 1.0f});
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                ball.CpuIndices().data(),
                                static_cast<uint32_t>(ball.CpuIndices().size()), name);
}

// A grass tuft rendered as three crossed alpha cards. Each card is a vertical
// quad tiled with grass.png (green blades on a transparent background); the Draw
// system's vegetation pass material sets alphaTest so the transparent parts are
// discarded (crisp silhouette) rather than drawn as a translucent quad. Crossed
// at 120 degrees so the tuft reads from every viewing angle without turning to a
// flat sheet. Vertices are white (baked tint) so the texture shows at full
// colour. Instanced many times by the terrain vegetation scatter.
Mesh MakeGrassMesh(Renderer& renderer, const std::string& name) {
    std::vector<Vertex3D> v;
    std::vector<uint16_t> idx;
    const float w = 0.85f, h = 0.92f;
    static const float angles[] = {0.0f, 1.0472f, 2.0944f}; // 0 / 120 / 240 deg
    for (float a : angles) {
        const float ca = std::cos(a), sa = std::sin(a);
        const math::Vec3 p0{-w * 0.5f * ca, 0.0f, -w * 0.5f * sa};
        const math::Vec3 p1{w * 0.5f * ca, 0.0f, w * 0.5f * sa};
        const math::Vec3 p2{w * 0.5f * ca, h, w * 0.5f * sa};
        const math::Vec3 p3{-w * 0.5f * ca, h, -w * 0.5f * sa};
        // UV: bottom edge v=1 (grass root), top edge v=0 (blade tips).
        AppendQuad(v, idx, p0, p1, p2, p3, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f},
                   {0.0f, 0.0f});
    }
    return Mesh::CreateFromData(renderer, v.data(), static_cast<uint32_t>(v.size()),
                                idx.data(), static_cast<uint32_t>(idx.size()), name);
}

} // namespace neon::gfx



















