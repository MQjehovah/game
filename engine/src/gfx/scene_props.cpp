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
