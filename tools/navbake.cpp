// navbake: offline mesh -> nav grid baker.
//
// The authoritative authoring entry for .navgrid.json is the editor's
// "从选中 Mesh 烘焙" panel (editor/src/panels/nav_panel.cpp), which reuses the
// engine bake nav::BakeFromTriangles. This CLI is the headless equivalent for
// CI / asset regeneration: it reads a .glb/.gltf, flattens every mesh node to a
// world-space triangle soup (node transform + the scene entity's TRS) and calls
// the SAME engine bake, so both entries produce identical output.
//
//   navbake --mesh <in.glb> --out <out.navgrid.json> [options]
//     --cell <f>          grid cell size (default 1.0)
//     --radius <f>        agent radius, dilates blocked cells (default 0.6)
//     --clearance <f>     Y span that marks a cell an obstacle (default 1.6)
//     --yaw-deg <f>       scene entity yaw about Y (default 0)
//     --pos <x,z>         scene entity XZ position (default 0,0)
//     --scale <f>         scene entity uniform scale (default 1)
//     --carve <x,z,r>     force a walkable disk (repeatable)
//     --check <x,z>       print a cell report (repeatable)

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec3.hpp"
#include "neon/nav/nav_grid.hpp"

namespace {

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size > 0 ? size : 0));
    if (size > 0) in.read(reinterpret_cast<char*>(out->data()), size);
    return true;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

struct Gltf {
    neon::core::Json root;
    std::vector<std::vector<uint8_t>> bins;
};

// Splits a GLB container (or parses a .gltf + external buffers).
bool LoadGltf(const std::string& path, Gltf* out) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, &bytes)) return false;
    std::string jsonText;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        const uint32_t length = ReadU32(bytes.data() + 8);
        size_t off = 12;
        while (off + 8 <= bytes.size() && off < length) {
            const uint32_t clen = ReadU32(bytes.data() + off);
            const uint32_t ctype = ReadU32(bytes.data() + off + 4);
            off += 8;
            if (off + clen > bytes.size()) break;
            if (ctype == 0x4E4F534A) // 'JSON'
                jsonText.assign(reinterpret_cast<const char*>(bytes.data() + off), clen);
            else if (ctype == 0x004E4942) // 'BIN'
                out->bins.emplace_back(bytes.begin() + static_cast<long>(off),
                                       bytes.begin() + static_cast<long>(off + clen));
            off += clen;
        }
    } else {
        jsonText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::string err;
    out->root = neon::core::Json::Parse(jsonText, &err);
    if (!err.empty() || !out->root.IsObject()) return false;
    // .gltf with external buffers (only buffers[0] is supported headlessly).
    if (out->bins.empty()) {
        const neon::core::Json* buffers = out->root.Get("buffers");
        if (buffers && buffers->IsArray() && buffers->Size() > 0) {
            const neon::core::Json* uri = buffers->At(0)->Get("uri");
            if (uri && uri->IsString()) {
                const size_t slash = path.find_last_of("/\\");
                const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
                std::vector<uint8_t> bin;
                if (ReadFile(dir + uri->GetString(), &bin)) out->bins.push_back(std::move(bin));
            }
        }
    }
    return true;
}

int NumComponents(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

int ComponentSize(int componentType) {
    switch (componentType) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
    }
}

double ReadComponent(const uint8_t* p, int componentType) {
    switch (componentType) {
        case 5120: { int8_t v; std::memcpy(&v, p, 1); return v; }
        case 5121: return *p;
        case 5122: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        case 5126: { float v; std::memcpy(&v, p, 4); return v; }
        default: return 0.0;
    }
}

// Reads one accessor into `out` as flat doubles (component-major).
bool ReadAccessor(const Gltf& g, int index, std::vector<double>* out, int* ncompOut,
                  int* componentTypeOut) {
    const neon::core::Json* accessors = g.root.Get("accessors");
    const neon::core::Json* views = g.root.Get("bufferViews");
    if (!accessors || !views || index < 0 || index >= static_cast<int>(accessors->Size()))
        return false;
    const neon::core::Json* acc = accessors->At(static_cast<size_t>(index));
    const int bv = acc->Get("bufferView") ? acc->Get("bufferView")->GetInt(-1) : -1;
    if (bv < 0 || bv >= static_cast<int>(views->Size())) return false;
    const neon::core::Json* view = views->At(static_cast<size_t>(bv));
    const int buffer = view->Get("buffer") ? view->Get("buffer")->GetInt(0) : 0;
    if (buffer < 0 || buffer >= static_cast<int>(g.bins.size())) return false;
    const uint8_t* base = g.bins[static_cast<size_t>(buffer)].data();
    const int viewOff = view->Get("byteOffset") ? view->Get("byteOffset")->GetInt(0) : 0;
    const int accOff = acc->Get("byteOffset") ? acc->Get("byteOffset")->GetInt(0) : 0;
    const int componentType = acc->Get("componentType") ? acc->Get("componentType")->GetInt(0) : 0;
    const std::string type = acc->Get("type") ? acc->Get("type")->GetString() : "";
    const int count = acc->Get("count") ? acc->Get("count")->GetInt(0) : 0;
    const int ncomp = NumComponents(type);
    const int csz = ComponentSize(componentType);
    if (ncomp <= 0 || csz <= 0) return false;
    int stride = view->Get("byteStride") ? view->Get("byteStride")->GetInt(0) : 0;
    if (stride == 0) stride = csz * ncomp;
    out->resize(static_cast<size_t>(count) * ncomp);
    for (int i = 0; i < count; ++i) {
        const uint8_t* p = base + viewOff + accOff + static_cast<size_t>(i) * stride;
        for (int c = 0; c < ncomp; ++c)
            (*out)[static_cast<size_t>(i) * ncomp + c] =
                ReadComponent(p + static_cast<size_t>(c) * csz, componentType);
    }
    if (ncompOut) *ncompOut = ncomp;
    if (componentTypeOut) *componentTypeOut = componentType;
    return true;
}

neon::math::Mat4 NodeMatrix(const neon::core::Json* node) {
    if (const neon::core::Json* m = node->Get("matrix")) {
        if (m->IsArray() && m->Size() == 16) {
            neon::math::Mat4 r;
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    r.m[row * 4 + col] = static_cast<float>(m->At(col * 4 + row)->GetNumber());
            return r;
        }
    }
    neon::math::Vec3 t{0, 0, 0}, s{1, 1, 1};
    neon::math::Quat q{0, 0, 0, 1};
    if (const neon::core::Json* v = node->Get("translation"))
        if (v->IsArray() && v->Size() == 3)
            t = {static_cast<float>(v->At(0)->GetNumber()),
                 static_cast<float>(v->At(1)->GetNumber()),
                 static_cast<float>(v->At(2)->GetNumber())};
    if (const neon::core::Json* v = node->Get("scale"))
        if (v->IsArray() && v->Size() == 3)
            s = {static_cast<float>(v->At(0)->GetNumber()),
                 static_cast<float>(v->At(1)->GetNumber()),
                 static_cast<float>(v->At(2)->GetNumber())};
    if (const neon::core::Json* v = node->Get("rotation"))
        if (v->IsArray() && v->Size() == 4)
            q = {static_cast<float>(v->At(0)->GetNumber()),
                 static_cast<float>(v->At(1)->GetNumber()),
                 static_cast<float>(v->At(2)->GetNumber()),
                 static_cast<float>(v->At(3)->GetNumber())};
    neon::math::Mat4 r = neon::math::Mat4::Translation(t);
    r = r * q.ToMat4() * neon::math::Mat4::Scale(s);
    return r;
}

bool ParseVec2(const std::string& s, float* a, float* b) {
    return std::sscanf(s.c_str(), "%f,%f", a, b) == 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string meshPath, outPath;
    float cell = 1.0f, radius = 0.6f, clearance = 1.6f;
    float yawDeg = 0.0f, scale = 1.0f;
    float posX = 0.0f, posZ = 0.0f;
    std::vector<std::array<float, 3>> carve;
    std::vector<std::array<float, 2>> checks;
    std::vector<std::array<float, 4>> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* v) { if (i + 1 < argc) *v = argv[++i]; };
        if (a == "--mesh") next(&meshPath);
        else if (a == "--out") next(&outPath);
        else if (a == "--cell") { std::string v; next(&v); cell = std::stof(v); }
        else if (a == "--radius") { std::string v; next(&v); radius = std::stof(v); }
        else if (a == "--clearance") { std::string v; next(&v); clearance = std::stof(v); }
        else if (a == "--yaw-deg") { std::string v; next(&v); yawDeg = std::stof(v); }
        else if (a == "--scale") { std::string v; next(&v); scale = std::stof(v); }
        else if (a == "--pos") { std::string v; next(&v); ParseVec2(v, &posX, &posZ); }
        else if (a == "--carve") {
            std::string v; next(&v);
            std::array<float, 3> c{0, 0, 2.0f};
            const int n = std::sscanf(v.c_str(), "%f,%f,%f", &c[0], &c[1], &c[2]);
            if (n >= 2) carve.push_back(c);
        } else if (a == "--check") {
            std::string v; next(&v);
            std::array<float, 2> c{0, 0};
            if (ParseVec2(v, &c[0], &c[1])) checks.push_back(c);
        } else if (a == "--path") {
            std::string v; next(&v);
            std::array<float, 4> c{0, 0, 0, 0};
            if (std::sscanf(v.c_str(), "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4)
                paths.push_back(c);
        }
    }
    if (meshPath.empty() || outPath.empty()) {
        std::fprintf(stderr, "usage: navbake --mesh <in.glb> --out <out.navgrid.json> [...]\n");
        return 2;
    }

    Gltf gltf;
    if (!LoadGltf(meshPath, &gltf)) {
        std::fprintf(stderr, "navbake: cannot read '%s'\n", meshPath.c_str());
        return 1;
    }
    const neon::math::Mat4 entity =
        neon::math::Mat4::Translation({posX, 0.0f, posZ}) *
        neon::math::Mat4::RotationY(yawDeg * 3.14159265358979f / 180.0f) *
        neon::math::Mat4::Scale({scale, scale, scale});

    std::vector<neon::math::Vec3> positions;
    std::vector<uint32_t> indices;
    const neon::core::Json* nodes = gltf.root.Get("nodes");
    const neon::core::Json* meshes = gltf.root.Get("meshes");
    if (!nodes || !meshes) {
        std::fprintf(stderr, "navbake: glTF has no nodes/meshes\n");
        return 1;
    }

    // Walk the scene graph, accumulating world matrices, and flatten every
    // mesh node's primitives into one triangle soup.
    std::function<void(int, const neon::math::Mat4&)> visit =
        [&](int idx, const neon::math::Mat4& parent) {
            if (idx < 0 || idx >= static_cast<int>(nodes->Size())) return;
            const neon::core::Json* node = nodes->At(static_cast<size_t>(idx));
            const neon::math::Mat4 world = parent * NodeMatrix(node);
            const int meshIndex = node->Get("mesh") ? node->Get("mesh")->GetInt(-1) : -1;
            if (meshIndex >= 0 && meshIndex < static_cast<int>(meshes->Size())) {
                const neon::core::Json* prims = meshes->At(static_cast<size_t>(meshIndex))->Get("primitives");
                if (prims && prims->IsArray()) {
                    for (size_t pi = 0; pi < prims->Size(); ++pi) {
                        const neon::core::Json* attrs = prims->At(pi)->Get("attributes");
                        if (!attrs) continue;
                        const neon::core::Json* posAcc = attrs->Get("POSITION");
                        if (!posAcc) continue;
                        std::vector<double> pos;
                        int ncomp = 0, ctype = 0;
                        if (!ReadAccessor(gltf, posAcc->GetInt(-1), &pos, &ncomp, &ctype)) continue;
                        if (ncomp != 3) continue;
                        const uint32_t base = static_cast<uint32_t>(positions.size());
                        for (size_t v = 0; v < pos.size(); v += 3) {
                            neon::math::Vec3 p{static_cast<float>(pos[v]),
                                               static_cast<float>(pos[v + 1]),
                                               static_cast<float>(pos[v + 2])};
                            positions.push_back(entity.TransformPoint(world.TransformPoint(p)));
                        }
                        const neon::core::Json* idxAcc = prims->At(pi)->Get("indices");
                        if (idxAcc) {
                            std::vector<double> ind;
                            int incomp = 0, ict = 0;
                            if (ReadAccessor(gltf, idxAcc->GetInt(-1), &ind, &incomp, &ict)) {
                                for (double d : ind)
                                    indices.push_back(base + static_cast<uint32_t>(d));
                            }
                        } else {
                            for (size_t v = 0; v < pos.size() / 3; ++v)
                                indices.push_back(base + static_cast<uint32_t>(v));
                        }
                    }
                }
            }
            const neon::core::Json* children = node->Get("children");
            if (children && children->IsArray())
                for (size_t c = 0; c < children->Size(); ++c)
                    visit(children->At(c)->GetInt(-1), world);
        };
    const neon::core::Json* scenes = gltf.root.Get("scenes");
    const int scene = gltf.root.Get("scene") ? gltf.root.Get("scene")->GetInt(0) : 0;
    if (scenes && scenes->IsArray() && scene < static_cast<int>(scenes->Size())) {
        const neon::core::Json* roots = scenes->At(static_cast<size_t>(scene))->Get("nodes");
        if (roots && roots->IsArray())
            for (size_t r = 0; r < roots->Size(); ++r)
                visit(roots->At(r)->GetInt(-1), neon::math::Mat4::Identity());
    }

    std::printf("navbake: %zu vertices, %zu indices (%zu triangles)\n", positions.size(),
                indices.size(), indices.size() / 3);
    if (indices.size() < 3) {
        std::fprintf(stderr, "navbake: no triangles\n");
        return 1;
    }

    neon::nav::BakeParams params;
    params.cellSize = cell;
    params.agentRadius = radius;
    params.clearance = clearance;
    for (const auto& c : carve) params.forceWalkable.push_back({{c[0], c[1]}, c[2]});
    auto baked = neon::nav::BakeFromTriangles(positions.data(), positions.size(),
                                              indices.data(), indices.size(), params);
    if (!baked.Ok()) {
        std::fprintf(stderr, "navbake: %s\n", baked.Error().c_str());
        return 1;
    }
    const neon::nav::NavGrid& grid = baked.Value();
    for (const auto& c : checks) {
        int cx = 0, cz = 0;
        const bool in = grid.WorldToCell({c[0], c[1]}, &cx, &cz);
        std::printf("check (%.1f,%.1f): %s\n", c[0], c[1],
                    in ? (grid.Walkable(cx, cz) ? "walkable" : "blocked") : "out-of-bounds");
    }
    for (const auto& p : paths) {
        auto path = grid.FindPath({p[0], p[1]}, {p[2], p[3]});
        std::printf("path (%.0f,%.0f)->(%.0f,%.0f): %zu waypoints\n", p[0], p[1], p[2], p[3],
                    path.size());
    }
    auto json = grid.ToJson();
    if (!json.Ok()) {
        std::fprintf(stderr, "navbake: %s\n", json.Error().c_str());
        return 1;
    }
    std::ofstream out(outPath, std::ios::binary);
    if (!out.is_open()) {
        std::fprintf(stderr, "navbake: cannot write '%s'\n", outPath.c_str());
        return 1;
    }
    out << neon::core::JsonWriter::WritePretty(json.Value()) << "\n";
    std::printf("navbake: wrote %s (%d x %d)\n", outPath.c_str(), grid.Width(), grid.Height());
    return 0;
}
