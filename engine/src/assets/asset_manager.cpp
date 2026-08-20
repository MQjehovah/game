#include "neon/assets/asset_manager.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "neon/core/log.hpp"
#include "neon/core/json.hpp"
#include "neon/math/quat.hpp"

namespace neon::assets {

namespace {

std::string DirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos + 1);
}

void LoadMaterialColors(const std::string& mtlPath,
                        std::map<std::string, math::Vec4>& out) {
    std::ifstream in(mtlPath);
    if (!in.is_open()) return;
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "newmtl") {
            ss >> current;
        } else if (kind == "Kd") {
            float r = 1, g = 1, b = 1;
            ss >> r >> g >> b;
            out[current] = {r, g, b, 1.0f};
        }
        // map_Kd (textures) is not consumed by this pipeline yet.
    }
}

struct FaceIndex {
    int v = 0;
    int t = 0;
    int n = 0;
    FaceIndex(int v_, int t_, int n_) : v(v_), t(t_), n(n_) {}
};

struct GltfAccessorLayout {
    const uint8_t* base = nullptr;
    int stride = 0;
    int count = 0;
};

int GltfComponentSize(int ct) {
    switch (ct) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
    }
}

int GltfComponentCount(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT4") return 16;
    return 0;
}

// Resolves an accessor against the parsed buffer views / binary into a
// contiguous-window descriptor honoring bufferView byteStride and accessor
// byteOffset. Shared by the mesh importer and GltfAsset::ReadAccessorFloats.
bool ResolveGltfAccessor(const std::vector<uint8_t>& bin,
                         const std::vector<assets::GltfBufferView>& views,
                         const std::vector<assets::GltfAccessor>& accs,
                         int index, GltfAccessorLayout& out) {
    if (index < 0 || index >= static_cast<int>(accs.size())) return false;
    const assets::GltfAccessor& acc = accs[static_cast<size_t>(index)];
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(views.size())) return false;
    const assets::GltfBufferView& view = views[static_cast<size_t>(acc.bufferView)];
    int compSize = GltfComponentSize(acc.componentType);
    int compCount = GltfComponentCount(acc.type);
    if (compSize == 0 || compCount == 0 || acc.count < 0) return false;
    int stride = view.byteStride != 0 ? view.byteStride : compSize * compCount;
    if (stride <= 0) return false;
    int64_t base = static_cast<int64_t>(view.byteOffset) + acc.byteOffset;
    int64_t lastElementEnd = base + static_cast<int64_t>(stride) * (acc.count - 1) +
                             compSize * compCount;
    if (base < 0 || lastElementEnd > static_cast<int64_t>(bin.size())) return false;
    out.base = bin.data() + base;
    out.stride = stride;
    out.count = acc.count;
    return true;
}

// Decomposes a row-major T*R*S matrix into a unit quaternion (scale baked into
// the row lengths is normalized away; mirror/shear decompositions are not
// handled and yield the closest rotation).
math::Quat Mat4ToQuat(const math::Mat4& m) {
    auto row = [&](int r) {
        return math::Vec3{m.m[r * 4 + 0], m.m[r * 4 + 1], m.m[r * 4 + 2]}.Normalized();
    };
    math::Vec3 r0 = row(0), r1 = row(1), r2 = row(2);
    float trace = r0.x + r1.y + r2.z;
    math::Quat q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r1.z - r2.y) / s;
        q.y = (r2.x - r0.z) / s;
        q.z = (r0.y - r1.x) / s;
    } else if (r0.x > r1.y && r0.x > r2.z) {
        float s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f;
        q.w = (r1.z - r2.y) / s;
        q.x = 0.25f * s;
        q.y = (r0.y + r1.x) / s;
        q.z = (r2.x + r0.z) / s;
    } else if (r1.y > r2.z) {
        float s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f;
        q.w = (r2.x - r0.z) / s;
        q.x = (r0.y + r1.x) / s;
        q.y = 0.25f * s;
        q.z = (r1.z + r2.y) / s;
    } else {
        float s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f;
        q.w = (r0.y - r1.x) / s;
        q.x = (r2.x + r0.z) / s;
        q.y = (r1.z + r2.y) / s;
        q.z = 0.25f * s;
    }
    return q.Normalized();
}

} // namespace

core::Result<std::vector<float>> GltfAsset::ReadAccessorFloats(int accessorIndex) const {
    GltfAccessorLayout layout;
    if (!ResolveGltfAccessor(rawBin, bufferViews, accessors, accessorIndex, layout))
        return core::Result<std::vector<float>>::Err("gltf: cannot resolve accessor");
    const GltfAccessor& acc = accessors[static_cast<size_t>(accessorIndex)];
    int comps = GltfComponentCount(acc.type);
    if (comps == 0)
        return core::Result<std::vector<float>>::Err("gltf: unsupported accessor type");
    std::vector<float> out(static_cast<size_t>(layout.count) * static_cast<size_t>(comps));
    const uint8_t* base = layout.base;
    for (int i = 0; i < layout.count; ++i) {
        const uint8_t* src = base + static_cast<size_t>(i) * layout.stride;
        for (int c = 0; c < comps; ++c) {
            float v = 0.0f;
            switch (acc.componentType) {
                case 5120: v = static_cast<float>(static_cast<int8_t>(src[c])); break;
                case 5121: v = static_cast<float>(src[c]); break;
                case 5122: v = static_cast<float>(reinterpret_cast<const int16_t*>(src)[c]); break;
                case 5123: v = static_cast<float>(reinterpret_cast<const uint16_t*>(src)[c]); break;
                case 5125: v = static_cast<float>(reinterpret_cast<const uint32_t*>(src)[c]); break;
                case 5126: v = reinterpret_cast<const float*>(src)[c]; break;
                default:
                    return core::Result<std::vector<float>>::Err("gltf: unsupported component type");
            }
            out[static_cast<size_t>(i) * static_cast<size_t>(comps) + static_cast<size_t>(c)] = v;
        }
    }
    return core::Result<std::vector<float>>::Ok(std::move(out));
}

gfx::Texture AssetManager::LoadTexture(const std::string& path) {
    auto cached = textures_.find(path);
    if (cached != textures_.end()) return cached->second;

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        NEON_LOG_ERROR("Asset: failed to load texture '%s'", path.c_str());
        return {};
    }
    gfx::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.rgba = data;
    desc.mipmaps = true;
    gfx::Texture texture = renderer_->CreateTexture(desc);
    stbi_image_free(data);
    textures_[path] = texture;
    NEON_LOG_INFO("Asset: loaded texture '%s' (%dx%d)", path.c_str(), w, h);
    return texture;
}

gfx::Mesh AssetManager::LoadMeshOBJ(const std::string& path) {
    auto cached = meshes_.find(path);
    if (cached != meshes_.end()) return cached->second;

    std::ifstream in(path);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Asset: failed to open OBJ '%s'", path.c_str());
        return {};
    }

    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<math::Vec2> uvs;
    std::vector<gfx::Vertex3D> verts;
    std::vector<uint16_t> indices;
    std::map<std::string, math::Vec4> materialColors;
    std::string currentMaterial;

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "mtllib") {
            std::string mtlFile;
            ss >> mtlFile;
            LoadMaterialColors(DirName(path) + mtlFile, materialColors);
        } else if (kind == "usemtl") {
            ss >> currentMaterial;
        } else if (kind == "v") {
            float x, y, z;
            ss >> x >> y >> z;
            positions.push_back({x, y, z});
        } else if (kind == "vn") {
            float x, y, z;
            ss >> x >> y >> z;
            normals.push_back({x, y, z});
        } else if (kind == "vt") {
            float u, v;
            ss >> u >> v;
            uvs.push_back({u, v});
        } else if (kind == "f") {
            std::vector<FaceIndex> face;
            std::string token;
            while (ss >> token) {
                size_t s1 = token.find('/');
                size_t s2 = s1 == std::string::npos ? std::string::npos : token.find('/', s1 + 1);
                int vi = std::atoi(token.substr(0, s1).c_str());
                int ti = 0;
                int ni = 0;
                if (s1 != std::string::npos) {
                    if (s2 != std::string::npos && s2 != s1 + 1) {
                        ti = std::atoi(token.substr(s1 + 1, s2 - s1 - 1).c_str());
                    }
                    if (s2 != std::string::npos) ni = std::atoi(token.substr(s2 + 1).c_str());
                }
                if (vi < 0) vi += static_cast<int>(positions.size()) + 1;
                if (ti < 0) ti += static_cast<int>(uvs.size()) + 1;
                if (ni < 0) ni += static_cast<int>(normals.size()) + 1;
                face.emplace_back(vi, ti, ni);
            }
            if (face.size() < 3) continue;

            // Flat normal fallback when the face lacks vertex normals.
            bool allNormals = true;
            for (const FaceIndex& f : face) {
                if (f.n <= 0 || f.n > static_cast<int>(normals.size())) allNormals = false;
            }
            math::Vec3 faceNormal{0, 1, 0};
            if (!allNormals) {
                if (face[0].v > 0 && face[0].v <= static_cast<int>(positions.size()) &&
                    face[1].v > 0 && face[1].v <= static_cast<int>(positions.size()) &&
                    face[2].v > 0 && face[2].v <= static_cast<int>(positions.size())) {
                    math::Vec3 a = positions[face[0].v - 1];
                    math::Vec3 b = positions[face[1].v - 1];
                    math::Vec3 c = positions[face[2].v - 1];
                    faceNormal = math::Cross(b - a, c - a).Normalized();
                    if (faceNormal.LengthSq() < 0.5f) faceNormal = {0, 1, 0};
                }
            }

            auto colorIt = materialColors.find(currentMaterial);
            math::Vec4 color = colorIt != materialColors.end() ? colorIt->second
                                                               : math::Vec4{1, 1, 1, 1};
            uint16_t base = static_cast<uint16_t>(verts.size());
            for (size_t i = 0; i < face.size(); ++i) {
                const FaceIndex& f = face[i];
                if (f.v <= 0 || f.v > static_cast<int>(positions.size())) {
                    NEON_LOG_WARN("OBJ '%s': bad vertex index %d", path.c_str(), f.v);
                    return {};
                }
                math::Vec3 normal = allNormals ? normals[f.n - 1] : faceNormal;
                math::Vec2 uv{0, 0};
                if (f.t > 0 && f.t <= static_cast<int>(uvs.size())) uv = uvs[f.t - 1];
                verts.push_back({positions[f.v - 1], normal, uv, color});
            }
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                indices.push_back(base);
                indices.push_back(base + static_cast<uint16_t>(i));
                indices.push_back(base + static_cast<uint16_t>(i + 1));
            }
        }
    }

    if (verts.empty()) {
        NEON_LOG_ERROR("OBJ '%s': no vertices parsed", path.c_str());
        return {};
    }
    gfx::Mesh mesh = gfx::Mesh::CreateFromData(*renderer_, verts.data(),
                                               static_cast<uint32_t>(verts.size()),
                                               indices.data(), static_cast<uint32_t>(indices.size()),
                                               path);
    meshes_[path] = mesh;
    NEON_LOG_INFO("Asset: loaded OBJ '%s' (%zu verts)", path.c_str(), verts.size());
    return mesh;
}

GltfAsset AssetManager::LoadGLTF(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        NEON_LOG_ERROR("GLTF: failed to open '%s'", path.c_str());
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string parseError;
    core::Json root = core::Json::Parse(ss.str(), &parseError);
    if (root.IsNull()) {
        NEON_LOG_ERROR("GLTF: JSON parse error: %s", parseError.c_str());
        return {};
    }
    std::string dir = DirName(path);

    // Binary buffer.
    std::vector<uint8_t> bin;
    if (const core::Json* buffers = root.Get("buffers"); buffers && buffers->Size() > 0) {
        const core::Json* b0 = buffers->At(0);
        if (b0 && b0->Get("uri")) {
            std::string uri = b0->Get("uri")->GetString();
            std::ifstream binFile(dir + uri, std::ios::binary);
            if (binFile.is_open()) {
                bin.assign(std::istreambuf_iterator<char>(binFile), std::istreambuf_iterator<char>());
            } else {
                NEON_LOG_ERROR("GLTF: cannot open buffer '%s'", (dir + uri).c_str());
                return {};
            }
        }
    }
    if (bin.empty()) {
        NEON_LOG_ERROR("GLTF: empty binary buffer in '%s'", path.c_str());
        return {};
    }

    // Parse bufferViews / accessors into plain structs shared by the mesh
    // importer below and GltfAsset::ReadAccessorFloats (animation samplers).
    std::vector<GltfBufferView> parsedViews;
    if (const core::Json* bvs = root.Get("bufferViews")) {
        for (size_t i = 0; i < bvs->Size(); ++i) {
            const core::Json* v = bvs->At(i);
            if (!v) continue;
            GltfBufferView bv;
            bv.buffer = v->Get("buffer") ? v->Get("buffer")->GetInt(0) : 0;
            bv.byteOffset = v->Get("byteOffset") ? v->Get("byteOffset")->GetInt(0) : 0;
            bv.byteLength = v->Get("byteLength") ? v->Get("byteLength")->GetInt(0) : 0;
            bv.byteStride = v->Get("byteStride") ? v->Get("byteStride")->GetInt(0) : 0;
            parsedViews.push_back(bv);
        }
    }
    std::vector<GltfAccessor> parsedAccessors;
    if (const core::Json* accs = root.Get("accessors")) {
        for (size_t i = 0; i < accs->Size(); ++i) {
            const core::Json* a = accs->At(i);
            if (!a) continue;
            GltfAccessor ga;
            ga.bufferView = a->Get("bufferView") ? a->Get("bufferView")->GetInt(-1) : -1;
            ga.byteOffset = a->Get("byteOffset") ? a->Get("byteOffset")->GetInt(0) : 0;
            ga.componentType = a->Get("componentType") ? a->Get("componentType")->GetInt(0) : 0;
            ga.count = a->Get("count") ? a->Get("count")->GetInt(0) : 0;
            ga.type = a->Get("type") ? a->Get("type")->GetString() : std::string();
            parsedAccessors.push_back(ga);
        }
    }
    auto readAccessor = [&](int accessorIndex, const uint8_t** outBase, int& outStride,
                            int& outCount) -> bool {
        GltfAccessorLayout layout;
        if (!ResolveGltfAccessor(bin, parsedViews, parsedAccessors, accessorIndex, layout))
            return false;
        *outBase = layout.base;
        outStride = layout.stride;
        outCount = layout.count;
        return true;
    };

    // Textures.
    std::vector<gfx::Texture> textures;
    if (const core::Json* texs = root.Get("textures")) {
        const core::Json* images = root.Get("images");
        for (size_t i = 0; i < texs->Size(); ++i) {
            int src = -1;
            if (const core::Json* srcNode = texs->At(i)->Get("source")) src = srcNode->GetInt(-1);
            std::string uri;
            if (images && src >= 0 && images->At(src) && images->At(src)->Get("uri")) {
                uri = images->At(src)->Get("uri")->GetString();
            }
            textures.push_back(uri.empty() ? gfx::Texture{} : LoadTexture(dir + uri));
        }
    }

    // Materials.
    std::vector<gfx::Material> materials;
    if (const core::Json* mats = root.Get("materials")) {
        for (size_t i = 0; i < mats->Size(); ++i) {
            const core::Json* m = mats->At(i);
            gfx::Material mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
            if (const core::Json* pbr = m->Get("pbrMetallicRoughness")) {
                if (const core::Json* bc = pbr->Get("baseColorTexture")) {
                    int idx = bc->Get("index")->GetInt(-1);
                    if (idx >= 0 && idx < static_cast<int>(textures.size())) mat.albedo = textures[idx].Handle();
                }
                if (const core::Json* mr = pbr->Get("metallicRoughnessTexture")) {
                    int idx = mr->Get("index")->GetInt(-1);
                    if (idx >= 0 && idx < static_cast<int>(textures.size()))
                        mat.metallicRoughness = textures[idx].Handle();
                }
                if (const core::Json* bcf = pbr->Get("baseColorFactor")) {
                    if (bcf->Size() >= 3) {
                        mat.tint = {static_cast<float>(bcf->At(0)->GetNumber()),
                                    static_cast<float>(bcf->At(1)->GetNumber()),
                                    static_cast<float>(bcf->At(2)->GetNumber()),
                                    bcf->Size() > 3 ? static_cast<float>(bcf->At(3)->GetNumber()) : 1.0f};
                    }
                }
                const core::Json* mf = pbr->Get("metallicFactor");
                const core::Json* rf = pbr->Get("roughnessFactor");
                mat.metallic = static_cast<float>(mf ? mf->GetNumber(0.0) : 0.0);
                mat.roughness = static_cast<float>(rf ? rf->GetNumber(1.0) : 1.0);
            }
            if (const core::Json* occ = m->Get("occlusionTexture")) {
                int idx = occ->Get("index")->GetInt(-1);
                if (idx >= 0 && idx < static_cast<int>(textures.size())) mat.occlusion = textures[idx].Handle();
            }
            if (const core::Json* emi = m->Get("emissiveTexture")) {
                int idx = emi->Get("index")->GetInt(-1);
                if (idx >= 0 && idx < static_cast<int>(textures.size())) mat.emissive = textures[idx].Handle();
            }
            materials.push_back(mat);
        }
    }

    // Meshes.
    struct RawMesh {
        std::vector<gfx::Vertex3D> verts;
        std::vector<uint16_t> indices;
        gfx::Material material;
        std::vector<uint16_t> jointIds;   // 4 per vertex
        std::vector<float> jointWeights;  // 4 per vertex
        bool skinned = false;
    };
    std::vector<RawMesh> rawMeshes;
    if (const core::Json* meshes = root.Get("meshes")) {
        for (size_t mi = 0; mi < meshes->Size(); ++mi) {
            const core::Json* prims = meshes->At(mi)->Get("primitives");
            if (!prims) continue;
            for (size_t pi = 0; pi < prims->Size(); ++pi) {
                const core::Json* prim = prims->At(pi);
                const core::Json* attrs = prim->Get("attributes");
                if (!attrs) continue;
                RawMesh rm;
                int matIdx = -1;
                if (const core::Json* matNode = prim->Get("material")) matIdx = matNode->GetInt(-1);
                rm.material = matIdx >= 0 && matIdx < static_cast<int>(materials.size())
                                  ? materials[matIdx]
                                  : gfx::Material::Lit({}, gfx::Color::White);

                const uint8_t* posBase = nullptr;
                int posStride = 0, posCount = 0;
                const uint8_t* nrmBase = nullptr;
                int nrmStride = 0, nrmCount = 0;
                const uint8_t* uvBase = nullptr;
                int uvStride = 0, uvCount = 0;
                if (const core::Json* p = attrs->Get("POSITION"))
                    readAccessor(p->GetInt(), &posBase, posStride, posCount);
                if (const core::Json* n = attrs->Get("NORMAL"))
                    readAccessor(n->GetInt(), &nrmBase, nrmStride, nrmCount);
                if (const core::Json* t = attrs->Get("TEXCOORD_0"))
                    readAccessor(t->GetInt(), &uvBase, uvStride, uvCount);
                int jCount = 0;
                if (const core::Json* j = attrs->Get("JOINTS_0")) {
                    const uint8_t* jBase = nullptr;
                    int jStride = 0;
                    if (readAccessor(j->GetInt(), &jBase, jStride, jCount)) {
                        int jidx = j->GetInt();
                        int jct = (jidx >= 0 && jidx < static_cast<int>(parsedAccessors.size()))
                                      ? parsedAccessors[static_cast<size_t>(jidx)].componentType
                                      : 0;
                        if (jct == 5121) {
                            rm.jointIds.resize(static_cast<size_t>(jCount) * 4);
                            for (int v = 0; v < jCount; ++v) {
                                const uint8_t* src = jBase + v * jStride;
                                for (int c = 0; c < 4; ++c)
                                    rm.jointIds[static_cast<size_t>(v) * 4 + c] = src[c];
                            }
                        } else if (jct == 5123) {
                            rm.jointIds.resize(static_cast<size_t>(jCount) * 4);
                            for (int v = 0; v < jCount; ++v) {
                                const uint16_t* src =
                                    reinterpret_cast<const uint16_t*>(jBase + v * jStride);
                                for (int c = 0; c < 4; ++c)
                                    rm.jointIds[static_cast<size_t>(v) * 4 + c] = src[c];
                            }
                        }
                    }
                }
                int wCount = 0;
                if (const core::Json* w = attrs->Get("WEIGHTS_0")) {
                    const uint8_t* wBase = nullptr;
                    int wStride = 0;
                    if (readAccessor(w->GetInt(), &wBase, wStride, wCount)) {
                        rm.jointWeights.resize(static_cast<size_t>(wCount) * 4);
                        for (int v = 0; v < wCount; ++v) {
                            const float* src =
                                reinterpret_cast<const float*>(wBase + v * wStride);
                            for (int c = 0; c < 4; ++c)
                                rm.jointWeights[static_cast<size_t>(v) * 4 + c] = src[c];
                        }
                    }
                }
                if (!rm.jointIds.empty() && !rm.jointWeights.empty()) {
                    if (jCount == posCount && wCount == posCount) {
                        rm.skinned = true;
                    } else {
                        rm.jointIds.clear();
                        rm.jointWeights.clear();
                        NEON_LOG_WARN("GLTF: JOINTS_0/WEIGHTS_0 count (%d/%d) != POSITION count (%d)",
                                      jCount, wCount, posCount);
                    }
                }
                if (!posBase || posCount == 0) continue;
                rm.verts.resize(static_cast<size_t>(posCount));
                for (int v = 0; v < posCount; ++v) {
                    gfx::Vertex3D& out = rm.verts[static_cast<size_t>(v)];
                    const float* pos = reinterpret_cast<const float*>(posBase + v * posStride);
                    out.pos = {pos[0], pos[1], pos[2]};
                    out.color = {1, 1, 1, 1};
                    if (nrmBase && v < nrmCount) {
                        const float* n = reinterpret_cast<const float*>(nrmBase + v * nrmStride);
                        out.normal = {n[0], n[1], n[2]};
                    }
                    if (uvBase && v < uvCount) {
                        const float* uv = reinterpret_cast<const float*>(uvBase + v * uvStride);
                        out.uv = {uv[0], uv[1]};
                    }
                }
                const uint8_t* idxBase = nullptr;
                int idxStride = 0, idxCount = 0;
                if (const core::Json* idx = prim->Get("indices")) {
                    readAccessor(idx->GetInt(), &idxBase, idxStride, idxCount);
                    rm.indices.resize(static_cast<size_t>(idxCount));
                    if (idxStride == 2) {
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(idxBase);
                        for (int i = 0; i < idxCount; ++i) rm.indices[static_cast<size_t>(i)] = src[i];
                    } else if (idxStride == 4) {
                        const uint32_t* src = reinterpret_cast<const uint32_t*>(idxBase);
                        for (int i = 0; i < idxCount; ++i) {
                            if (src[i] > 65535u) {
                                NEON_LOG_WARN("GLTF: index %u exceeds 16-bit range", src[i]);
                                rm.indices.clear();
                                break;
                            }
                            rm.indices[static_cast<size_t>(i)] = static_cast<uint16_t>(src[i]);
                        }
                    }
                }
                rawMeshes.push_back(std::move(rm));
            }
        }
    }
    if (rawMeshes.empty()) {
        NEON_LOG_ERROR("GLTF: no meshes parsed from '%s'", path.c_str());
        return {};
    }

    // Nodes.
    struct NodeInfo {
        int mesh = -1;
        int skin = -1;
        math::Mat4 transform;
        std::vector<int> children;
        math::Vec3 translation{0, 0, 0};
        math::Quat rotation{};
        math::Vec3 scale{1, 1, 1};
        std::string name;
    };
    std::vector<NodeInfo> nodes;
    if (const core::Json* nodesJson = root.Get("nodes")) {
        nodes.resize(nodesJson->Size());
        for (size_t i = 0; i < nodesJson->Size(); ++i) {
            const core::Json* n = nodesJson->At(i);
            NodeInfo& info = nodes[i];
            if (const core::Json* nameNode = n->Get("name")) info.name = nameNode->GetString();
            if (const core::Json* meshNode = n->Get("mesh")) info.mesh = meshNode->GetInt(-1);
            if (const core::Json* skinNode = n->Get("skin")) info.skin = skinNode->GetInt(-1);
            if (const core::Json* matrix = n->Get("matrix")) {
                if (matrix->Size() == 16) {
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            info.transform.m[r * 4 + c] =
                                static_cast<float>(matrix->At(r * 4 + c)->GetNumber());
                        }
                    }
                    // Decompose T*R*S so the skeleton can build per-bone TRS.
                    info.translation = {info.transform.m[3], info.transform.m[7],
                                        info.transform.m[11]};
                    info.rotation = Mat4ToQuat(info.transform);
                    math::Mat4 rts = info.transform;
                    rts.m[3] = rts.m[7] = rts.m[11] = 0.0f;
                    math::Vec3 s{std::sqrt(rts.m[0] * rts.m[0] + rts.m[1] * rts.m[1] +
                                           rts.m[2] * rts.m[2]),
                                 std::sqrt(rts.m[4] * rts.m[4] + rts.m[5] * rts.m[5] +
                                           rts.m[6] * rts.m[6]),
                                 std::sqrt(rts.m[8] * rts.m[8] + rts.m[9] * rts.m[9] +
                                           rts.m[10] * rts.m[10])};
                    info.scale = s;
                }
            } else {
                if (const core::Json* translation = n->Get("translation")) {
                    info.translation = {static_cast<float>(translation->At(0)->GetNumber()),
                                        static_cast<float>(translation->At(1)->GetNumber()),
                                        static_cast<float>(translation->At(2)->GetNumber())};
                }
                if (const core::Json* scale = n->Get("scale")) {
                    info.scale = {static_cast<float>(scale->At(0)->GetNumber()),
                                  static_cast<float>(scale->At(1)->GetNumber()),
                                  static_cast<float>(scale->At(2)->GetNumber())};
                }
                if (const core::Json* rotation = n->Get("rotation")) {
                    info.rotation = {static_cast<float>(rotation->At(0)->GetNumber()),
                                     static_cast<float>(rotation->At(1)->GetNumber()),
                                     static_cast<float>(rotation->At(2)->GetNumber()),
                                     static_cast<float>(rotation->At(3)->GetNumber())};
                }
                info.transform = math::Mat4::Translation(info.translation) * info.rotation.ToMat4() *
                                 math::Mat4::Scale(info.scale);
            }
            if (const core::Json* children = n->Get("children")) {
                for (size_t c = 0; c < children->Size(); ++c) {
                    info.children.push_back(children->At(c)->GetInt());
                }
            }
        }
    }

    // Skins: joint node chains + one inverse-bind matrix per joint. A skin whose
    // inverseBindMatrices accessor is missing or unreadable keeps its joints and
    // an empty inverseBind list (consumers must handle the mismatch gracefully).
    std::vector<gfx::Skin> skins;
    if (const core::Json* skinsJson = root.Get("skins")) {
        for (size_t si = 0; si < skinsJson->Size(); ++si) {
            const core::Json* s = skinsJson->At(si);
            if (!s) continue;
            gfx::Skin skin;
            if (const core::Json* joints = s->Get("joints")) {
                for (size_t j = 0; j < joints->Size(); ++j) {
                    int nodeIdx = joints->At(j)->GetInt(-1);
                    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(nodes.size())) {
                        NEON_LOG_WARN("GLTF: skin %zu joint %zu index %d out of node range; using 0",
                                      si, j, nodeIdx);
                        nodeIdx = 0;
                    }
                    skin.joints.push_back(static_cast<uint32_t>(nodeIdx));
                }
            }
            if (const core::Json* ibm = s->Get("inverseBindMatrices")) {
                const uint8_t* ibmBase = nullptr;
                int ibmStride = 0, ibmCount = 0;
                if (readAccessor(ibm->GetInt(), &ibmBase, ibmStride, ibmCount)) {
                    skin.inverseBind.resize(static_cast<size_t>(ibmCount));
                    for (int m = 0; m < ibmCount; ++m) {
                        const float* src =
                            reinterpret_cast<const float*>(ibmBase + m * ibmStride);
                        math::Mat4 mat;
                        for (int c = 0; c < 16; ++c) mat.m[c] = src[c];
                        skin.inverseBind[static_cast<size_t>(m)] = mat;
                    }
                }
            }
            skins.push_back(std::move(skin));
        }
    }

    GltfAsset out;
    out.rawBin = std::move(bin);
    out.bufferViews = std::move(parsedViews);
    out.accessors = std::move(parsedAccessors);
    out.skins = std::move(skins);

    // Full node table: every glTF node (mesh, joint, or transform-only) with
    // its local TRS and parent, indexed by glTF node index. Skins reference
    // joints and animation channels reference targets by that index.
    out.nodesAll.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        GltfNode& gn = out.nodesAll[static_cast<size_t>(i)];
        gn.t = nodes[i].translation;
        gn.r = nodes[i].rotation;
        gn.s = nodes[i].scale;
        gn.name = nodes[i].name;
        gn.parent = -1;
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (int child : nodes[i].children) {
            if (child >= 0 && child < static_cast<int>(nodes.size()))
                out.nodesAll[static_cast<size_t>(child)].parent = static_cast<int>(i);
        }
    }

    std::function<void(int, const math::Mat4&)> visit = [&](int idx, const math::Mat4& parent) {
        if (idx < 0 || idx >= static_cast<int>(nodes.size())) return;
        const NodeInfo& n = nodes[static_cast<size_t>(idx)];
        math::Mat4 world = parent * n.transform;
        if (n.mesh >= 0 && n.mesh < static_cast<int>(rawMeshes.size())) {
            const RawMesh& rm = rawMeshes[static_cast<size_t>(n.mesh)];
            gfx::Mesh mesh = gfx::Mesh::CreateFromData(*renderer_, rm.verts.data(),
                                                       static_cast<uint32_t>(rm.verts.size()),
                                                       rm.indices.data(),
                                                       static_cast<uint32_t>(rm.indices.size()),
                                                       "gltf");
            if (mesh.Valid()) {
                if (rm.skinned)
                    mesh.AttachSkinData(rm.jointIds, rm.jointWeights, n.skin);
                out.nodes.push_back({world, mesh, rm.material});
            }
        }
        for (int child : n.children) visit(child, world);
    };
    if (const core::Json* scenes = root.Get("scenes")) {
        if (scenes->Size() > 0) {
            if (const core::Json* sceneNodes = scenes->At(0)->Get("nodes")) {
                for (size_t i = 0; i < sceneNodes->Size(); ++i) {
                    visit(sceneNodes->At(i)->GetInt(), math::Mat4::Identity());
                }
            }
        }
    }
    NEON_LOG_INFO("GLTF: loaded '%s' (%zu nodes)", path.c_str(), out.nodes.size());
    return out;
}

gfx::Font AssetManager::LoadFont(const std::string& path, int pixelHeight) {
    auto key = std::make_pair(path, pixelHeight);
    auto cached = fonts_.find(key);
    if (cached != fonts_.end()) return cached->second;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Asset: failed to open font '%s'", path.c_str());
        return {};
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    gfx::Font font = renderer_->CreateFontFromMemory(data.data(), data.size(), pixelHeight);
    fonts_[key] = font;
    return font;
}

gfx::Font AssetManager::LoadSystemCJKFont(int pixelHeight,
                                          const std::vector<std::string>& sampleTexts) {
#if defined(_WIN32)
    const char* kCandidates[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        nullptr};
#elif defined(__APPLE__)
    const char* kCandidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        nullptr};
#else
    const char* kCandidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        nullptr};
#endif

    // Collect unique non-ASCII codepoints from sample texts.
    std::vector<int32_t> codepoints;
    {
        std::vector<bool> seen(0x110000, false);
        for (const std::string& text : sampleTexts) {
            const char* it = text.data();
            const char* end = it + text.size();
            while (it < end) {
                int32_t cp = gfx::DecodeUTF8Next(it, end);
                if (cp > 126 && !seen[static_cast<size_t>(cp)]) {
                    seen[static_cast<size_t>(cp)] = true;
                    codepoints.push_back(cp);
                }
            }
        }
    }
    if (codepoints.empty()) return {};

    for (const char* candidate : kCandidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (!in.is_open()) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        gfx::Font font = renderer_->CreateFontFromMemoryWithCodepoints(
            data.data(), data.size(), pixelHeight, codepoints.data(),
            static_cast<int>(codepoints.size()));
        if (font.Valid()) {
            NEON_LOG_INFO("Asset: loaded system CJK font '%s' (%zu codepoints)",
                          candidate, codepoints.size());
            return font;
        }
    }
    NEON_LOG_WARN("Asset: no system CJK font found; Chinese text will not render");
    return {};
}

AssetStats AssetManager::Stats() const {
    AssetStats stats;
    for (const auto& kv : textures_) {
        if (!kv.second.Valid()) continue;
        ++stats.textures;
        stats.textureBytes +=
            static_cast<size_t>(kv.second.Width()) * static_cast<size_t>(kv.second.Height()) * 4;
    }
    for (const auto& kv : meshes_) {
        if (!kv.second.Valid()) continue;
        ++stats.meshes;
        stats.meshTriangles += kv.second.TriangleCount();
    }
    stats.fonts = fonts_.size();
    return stats;
}

} // namespace neon::assets
