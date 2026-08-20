#include "art.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace neon::demo {
namespace {

class Bitmap {
public:
    Bitmap(int w, int h) : w_(w), h_(h), px_(static_cast<size_t>(w) * h * 4, 0) {}

    uint8_t* Data() { return px_.data(); }
    int Width() const { return w_; }
    int Height() const { return h_; }

    void Fill(float r, float g, float b, float a = 1.0f) {
        uint8_t R = ToByte(r), G = ToByte(g), B = ToByte(b), A = ToByte(a);
        for (size_t i = 0; i < px_.size(); i += 4) {
            px_[i] = R;
            px_[i + 1] = G;
            px_[i + 2] = B;
            px_[i + 3] = A;
        }
    }

    void SetPixel(int x, int y, float r, float g, float b, float a = 1.0f) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        size_t i = (static_cast<size_t>(y) * w_ + x) * 4;
        px_[i] = ToByte(r);
        px_[i + 1] = ToByte(g);
        px_[i + 2] = ToByte(b);
        px_[i + 3] = ToByte(a);
    }

    void FillRect(int x0, int y0, int x1, int y1, float r, float g, float b, float a = 1.0f) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) SetPixel(x, y, r, g, b, a);
        }
    }

    void FillCircle(int cx, int cy, int radius, float r, float g, float b, float a = 1.0f) {
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                float dx = static_cast<float>(x - cx);
                float dy = static_cast<float>(y - cy);
                float d = std::sqrt(dx * dx + dy * dy);
                if (d <= radius) SetPixel(x, y, r, g, b, a);
            }
        }
    }

    void RadialGlow(int cx, int cy, int radius, float r, float g, float b, float peak = 1.0f) {
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                float dx = static_cast<float>(x - cx);
                float dy = static_cast<float>(y - cy);
                float d = std::sqrt(dx * dx + dy * dy) / radius;
                if (d <= 1.0f) {
                    float a = peak * (1.0f - d) * (1.0f - d);
                    SetPixel(x, y, r, g, b, a);
                }
            }
        }
    }

    void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                      float r, float g, float b, float a = 1.0f) {
        int minY = std::max(0, std::min(y0, std::min(y1, y2)));
        int maxY = std::min(h_ - 1, std::max(y0, std::max(y1, y2)));
        for (int y = minY; y <= maxY; ++y) {
            float minX = 1e9f, maxX = -1e9f;
            int xs[3] = {x0, x1, x2};
            int ys[3] = {y0, y1, y2};
            for (int i = 0; i < 3; ++i) {
                int j = (i + 1) % 3;
                int ya = ys[i], yb = ys[j];
                if (ya == yb) continue;
                if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
                    float t = static_cast<float>(y - ya) / static_cast<float>(yb - ya);
                    float x = static_cast<float>(xs[i]) + t * static_cast<float>(xs[j] - xs[i]);
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                }
            }
            if (maxX < minX) continue;
            for (int x = std::max(0, static_cast<int>(std::ceil(minX)));
                 x <= std::min(w_ - 1, static_cast<int>(std::floor(maxX))); ++x) {
                SetPixel(x, y, r, g, b, a);
            }
        }
    }

private:
    static uint8_t ToByte(float v) {
        return static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f);
    }

    int w_;
    int h_;
    std::vector<uint8_t> px_;
};

gfx::Texture MakeTexture(gfx::Renderer& renderer, Bitmap& bmp) {
    gfx::TextureDesc desc;
    desc.width = bmp.Width();
    desc.height = bmp.Height();
    desc.rgba = bmp.Data();
    desc.mipmaps = true;
    return renderer.CreateTexture(desc);
}

void AddBoxToMesh(std::vector<gfx::Vertex3D>& verts, std::vector<uint16_t>& indices,
                  const math::Vec3& center, const math::Vec3& size) {
    static const math::Vec3 kNormals[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    math::Vec3 half = size * 0.5f;
    math::Vec3 mn = center - half;
    math::Vec3 mx = center + half;
    for (const math::Vec3& n : kNormals) {
        math::Vec3 u, v;
        if (std::fabs(n.x) > 0.5f) {
            u = {0, 0, n.x > 0 ? -1.0f : 1.0f};
            v = {0, 1, 0};
        } else if (std::fabs(n.y) > 0.5f) {
            u = {1, 0, 0};
            v = {0, 0, n.y > 0 ? -1.0f : 1.0f};
        } else {
            u = {1, 0, 0};
            v = {0, n.z > 0 ? 1.0f : -1.0f, 0};
        }
        // project to the correct axis
        float hn = 0.0f;
        if (std::fabs(n.x) > 0.5f) hn = half.x;
        else if (std::fabs(n.y) > 0.5f) hn = half.y;
        else hn = half.z;
        math::Vec3 centerFace = center + n * hn;
        math::Vec3 hu = u * (std::fabs(n.x) > 0.5f ? half.z : (std::fabs(n.y) > 0.5f ? half.x : half.x));
        math::Vec3 hv = v * (std::fabs(n.x) > 0.5f ? half.y : (std::fabs(n.y) > 0.5f ? half.z : half.y));
        math::Vec3 a = centerFace - hu - hv;
        math::Vec3 b = centerFace + hu - hv;
        math::Vec3 c = centerFace + hu + hv;
        math::Vec3 d = centerFace - hu + hv;
        uint16_t base = static_cast<uint16_t>(verts.size());
        verts.push_back({a, n, {0, 0}, {1, 1, 1, 1}});
        verts.push_back({b, n, {1, 0}, {1, 1, 1, 1}});
        verts.push_back({c, n, {1, 1}, {1, 1, 1, 1}});
        verts.push_back({d, n, {0, 1}, {1, 1, 1, 1}});
        indices.insert(indices.end(),
                       {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                        base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)});
    }
}

} // namespace

void CreateDemoAssets(gfx::Renderer& renderer, assets::AssetManager& assetMgr, DemoAssets& out) {
    // Glow sprite
    {
        Bitmap bmp(64, 64);
        bmp.RadialGlow(32, 32, 32, 1.0f, 1.0f, 1.0f);
        out.glow = MakeTexture(renderer, bmp);
    }
    // Ground grid
    {
        Bitmap bmp(256, 256);
        bmp.Fill(0.02f, 0.03f, 0.06f);
        for (int y = 0; y < 256; y += 16) {
            for (int x = 0; x < 256; ++x) bmp.SetPixel(x, y, 0.08f, 0.25f, 0.45f, 0.55f);
        }
        for (int x = 0; x < 256; x += 16) {
            for (int y = 0; y < 256; ++y) bmp.SetPixel(x, y, 0.08f, 0.25f, 0.45f, 0.55f);
        }
        out.ground = MakeTexture(renderer, bmp);
    }
    // Crate
    {
        Bitmap bmp(128, 128);
        bmp.Fill(0.10f, 0.10f, 0.13f);
        bmp.FillRect(4, 4, 123, 123, 0.16f, 0.16f, 0.2f);
        bmp.FillRect(10, 10, 117, 117, 0.11f, 0.12f, 0.16f);
        bmp.FillRect(60, 4, 67, 123, 0.2f, 0.2f, 0.24f);
        bmp.FillRect(4, 60, 123, 67, 0.2f, 0.2f, 0.24f);
        bmp.FillCircle(16, 16, 4, 0.35f, 0.35f, 0.4f);
        bmp.FillCircle(112, 16, 4, 0.35f, 0.35f, 0.4f);
        bmp.FillCircle(16, 112, 4, 0.35f, 0.35f, 0.4f);
        bmp.FillCircle(112, 112, 4, 0.35f, 0.35f, 0.4f);
        out.crate = MakeTexture(renderer, bmp);
    }
    // Pillar
    {
        Bitmap bmp(128, 256);
        bmp.Fill(0.07f, 0.1f, 0.16f);
        for (int y = 0; y < 256; ++y) {
            float t = static_cast<float>(y) / 255.0f;
            bmp.SetPixel(2, y, 0.25f, 0.35f, 0.5f);
            bmp.SetPixel(125, y, 0.25f, 0.35f, 0.5f);
            bmp.SetPixel(8, y, 0.12f + 0.05f * t, 0.18f + 0.05f * t, 0.28f + 0.05f * t);
        }
        for (int y = 0; y < 256; y += 32) bmp.FillRect(4, y, 123, y + 2, 0.14f, 0.2f, 0.3f);
        out.pillar = MakeTexture(renderer, bmp);
    }
    // Heart icon
    {
        Bitmap bmp(64, 64);
        bmp.FillCircle(22, 22, 13, 1.0f, 0.15f, 0.25f);
        bmp.FillCircle(42, 22, 13, 1.0f, 0.15f, 0.25f);
        bmp.FillTriangle(10, 26, 54, 26, 32, 52, 1.0f, 0.15f, 0.25f);
        bmp.FillRect(0, 0, 63, 0, 1, 1, 1, 0); // no-op keep alpha
        out.heart = MakeTexture(renderer, bmp);
    }

    // Meshes
    out.cube = gfx::Mesh::CreateCube(renderer, 1, 1, 1, "cube");
    out.sphere = gfx::Mesh::CreateSphere(renderer, 1.0f, 20, 12, "sphere");
    out.plane = gfx::Mesh::CreatePlane(renderer, 120, 120, 30, 30, "ground");
    out.cylinder = gfx::Mesh::CreateCylinder(renderer, 1.0f, 1.0f, 24, "cylinder");

    // Stylized humanoid knight made of boxes (single merged mesh).
    {
        std::vector<gfx::Vertex3D> verts;
        std::vector<uint16_t> indices;
        AddBoxToMesh(verts, indices, {0, 0.95f, 0}, {0.5f, 0.65f, 0.3f});    // torso
        AddBoxToMesh(verts, indices, {0, 1.55f, 0}, {0.3f, 0.3f, 0.3f});     // head
        AddBoxToMesh(verts, indices, {-0.13f, 0.3f, 0}, {0.18f, 0.6f, 0.22f}); // legs
        AddBoxToMesh(verts, indices, {0.13f, 0.3f, 0}, {0.18f, 0.6f, 0.22f});
        AddBoxToMesh(verts, indices, {-0.37f, 1.05f, 0}, {0.13f, 0.55f, 0.13f}); // arms
        AddBoxToMesh(verts, indices, {0.37f, 1.05f, 0}, {0.13f, 0.55f, 0.13f});
        AddBoxToMesh(verts, indices, {0.55f, 1.35f, 0.05f}, {0.07f, 1.05f, 0.07f}); // sword
        out.playerMesh = gfx::Mesh::CreateFromData(renderer, verts.data(),
                                                   static_cast<uint32_t>(verts.size()),
                                                   indices.data(),
                                                   static_cast<uint32_t>(indices.size()),
                                                   "player");
    }

    // Low-poly wolf made of boxes (body, head, ears, tail, legs).
    {
        std::vector<gfx::Vertex3D> verts;
        std::vector<uint16_t> indices;
        AddBoxToMesh(verts, indices, {0, 0.45f, 0}, {0.55f, 0.4f, 1.0f});       // body
        AddBoxToMesh(verts, indices, {0, 0.78f, 0.38f}, {0.28f, 0.26f, 0.3f});   // head
        AddBoxToMesh(verts, indices, {-0.09f, 0.92f, 0.45f}, {0.08f, 0.18f, 0.08f}); // ear L
        AddBoxToMesh(verts, indices, {0.09f, 0.92f, 0.45f}, {0.08f, 0.18f, 0.08f});  // ear R
        AddBoxToMesh(verts, indices, {0, 0.55f, -0.48f}, {0.07f, 0.08f, 0.28f}); // tail
        AddBoxToMesh(verts, indices, {-0.22f, 0.22f, 0.25f}, {0.13f, 0.44f, 0.14f}); // legs
        AddBoxToMesh(verts, indices, {0.22f, 0.22f, 0.25f}, {0.13f, 0.44f, 0.14f});
        AddBoxToMesh(verts, indices, {-0.22f, 0.22f, -0.25f}, {0.13f, 0.44f, 0.14f});
        AddBoxToMesh(verts, indices, {0.22f, 0.22f, -0.25f}, {0.13f, 0.44f, 0.14f});
        out.wolfMesh = gfx::Mesh::CreateFromData(renderer, verts.data(),
                                                 static_cast<uint32_t>(verts.size()),
                                                 indices.data(),
                                                 static_cast<uint32_t>(indices.size()),
                                                 "wolf");
    }

    // Kenney Nature Kit models (CC0, MIT-style license in pack).
    out.kenneyPine = assetMgr.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj");
    out.kenneyOak = assetMgr.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/tree_default.obj");
    out.kenneyRock = assetMgr.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/rock_largeA.obj");
    out.kenneyLog = assetMgr.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/log.obj");
}

} // namespace neon::demo
