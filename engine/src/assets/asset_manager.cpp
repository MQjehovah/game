#include "neon/assets/asset_manager.hpp"
#include "neon/assets/asset_path.hpp"

#include "system_cjk_codepoints.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "neon/assets/asset_importer.hpp"
#include "neon/assets/bc1.hpp"
#include "neon/assets/image_decode.hpp"
#include "neon/core/log.hpp"
#include "neon/core/json.hpp"
#include "neon/math/quat.hpp"

namespace neon::assets {

namespace {

std::string DirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos + 1);
}

// File modification time (seconds). 0 when the file does not exist.
uint64_t FileMTime(const std::string& path) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) == 0) return static_cast<uint64_t>(st.st_mtime);
#else
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) return static_cast<uint64_t>(st.st_mtime);
#endif
    return 0;
}

} // anonymous namespace

// Pure-CPU image decode + optional BC1 compression. Never touches GL, so it is
// safe to run on an async worker thread. Loading in the image's NATIVE channel
// count lets us distinguish opaque images (gray / RGB) from alpha-bearing ones
// (gray+alpha / RGBA): BC1 has no alpha, so only opaque images are compressed.
// Alpha images stay RGBA8.
//
// G7-1: whole-file read through the optional VFS layer, falling back to the
// real filesystem when no layer is set. Returns Err on missing/unreadable.
// When a VFS is set but rejects the path (an absolute path, or a reference
// that escapes the mounted root), it falls back to a direct CWD read of the
// ORIGINAL path — so relative/absolute/legacy references all resolve through
// this one entry point without per-call path munging.
core::Result<std::vector<uint8_t>> ReadAllBytes(neon::io::IFileSystem* fs,
                                                const std::string& path) {
    if (fs) {
        auto r = fs->ReadFile(path);
        if (r.Ok()) return r;
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return core::Result<std::vector<uint8_t>>::Err("cannot open");
    const std::streamsize size = in.tellg();
    if (size < 0) return core::Result<std::vector<uint8_t>>::Err("cannot size");
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (in.bad()) return core::Result<std::vector<uint8_t>>::Err("read failed");
    return core::Result<std::vector<uint8_t>>::Ok(std::move(bytes));
}

// The produced RGBA8 pixels are byte-identical to the old
// stbi_load(path, &w, &h, &ch, 4) path: stb_image expands gray->RGB->RGBA with
// the same replication and alpha=255.
// Pure decode of already-read image bytes (stbi_load_from_memory, native
// channel count) + optional BC1. Shared by DecodeImageFile (disk) and the
// AssetManager VFS path (G7-1).

DecodedImage DecodeImageFile(const std::string& path, bool compressBc1, bool flipVertically) {
    const core::Result<std::vector<uint8_t>> bytes = ReadAllBytes(nullptr, path);
    if (!bytes.Ok()) return {};
    return DecodeImageBytes(bytes.Value(), compressBc1, flipVertically);
}

core::Result<std::vector<uint8_t>> AssetManager::IoRead(const std::string& path) const {
    // "@assets/x" / "assets:/x" -> "assets/x" (project-relative virtual path).
    const std::string norm = assets::NormalizeAssetPath(path);
    if (fs_) {
        auto r = fs_->ReadFile(norm);
        if (r.Ok()) return r;
        // VFS miss (absolute path, or a path outside the mounted root): fall
        // through to a direct read of the ORIGINAL path so legacy/external
        // references keep working during the gradual cutover.
    }
    return ReadAllBytes(nullptr, path);
}

uint64_t AssetManager::IoMTime(const std::string& path) const {
    const std::string norm = assets::NormalizeAssetPath(path);
    if (fs_) {
        const uint64_t m = fs_->FileMTime(norm);
        if (m != 0) return m;
    }
    return FileMTime(path); // CWD direct fallback (absolute / legacy paths)
}

namespace {

void LoadMaterialColors(neon::io::IFileSystem* fs, const std::string& mtlPath,
                        std::map<std::string, math::Vec4>& out) {
    const core::Result<std::vector<uint8_t>> bytes = ReadAllBytes(fs, mtlPath);
    if (!bytes.Ok()) return;
    std::istringstream in(std::string(bytes.Value().begin(), bytes.Value().end()));
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

} // namespace
DecodedImage DecodeImageBytes(const std::vector<uint8_t>& bytes, bool compressBc1,
                              bool flipVertically) {
    DecodedImage img;
    int w = 0, h = 0, channels = 0;
    if (flipVertically) stbi_set_flip_vertically_on_load(1);
    unsigned char* data =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 0);
    if (flipVertically) stbi_set_flip_vertically_on_load(0);
    if (!data) return img;
    img.width = w;
    img.height = h;
    img.channels = 4; // uploads are always RGBA8

    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (channels == 4) {
        img.rgba.assign(data, data + pixelCount * 4);
    } else if (channels == 2) {
        // gray+alpha: expand to RGBA (stb_image's req_comp=4 does the same
        // replication). NOTE: the 2-channel buffer is only pixelCount*2 bytes;
        // copying pixelCount*4 would over-read it.
        img.rgba.resize(pixelCount * 4);
        for (size_t i = 0; i < pixelCount; ++i) {
            img.rgba[i * 4 + 0] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = data[i * 2 + 0];
            img.rgba[i * 4 + 3] = data[i * 2 + 1];
        }
    } else {
        // Opaque (gray / RGB): expand to RGBA with alpha=255. Only these are
        // eligible for BC1 (which has no alpha channel).
        img.rgba.resize(pixelCount * 4);
        for (size_t i = 0; i < pixelCount; ++i) {
            uint8_t r, g, b;
            if (channels >= 3) {
                r = data[i * 3 + 0];
                g = data[i * 3 + 1];
                b = data[i * 3 + 2];
            } else {
                r = g = b = data[i];
            }
            img.rgba[i * 4 + 0] = r;
            img.rgba[i * 4 + 1] = g;
            img.rgba[i * 4 + 2] = b;
            img.rgba[i * 4 + 3] = 255;
        }
        if (compressBc1) Bc1EncodeOpaque(img.rgba.data(), w, h, img.bc1);
    }
    stbi_image_free(data);
    return img;
}

// G6-2: CPU-only OBJ parse, shared by the sync LoadMeshOBJ and the async
// LoadMeshOBJAsync (which runs it on a worker thread). Never touches the GPU or
// the AssetManager's main-thread maps: material *paths* are returned so the
// caller records dependency edges on the main thread. `error` is non-empty on
// failure (with the same wording the sync path logged).
// (Declared in neon::assets so FinishAsyncMesh can take it.)
struct ParsedObjMesh {
    std::vector<gfx::Vertex3D> verts;
    std::vector<uint16_t> indices;
    std::vector<std::string> mtlPaths; // dependency edges for RecordDependency
    std::string error;
};

namespace {

ParsedObjMesh ParseObjMesh(const std::string& path, const std::vector<uint8_t>& bytes,
                           neon::io::IFileSystem* fs) {
    ParsedObjMesh out;
    std::istringstream in(std::string(bytes.begin(), bytes.end()));

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
            const std::string mtlPath = DirName(path) + mtlFile;
            out.mtlPaths.push_back(mtlPath);
            LoadMaterialColors(fs, mtlPath, materialColors);
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
            // A13: the mesh index buffer is 16-bit; beyond 65536 vertices the
            // wraparound used to silently scramble geometry. Fail the load with
            // an explicit error instead.
            if (verts.size() + face.size() > 65536u) {
                out.error = "OBJ exceeds 16-bit index limit (65536 vertices): " +
                            std::to_string(verts.size() + face.size());
                return out;
            }
            uint16_t base = static_cast<uint16_t>(verts.size());
            for (size_t i = 0; i < face.size(); ++i) {
                const FaceIndex& f = face[i];
                if (f.v <= 0 || f.v > static_cast<int>(positions.size())) {
                    out.error = "bad vertex index " + std::to_string(f.v);
                    return out;
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
        out.error = "no vertices parsed";
        return out;
    }
    out.verts = std::move(verts);
    out.indices = std::move(indices);
    return out;
}

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
bool ResolveGltfAccessor(const std::vector<std::vector<uint8_t>>& bins,
                          const std::vector<assets::GltfBufferView>& views,
                         const std::vector<assets::GltfAccessor>& accs,
                         int index, GltfAccessorLayout& out) {
    if (index < 0 || index >= static_cast<int>(accs.size())) return false;
    const assets::GltfAccessor& acc = accs[static_cast<size_t>(index)];
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(views.size())) return false;
    const assets::GltfBufferView& view = views[static_cast<size_t>(acc.bufferView)];
    // A13: a bufferView names its buffer; honor it (multi-buffer assets used
    // to silently read everything from buffers[0]).
    if (view.buffer < 0 || view.buffer >= static_cast<int>(bins.size())) return false;
    const std::vector<uint8_t>& bin = bins[static_cast<size_t>(view.buffer)];
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

} // namespace

// G6-2: container-level glTF parse (GLB chunk split or .gltf JSON + external
// .bin read). Pure CPU, worker-safe; the main thread records the .bin dependency
// edge and builds the GPU/asset data via LoadGltfJson. `error` is non-empty on
// failure (same wording as the sync path logged).
struct ParsedGltf {
    core::Json root;
    std::vector<std::vector<uint8_t>> bins; // glTF buffers by index (A13)
    std::vector<std::string> binPaths;      // external .bin paths (dependency edges)
    std::string dir;     // directory for relative texture/buffer refs
    std::string error;
};

ParsedGltf ParseGltfContainer(const std::string& path, const std::vector<uint8_t>& bytes,
                              neon::io::IFileSystem* fs) {
    ParsedGltf out;
    if (bytes.size() >= 20 && bytes[0] == 'g' && bytes[1] == 'l' && bytes[2] == 'T' &&
        bytes[3] == 'F') {
        // GLB: 12-byte header then [len, type, data]* chunks.
        size_t off = 12;
        std::string jsonText;
        std::vector<uint8_t> glbBin;
        while (off + 8 <= bytes.size()) {
            uint32_t clen = 0, ctype = 0;
            std::memcpy(&clen, bytes.data() + off, 4);
            std::memcpy(&ctype, bytes.data() + off + 4, 4);
            off += 8;
            if (off + clen > bytes.size()) break;
            if (ctype == 0x4E4F534A) // 'JSON'
                jsonText.assign(reinterpret_cast<const char*>(bytes.data() + off), clen);
            else if (ctype == 0x004E4942) // 'BIN'
                glbBin.assign(bytes.data() + off, bytes.data() + off + clen);
            off += clen;
        }
        std::string parseError;
        out.root = core::Json::Parse(jsonText, &parseError);
        if (out.root.IsNull()) {
            out.error = "GLB JSON parse error in '" + path + "': " + parseError;
            return out;
        }
        if (glbBin.empty()) {
            out.error = "GLB no BIN chunk in '" + path + "'";
            return out;
        }
        out.bins.push_back(std::move(glbBin));
        // GLB images may still be EXTERNAL URIs relative to the file (Kenney
        // FTK glb references "Textures/colormap.png"), so keep the containing
        // directory like the .gltf branch; only the buffer bytes are embedded.
        out.dir = DirName(path);
        return out;
    }

    std::stringstream ss(std::string(bytes.begin(), bytes.end()));
    std::string parseError;
    out.root = core::Json::Parse(ss.str(), &parseError);
    if (out.root.IsNull()) {
        out.error = "JSON parse error: " + parseError;
        return out;
    }
    out.dir = DirName(path);
    // A13: load EVERY declared buffer, not just buffers[0]. Multi-buffer assets
    // (mesh in one .bin, animation in another) used to lose their tail data.
    if (const core::Json* buffers = out.root.Get("buffers"); buffers && buffers->Size() > 0) {
        for (size_t i = 0; i < buffers->Size(); ++i) {
            const core::Json* b = buffers->At(i);
            if (!b) {
                out.bins.emplace_back();
                continue;
            }
            if (const core::Json* uri = b->Get("uri"); uri && uri->IsString()) {
                const std::string u = uri->GetString();
                const core::Result<std::vector<uint8_t>> binBytes =
                    ReadAllBytes(fs, out.dir + u);
                if (binBytes.Ok()) {
                    out.binPaths.push_back(out.dir + u);
                    out.bins.push_back(binBytes.Value());
                } else {
                    out.error = "cannot open buffer '" + (out.dir + u) + "'";
                    return out;
                }
            } else {
                // A buffer without a URI in a .gltf (non-GLB) is malformed per
                // spec; keep the slot so buffer indices stay aligned.
                out.bins.emplace_back();
            }
        }
    }
    if (out.bins.empty() || out.bins[0].empty()) {
        out.error = "empty binary buffer in '" + path + "'";
        return out;
    }
    return out;
}

core::Result<std::vector<float>> GltfAsset::ReadAccessorFloats(int accessorIndex) const {
    GltfAccessorLayout layout;
    if (!ResolveGltfAccessor(bins, bufferViews, accessors, accessorIndex, layout))
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

AssetManager::~AssetManager() {
    // Join the worker pool before any member dies so no worker closure can
    // reference a torn-down AssetManager. Pending/undelivered work is
    // discarded (nothing is uploaded at shutdown).
    asyncLoader_.Shutdown();
}

DecodedImage AssetManager::DecodeImage(const std::string& path, const TextureLoadOptions& opts,
                                       bool compressed) {
    if (decodeFn_) return decodeFn_(path, opts);
    if (fs_) {
        const core::Result<std::vector<uint8_t>> bytes = IoRead(path);
        if (!bytes.Ok()) return {};
        return DecodeImageBytes(bytes.Value(), compressed, opts.flipVertically);
    }
    return DecodeImageFile(path, compressed, opts.flipVertically);
}

gfx::Texture AssetManager::UploadDecoded(const DecodedImage& img, gfx::Wrap wrap) {
    if (!img.bc1.empty()) {
        gfx::Texture tex = renderer_->CreateTextureCompressed(
            img.width, img.height, kBc1Format, img.bc1.data(), img.bc1.size());
        if (tex.Valid()) return tex;
        // The driver rejected the compressed upload: fall back to RGBA8 and
        // remember not to compress again (log the fallback once).
        if (!bc1FallbackWarned_) {
            bc1FallbackWarned_ = true;
            NEON_LOG_WARN("Asset: BC1 compressed upload rejected by the driver; "
                          "falling back to RGBA8 (compression disabled for this session)");
        }
        bc1Supported_ = false;
    }
    gfx::TextureDesc desc;
    desc.width = img.width;
    desc.height = img.height;
    desc.rgba = img.rgba.data();
    desc.mipmaps = true;
    desc.wrap = wrap;
    return renderer_->CreateTexture(desc);
}

gfx::Texture AssetManager::LoadTexture(const std::string& path) {
    return LoadTexture(path, TextureLoadOptions{});
}

std::string AssetManager::TextureCacheKey(const std::string& path,
                                          const TextureLoadOptions& opts) {
    // Unit separator keeps the suffix unambiguous; Stats()/resource panel
    // strip it when displaying the path.
    std::string key = path;
    if (opts.flipVertically) key += std::string("\x1F") + "f";
    if (opts.wrap == gfx::Wrap::Repeat) key += std::string("\x1F") + "r";
    return key;
}

// A9: the path part of a cache key (suffixes stripped).
std::string AssetManager::KeyPathOf(const std::string& key) {
    const size_t cut = key.find('\x1F');
    return cut == std::string::npos ? key : key.substr(0, cut);
}

// A9: reconstructs the load options encoded in a cache key (inverse of
// TextureCacheKey by construction).
TextureLoadOptions AssetManager::OptsFromKey(const std::string& key) {
    TextureLoadOptions opts;
    const size_t cut = key.find('\x1F');
    if (cut == std::string::npos) return opts;
    for (size_t i = cut; i < key.size(); ++i) {
        if (key[i] == 'f') opts.flipVertically = true;
        if (key[i] == 'r') opts.wrap = gfx::Wrap::Repeat;
    }
    return opts;
}

gfx::Texture AssetManager::LoadTexture(const std::string& path, const TextureLoadOptions& opts) {
    const std::string key = TextureCacheKey(path, opts);
    auto cached = textures_.find(key);
    if (cached != textures_.end()) {
        ++textureRefs_[key];  // every load is an acquisition
        return cached->second;
    }
    // Negative cache: a previously missing file returns {} silently while it
    // stays missing (the first failure already logged). If the file appeared
    // since then, retry the real load once.
    if (failedTextures_.count(key) != 0) {
        if (IoMTime(path) == 0) return {};
        failedTextures_.erase(key);
    }

    DecodedImage img;
    // G5-4-3: prefer a pre-baked BC1 texture (offline AssetImporter cache) —
    // upload the blocks directly, skipping the runtime decode+compress. Reads
    // through the VFS when installed (packed game), else the bake dir on disk.
    if (!bakeDir_.empty() && img.bc1.empty()) {
        const core::Result<std::vector<uint8_t>> baked =
            IoRead(bakeDir_ + "/" + path + ".nbc1");
        if (baked.Ok() && baked.Value().size() >= 12 &&
            baked.Value()[0] == 'N' && baked.Value()[1] == 'B' &&
            baked.Value()[2] == 'C' && baked.Value()[3] == '1') {
            const uint8_t* b = baked.Value().data();
            img.width = static_cast<int>(b[4]) | (static_cast<int>(b[5]) << 8) |
                        (static_cast<int>(b[6]) << 16) | (static_cast<int>(b[7]) << 24);
            img.height = static_cast<int>(b[8]) | (static_cast<int>(b[9]) << 8) |
                         (static_cast<int>(b[10]) << 16) | (static_cast<int>(b[11]) << 24);
            if (img.width > 0 && img.height > 0) {
                img.channels = 4;
                img.bc1.assign(baked.Value().begin() + 12, baked.Value().end());
                NEON_LOG_INFO("Asset: loaded baked texture '%s' (%dx%d, BC1)",
                              path.c_str(), img.width, img.height);
            }
        }
    }
    if (img.channels == 0)
        img = DecodeImage(path, opts, opts.compressBc1 && bc1Supported_);
    if (img.channels == 0) {
        failedTextures_.insert(key);
        NEON_LOG_ERROR("Asset: failed to load texture '%s'", path.c_str());
        return {};
    }
    gfx::Texture texture = UploadDecoded(img, opts.wrap);
    if (!texture.Valid()) {
        NEON_LOG_ERROR("Asset: failed to upload texture '%s'", path.c_str());
        return {};
    }
    textures_[key] = texture;
    textureMtimes_[key] = IoMTime(path);
    textureRefs_[key] = 1;
    NEON_LOG_INFO("Asset: loaded texture '%s' (%dx%d%s)", path.c_str(), img.width, img.height,
                  img.bc1.empty() ? "" : ", BC1");
    return texture;
}

void AssetManager::LoadTextureAsync(const std::string& path, std::function<void(bool)> cb,
                                    const TextureLoadOptions& opts) {
    if (!cb) return;

    // Already cached? The async contract fires cb on the main thread, and we
    // are on the main thread, so firing inline matches the sync path exactly.
    const std::string key = TextureCacheKey(path, opts);
    auto cached = textures_.find(key);
    if (cached != textures_.end()) {
        cb(true);
        return;
    }
    // Negative cache (see LoadTexture): known-missing files skip the worker.
    if (failedTextures_.count(key) != 0) {
        if (IoMTime(path) == 0) {
            cb(false);
            return;
        }
        failedTextures_.erase(key);
    }

    if (!asyncEnabled_ || !asyncLoader_.Available()) {
        // No worker pool (disabled for tests, or thread creation failed):
        // degrade to a synchronous load, callback fires inline.
        gfx::Texture tex = LoadTexture(path, opts);
        cb(tex.Valid());
        return;
    }

    // Dedupe: one in-flight decode per CACHE KEY (A9); later callers asking
    // for the same path with different options (flip/wrap) no longer coalesce
    // onto the first request's decode result.
    const bool inFlight = inFlight_.count(key) != 0;
    if (!inFlight) inFlight_[key] = true;
    pendingCallbacks_[key].push_back(std::move(cb));
    if (inFlight) return;

    // Compression eligibility is decided HERE on the main thread (reads the
    // driver-capability flag); the worker just runs whatever it is told.
    const bool compressed = opts.compressBc1 && bc1Supported_;
    bool ok = asyncLoader_.Submit([this, path, opts, compressed]() {
        DecodedImage img = DecodeImage(path, opts, compressed);
        asyncLoader_.Deliver([this, path, opts, img = std::move(img)]() mutable {
            FinishAsyncTexture(path, std::move(img), opts);
        });
    });
    if (!ok) {
        // The pool became unavailable between the check and the submit; fall
        // back to a synchronous load for this one request.
        inFlight_.erase(key);
        auto cbs = std::move(pendingCallbacks_[key]);
        pendingCallbacks_.erase(key);
        gfx::Texture tex = LoadTexture(path, opts);
        for (auto& c : cbs) c(tex.Valid());
    }
}

void AssetManager::PumpAsync() {
    asyncLoader_.Pump();
    ++pumpFrame_;
    ReclaimRetired(/*frame=*/pumpFrame_);
}

// G1-4 dependency graph ------------------------------------------------

void AssetManager::RecordDependency(const std::string& parent, const std::string& dep) {
    auto& deps = dependencies_[parent];
    if (std::find(deps.begin(), deps.end(), dep) == deps.end()) deps.push_back(dep);
    auto& rev = dependents_[dep];
    if (std::find(rev.begin(), rev.end(), parent) == rev.end()) rev.push_back(parent);
}

const std::vector<std::string>& AssetManager::DependenciesOf(const std::string& path) const {
    static const std::vector<std::string> kEmpty;
    const auto it = dependencies_.find(path);
    return it == dependencies_.end() ? kEmpty : it->second;
}

std::vector<std::string> AssetManager::DependentsOf(const std::string& path) const {
    std::vector<std::string> out;
    const auto it = dependents_.find(path);
    if (it != dependents_.end()) out = it->second;
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

bool FileLoadable(neon::io::IFileSystem* fs, const std::string& path) {
    if (fs) return fs->Exists(path);
#if defined(_WIN32)
    struct _stat64 st;
    return ::_stat64(path.c_str(), &st) == 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

// Image extensions the texture pipeline can decode (stb_image). Non-image
// leaves (glTF .bin buffers, .mtl) are file-loaded by the asset loader itself
// and must not be fed to LoadTextureAsync.
bool IsImagePath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" ||
           ext == "bmp" || ext == "gif" || ext == "webp";
}

} // namespace

std::vector<std::string> AssetManager::MissingDependencies(const std::string& path) const {
    std::vector<std::string> missing;
    std::set<std::string> visited;
    std::vector<std::string> stack;
    const auto root = dependencies_.find(path);
    if (root != dependencies_.end()) stack = root->second;
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (!visited.insert(cur).second) continue;
        const auto deps = dependencies_.find(cur);
        if (deps != dependencies_.end()) {
            // Composite asset: recurse into its dependencies.
            stack.insert(stack.end(), deps->second.begin(), deps->second.end());
            continue;
        }
        // Leaf (texture / buffer / MTL): missing when the file cannot be read.
        if (!FileLoadable(fs_, cur)) missing.push_back(cur);
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    return missing;
}

void AssetManager::LoadDependenciesAsync(
    const std::string& path, std::function<void(bool, const std::string&)> cb) {
    if (!cb) return;
    // Collect every transitive LEAF dependency (textures etc.) to load.
    std::vector<std::string> leaves;
    std::set<std::string> visited;
    std::vector<std::string> stack;
    const auto root = dependencies_.find(path);
    if (root != dependencies_.end()) stack = root->second;
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (!visited.insert(cur).second) continue;
        const auto deps = dependencies_.find(cur);
        if (deps != dependencies_.end()) {
            stack.insert(stack.end(), deps->second.begin(), deps->second.end());
            continue;
        }
        if (IsImagePath(cur)) leaves.push_back(cur);
    }
    if (leaves.empty()) {
        cb(true, "");
        return;
    }
    struct DepState {
        int done = 0;
        int total = 0;
        std::string firstError;
    };
    auto state = std::make_shared<DepState>();
    state->total = static_cast<int>(leaves.size());
    for (const std::string& leaf : leaves) {
        LoadTextureAsync(leaf, [state, cb, leaf](bool ok) {
            if (!ok && state->firstError.empty())
                state->firstError = "dependency '" + leaf + "' failed to load";
            ++state->done;
            if (state->done == state->total) cb(state->firstError.empty(), state->firstError);
        });
    }
}

void AssetManager::FinishAsyncTexture(const std::string& path, DecodedImage img,
                                      const TextureLoadOptions& opts) {
    const std::string key = TextureCacheKey(path, opts); // A9: key-based bookkeeping
    inFlight_.erase(key);
    std::vector<std::function<void(bool)>> cbs;
    auto cbIt = pendingCallbacks_.find(key);
    if (cbIt != pendingCallbacks_.end()) {
        cbs = std::move(cbIt->second);
        pendingCallbacks_.erase(cbIt);
    }

    bool ok = false;
    if (img.channels > 0 && !img.rgba.empty()) {
        gfx::Texture tex = UploadDecoded(img, opts.wrap);
        if (tex.Valid()) {
            const std::string key = TextureCacheKey(path, opts);
            textures_[key] = tex;
            textureMtimes_[key] = IoMTime(path);
            textureRefs_[key] = 1;
            ok = true;
            NEON_LOG_INFO("Asset: async texture '%s' loaded (%dx%d%s)", path.c_str(), img.width,
                          img.height, img.bc1.empty() ? "" : ", BC1");
        } else {
            NEON_LOG_ERROR("Asset: async texture '%s' GPU upload failed", path.c_str());
        }
    } else {
        failedTextures_.insert(TextureCacheKey(path, opts));
        NEON_LOG_ERROR("Asset: async texture '%s' decode failed", path.c_str());
    }
    for (auto& cb : cbs) cb(ok);
}

gfx::Mesh AssetManager::LoadMeshOBJ(const std::string& path) {
    auto cached = meshes_.find(path);
    if (cached != meshes_.end()) {
        ++meshRefs_[path];  // every load is an acquisition
        return cached->second;
    }

    const core::Result<std::vector<uint8_t>> fileBytes = IoRead(path);
    if (!fileBytes.Ok()) {
        NEON_LOG_ERROR("Asset: failed to open OBJ '%s'", path.c_str());
        return {};
    }
    ParsedObjMesh parsed = ParseObjMesh(path, fileBytes.Value(), fs_);
    for (const std::string& mtl : parsed.mtlPaths) RecordDependency(path, mtl);
    if (!parsed.error.empty()) {
        NEON_LOG_ERROR("OBJ '%s': %s", path.c_str(), parsed.error.c_str());
        return {};
    }

    gfx::Mesh mesh = gfx::Mesh::CreateFromData(*renderer_, parsed.verts.data(),
                                               static_cast<uint32_t>(parsed.verts.size()),
                                               parsed.indices.data(),
                                               static_cast<uint32_t>(parsed.indices.size()), path);
    meshes_[path] = mesh;
    meshMtimes_[path] = IoMTime(path);
    meshRefs_[path] = 1;
    NEON_LOG_INFO("Asset: loaded OBJ '%s' (%zu verts)", path.c_str(), parsed.verts.size());
    return mesh;
}

// G6-2: async OBJ load. The worker reads + parses (pure CPU); the main thread
// (PumpAsync -> FinishAsyncMesh) uploads, records dependency edges and fires the
// callbacks. Mirrors the texture async contract: cached hits and degraded
// (no-pool) loads fire inline, in-flight requests coalesce per path.
void AssetManager::LoadMeshOBJAsync(const std::string& path, std::function<void(bool)> cb) {
    if (!cb) return;
    auto cached = meshes_.find(path);
    if (cached != meshes_.end()) {
        ++meshRefs_[path];
        cb(true);
        return;
    }
    if (!asyncEnabled_ || !asyncLoader_.Available()) {
        cb(LoadMeshOBJ(path).Valid());
        return;
    }

    const bool inFlight = meshInFlight_.count(path) != 0;
    if (!inFlight) meshInFlight_[path] = true;
    meshPendingCallbacks_[path].push_back(std::move(cb));
    if (inFlight) return;

    bool ok = asyncLoader_.Submit([this, path]() {
        const core::Result<std::vector<uint8_t>> bytes = IoRead(path);
        ParsedObjMesh parsed;
        if (bytes.Ok()) parsed = ParseObjMesh(path, bytes.Value(), fs_);
        else parsed.error = "failed to open OBJ";
        asyncLoader_.Deliver([this, path, parsed = std::move(parsed)]() mutable {
            FinishAsyncMesh(path, std::move(parsed));
        });
    });
    if (!ok) {
        // The pool became unavailable mid-request: degrade to a sync load.
        meshInFlight_.erase(path);
        auto cbs = std::move(meshPendingCallbacks_[path]);
        meshPendingCallbacks_.erase(path);
        const bool loaded = LoadMeshOBJ(path).Valid();
        for (auto& c : cbs) c(loaded);
    }
}

void AssetManager::FinishAsyncMesh(const std::string& path, ParsedObjMesh&& parsed) {
    for (const std::string& mtl : parsed.mtlPaths) RecordDependency(path, mtl);
    const bool ok = parsed.error.empty() && !parsed.verts.empty();
    if (ok) {
        gfx::Mesh mesh = gfx::Mesh::CreateFromData(*renderer_, parsed.verts.data(),
                                                   static_cast<uint32_t>(parsed.verts.size()),
                                                   parsed.indices.data(),
                                                   static_cast<uint32_t>(parsed.indices.size()),
                                                   path);
        meshes_[path] = mesh;
        meshMtimes_[path] = IoMTime(path);
        meshRefs_[path] = 1;
        NEON_LOG_INFO("Asset: loaded OBJ '%s' async (%zu verts)", path.c_str(),
                      parsed.verts.size());
    } else {
        NEON_LOG_ERROR("OBJ '%s': %s", path.c_str(),
                       parsed.error.empty() ? "no vertices parsed" : parsed.error.c_str());
    }
    meshInFlight_.erase(path);
    auto cbs = std::move(meshPendingCallbacks_[path]);
    meshPendingCallbacks_.erase(path);
    for (auto& c : cbs) c(ok);
}

GltfAsset AssetManager::LoadGLTF(const std::string& path) {
    // Parse once per file version: a glTF scene is frequently referenced by
    // many entities (a wolf pack, an instanced prop) and re-parsing it per
    // entity re-decodes the JSON and re-uploads every mesh. The mtime gate
    // keeps hot-reload correct: an edited file gets re-parsed on the next
    // request. A missing file (mtime 0) is not cached, so a file created later
    // is picked up.
    const uint64_t mtime = IoMTime(path);
    const auto gltfIt = gltfs_.find(path);
    const auto mtimeIt = gltfMtimes_.find(path);
    if (gltfIt != gltfs_.end() && mtimeIt != gltfMtimes_.end() &&
        mtimeIt->second == mtime && mtime != 0) {
        return gltfIt->second;
    }

    const core::Result<std::vector<uint8_t>> fileBytes = IoRead(path);
    if (!fileBytes.Ok()) {
        NEON_LOG_ERROR("GLTF: failed to open '%s'", path.c_str());
        return {};
    }

    ParsedGltf parsed = ParseGltfContainer(path, fileBytes.Value(), fs_);
    if (!parsed.error.empty()) {
        NEON_LOG_ERROR("GLTF: %s", parsed.error.c_str());
        return {};
    }
    for (const std::string& bp : parsed.binPaths) RecordDependency(path, bp);
    return LoadGltfJson(parsed.root, std::move(parsed.bins), path, mtime, parsed.dir);
}

// G6-2: async glTF/GLB load. The worker reads + parses the container (JSON +
// binary buffer); the main thread records the .bin dependency, builds meshes +
// textures (GPU) via LoadGltfJson and fires the callbacks. Same coalescing and
// inline-fallback contract as LoadMeshOBJAsync.
void AssetManager::LoadGLTFAsync(const std::string& path, std::function<void(bool)> cb) {
    if (!cb) return;
    const uint64_t mtime = IoMTime(path);
    const auto it = gltfs_.find(path);
    const auto mit = gltfMtimes_.find(path);
    if (it != gltfs_.end() && mit != gltfMtimes_.end() && mit->second == mtime && mtime != 0) {
        cb(true);
        return;
    }
    if (!asyncEnabled_ || !asyncLoader_.Available()) {
        cb(LoadGLTF(path).Valid());
        return;
    }

    const bool inFlight = gltfInFlight_.count(path) != 0;
    if (!inFlight) gltfInFlight_[path] = true;
    gltfPendingCallbacks_[path].push_back(std::move(cb));
    if (inFlight) return;

    bool ok = asyncLoader_.Submit([this, path, mtime]() {
        const core::Result<std::vector<uint8_t>> bytes = IoRead(path);
        ParsedGltf parsed;
        if (bytes.Ok()) parsed = ParseGltfContainer(path, bytes.Value(), fs_);
        else parsed.error = "failed to open '" + path + "'";
        asyncLoader_.Deliver([this, path, mtime, parsed = std::move(parsed)]() mutable {
            FinishAsyncGltf(path, mtime, std::move(parsed));
        });
    });
    if (!ok) {
        gltfInFlight_.erase(path);
        auto cbs = std::move(gltfPendingCallbacks_[path]);
        gltfPendingCallbacks_.erase(path);
        const bool loaded = LoadGLTF(path).Valid();
        for (auto& c : cbs) c(loaded);
    }
}

void AssetManager::FinishAsyncGltf(const std::string& path, uint64_t mtime, ParsedGltf&& parsed) {
    const bool hasBin = !parsed.bins.empty() && !parsed.bins[0].empty();
    bool ok = parsed.error.empty() && !parsed.root.IsNull() && hasBin;
    if (ok) {
        for (const std::string& bp : parsed.binPaths) RecordDependency(path, bp);
        GltfAsset asset = LoadGltfJson(parsed.root, std::move(parsed.bins), path, mtime,
                                       parsed.dir);
        ok = asset.Valid();
        if (ok)
            NEON_LOG_INFO("Asset: loaded GLTF '%s' async", path.c_str());
    }
    if (!ok)
        NEON_LOG_ERROR("GLTF: %s", parsed.error.empty() ? "invalid asset" : parsed.error.c_str());
    gltfInFlight_.erase(path);
    auto cbs = std::move(gltfPendingCallbacks_[path]);
    gltfPendingCallbacks_.erase(path);
    for (auto& c : cbs) c(ok);
}

GltfAsset AssetManager::LoadGltfJson(core::Json& root, std::vector<std::vector<uint8_t>> bins,
                                     const std::string& path, uint64_t mtime,
                                     const std::string& dir) {
    // Shared body for .gltf (JSON + external .bin) and .glb (JSON chunk +
    // BIN chunk). `bins` may be empty for malformed assets; callers below
    // already logged the specific failure.

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
        if (!ResolveGltfAccessor(bins, parsedViews, parsedAccessors, accessorIndex, layout))
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
        const core::Json* samplers = root.Get("samplers");
        auto samplerWrap = [&](int samplerIndex) -> gfx::Wrap {
            // glTF samplers default to REPEAT; only CLAMP_TO_EDGE overrides
            // it (MIRRORED_REPEAT is approximated with plain REPEAT).
            gfx::Wrap w = gfx::Wrap::Repeat;
            if (samplerIndex >= 0 && samplers && samplerIndex < static_cast<int>(samplers->Size())) {
                const core::Json* s = samplers->At(samplerIndex);
                if (s) {
                    if (const core::Json* wt = s->Get("wrapT")) {
                        if (wt->GetInt(10497) == 33071) w = gfx::Wrap::Clamp;
                    }
                    if (const core::Json* ws = s->Get("wrapS")) {
                        if (ws->GetInt(10497) == 33071) w = gfx::Wrap::Clamp;
                    }
                }
            }
            return w;
        };
        for (size_t i = 0; i < texs->Size(); ++i) {
            int src = -1;
            if (const core::Json* srcNode = texs->At(i)->Get("source")) src = srcNode->GetInt(-1);
            int samplerIdx = -1;
            if (const core::Json* smp = texs->At(i)->Get("sampler")) samplerIdx = smp->GetInt(-1);
            std::string uri;
            if (images && src >= 0 && images->At(src) && images->At(src)->Get("uri")) {
                uri = images->At(src)->Get("uri")->GetString();
            }
            if (uri.empty()) {
                textures.push_back(gfx::Texture{});
            } else {
                // This engine uploads textures with their first row at v=0,
                // which already matches glTF's top-left UV origin - no flip.
                // Use the sampler's wrap mode so UVs outside [0,1] (common in
                // Khronos sample assets like DamagedHelmet) sample correctly.
                TextureLoadOptions gltfOpts;
                gltfOpts.wrap = samplerWrap(samplerIdx);
                RecordDependency(path, dir + uri);
                textures.push_back(LoadTexture(dir + uri, gltfOpts));
            }
        }
    }

    // Materials.
    std::vector<gfx::Material> materials;
    if (const core::Json* mats = root.Get("materials")) {
        for (size_t i = 0; i < mats->Size(); ++i) {
            const core::Json* m = mats->At(i);
            gfx::Material mat = gfx::Material::Lit({}, gfx::Color::White, 24.0f);
            // glTF alpha/blend + double-sided flags: a BLEND/MASK surface
            // (e.g. fur cards) must render transparent, and doubleSided
            // surfaces must not be back-face culled or they show holes.
            if (const core::Json* am = m->Get("alphaMode")) {
                std::string alpha = am->GetString();
                if (alpha == "MASK") {
                    mat.alphaTest = true;
                    mat.transparent = false;
                } else if (alpha == "BLEND") {
                    mat.transparent = true;
                }
                if (const core::Json* cf = m->Get("alphaCutoffFactor")) {
                    mat.alphaCutoff = static_cast<float>(cf->GetNumber(0.5));
                }
            }
            if (const core::Json* ds = m->Get("doubleSided")) {
                mat.doubleSided = ds->GetBool();
            }
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
        // 大型 glTF mesh（>65535 顶点，如 Poly Haven 扫描树）用 32 位索引。
        // 底层 Mesh::CreateFromDataU32 已支持；此前导入器直接丢弃这类 mesh。
        std::vector<uint32_t> indicesU32;
        bool needsU32 = false;
        gfx::Material material;
        std::vector<uint16_t> jointIds;   // 4 per vertex
        std::vector<float> jointWeights;  // 4 per vertex
        bool skinned = false;
    };
    std::vector<RawMesh> rawMeshes;
    // C15 延伸（多 mesh glTF 场景，如 Sponza）：每个 mesh 的 primitive 在
    // rawMeshes 中的起始索引与数量。一个 node 引用一个 mesh 时，它的全部
    // primitives（各自材质）都要成为 GltfMeshNode，而非只取第一个。
    std::vector<std::pair<int, int>> meshPrimRange;  // mesh index -> (start, count)
    if (const core::Json* meshes = root.Get("meshes")) {
        for (size_t mi = 0; mi < meshes->Size(); ++mi) {
            const core::Json* prims = meshes->At(mi)->Get("primitives");
            const int start = static_cast<int>(rawMeshes.size());
            int count = 0;
            if (prims) {
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
                    if (rm.skinned) {
                        // Bake the skin influence into the vertex so the VBO
                        // carries the same data AttachSkinData records on CPU.
                        const size_t vi = static_cast<size_t>(v) * 4;
                        for (int c = 0; c < 4; ++c) {
                            out.j[c] = static_cast<float>(rm.jointIds[vi + static_cast<size_t>(c)]);
                            out.w[c] = rm.jointWeights[vi + static_cast<size_t>(c)];
                        }
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
                        // 大型 mesh（>65535 顶点）：保留 32 位索引，经
                        // CreateFromDataU32 上传（扫描高模 glTF 必备，此前
                        // 直接丢弃导致 Poly Haven 树木等不可见）。
                        const uint32_t* src = reinterpret_cast<const uint32_t*>(idxBase);
                        bool big = false;
                        for (int i = 0; i < idxCount; ++i) {
                            if (src[i] > 65535u) { big = true; break; }
                        }
                        if (big) {
                            rm.indicesU32.assign(src, src + idxCount);
                            rm.needsU32 = true;
                        } else {
                            rm.indices.resize(static_cast<size_t>(idxCount));
                            for (int i = 0; i < idxCount; ++i)
                                rm.indices[static_cast<size_t>(i)] =
                                    static_cast<uint16_t>(src[i]);
                        }
                    } else if (idxStride == 1) {
                        // uint8 indices (componentType 5121): Kenney FTK glb
                        // files pack <256-vertex meshes as bytes. Without this
                        // branch the vector stays zero-filled and every
                        // triangle collapses onto vertex 0 (invisible).
                        const uint8_t* src = reinterpret_cast<const uint8_t*>(idxBase);
                        for (int i = 0; i < idxCount; ++i) rm.indices[static_cast<size_t>(i)] = src[i];
                    }
                }
                rawMeshes.push_back(std::move(rm));
                ++count;
            }
            }
            meshPrimRange.emplace_back(start, count);
        }
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
                    // glTF stores the node matrix column-major; transpose into
                    // the engine's row-major convention so stored matrices are
                    // correct transforms in this engine.
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            info.transform.m[r * 4 + c] =
                                static_cast<float>(matrix->At(c * 4 + r)->GetNumber());
                        }
                    }
                    // Decompose T*R*S so the skeleton can build per-bone TRS.
                    info.translation = {info.transform.m[3], info.transform.m[7],
                                        info.transform.m[11]};
                    info.rotation = math::Mat4ToQuat(info.transform);
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
                        // glTF matrices are column-major; the engine Mat4 is
                        // row-major, so transpose on load (without this the
                        // inverse-bind matrices are wrong and every skinned
                        // mesh is distorted).
                        for (int c = 0; c < 16; ++c)
                            mat.m[c] = src[(c % 4) * 4 + (c / 4)];
                        skin.inverseBind[static_cast<size_t>(m)] = mat;
                    }
                }
            }
            skins.push_back(std::move(skin));
        }
    }

    GltfAsset out;
    out.rawBin = bins.empty() ? std::vector<uint8_t>{} : bins[0];
    out.bins = std::move(bins);
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
            // C15 延伸：一个 node 引用一个 mesh，该 mesh 的全部 primitives
            //（各自材质）都要成为 GltfMeshNode，而非只取第一个（Sponza 类
            // 单 mesh / 多 primitive 的建筑场景）。
            int pStart = n.mesh, pCount = 1;
            if (n.mesh < static_cast<int>(meshPrimRange.size())) {
                pStart = meshPrimRange[static_cast<size_t>(n.mesh)].first;
                pCount = meshPrimRange[static_cast<size_t>(n.mesh)].second;
            }
            for (int r = pStart; r < pStart + pCount && r < static_cast<int>(rawMeshes.size()); ++r) {
            RawMesh& rm = rawMeshes[static_cast<size_t>(r)];
            // glTF spec: JOINTS_0 stores indices *into* skin.joints, while the
            // engine's bone array is indexed by glTF *node* (bone == node).
            // Remap each vertex's joint id through the skin's joint table so
            // it picks the right bone matrix. This runs BEFORE CreateFromData
            // so the GPU vertex buffer carries the remapped ids too - the
            // skinned shader reads aJointIds straight from the VBO, and
            // AttachSkinData only updates the CPU-side copy.
            // NOTE: reads out.skins - the local `skins` vector was
            // std::move'd into `out` above and is empty here.
            std::vector<uint16_t> jids;
            if (rm.skinned) {
                jids = rm.jointIds;
                if (n.skin >= 0 && n.skin < static_cast<int>(out.skins.size())) {
                    const gfx::Skin& sk = out.skins[static_cast<size_t>(n.skin)];
                    for (uint16_t& ji : jids)
                        if (ji < sk.joints.size())
                            ji = static_cast<uint16_t>(sk.joints[ji]);
                    // Mirror the remap into the vertex payload (GPU source).
                    for (size_t vi = 0; vi < rm.verts.size() && vi * 4 + 3 < jids.size();
                         ++vi)
                        for (int c = 0; c < 4; ++c)
                            rm.verts[vi].j[c] =
                                static_cast<float>(jids[vi * 4 + c]);
                }
            }
            gfx::Mesh mesh =
                rm.needsU32
                    ? gfx::Mesh::CreateFromDataU32(*renderer_, rm.verts.data(),
                                                    static_cast<uint32_t>(rm.verts.size()),
                                                    rm.indicesU32.data(),
                                                    static_cast<uint32_t>(rm.indicesU32.size()),
                                                    "gltf")
                    : gfx::Mesh::CreateFromData(*renderer_, rm.verts.data(),
                                                static_cast<uint32_t>(rm.verts.size()),
                                                rm.indices.data(),
                                                static_cast<uint32_t>(rm.indices.size()),
                                                "gltf");
            if (mesh.Valid()) {
                if (rm.skinned)
                    mesh.AttachSkinData(std::move(jids), rm.jointWeights, n.skin);
                // Track under a per-node key so AssetManager::Stats() counts
                // glTF meshes in the resource panel (meshes_ otherwise only
                // holds OBJ meshes). Re-parsing the file re-inserts the key.
                const std::string meshKey =
                    path + "#" + std::to_string(idx) + ":" + std::to_string(r);
                meshes_[meshKey] = mesh;
                meshRefs_[meshKey] = 1;
                out.nodes.push_back({world, mesh, rm.material});
            }
            }  // for r: 该 mesh 的所有 primitives
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
    // Cache the parsed result (mesh/material handles are shared GPU resources;
    // copying the struct per caller is cheap compared to re-uploading).
    if (mtime != 0) {
        gltfs_[path] = out;
        gltfMtimes_[path] = mtime;
    }
    NEON_LOG_INFO("GLTF: loaded '%s' (%zu nodes)", path.c_str(), out.nodes.size());
    return out;
}

gfx::Font AssetManager::LoadFont(const std::string& path, int pixelHeight) {
    auto key = std::make_pair(path, pixelHeight);
    auto cached = fonts_.find(key);
    if (cached != fonts_.end()) return cached->second;

    const core::Result<std::vector<uint8_t>> bytes = IoRead(path);
    if (!bytes.Ok()) {
        NEON_LOG_ERROR("Asset: failed to open font '%s'", path.c_str());
        return {};
    }
    gfx::Font font = renderer_->CreateFontFromMemory(bytes.Value().data(), bytes.Value().size(),
                                                     pixelHeight);
    fonts_[key] = font;
    return font;
}

gfx::Font AssetManager::LoadSystemCJKFont(int pixelHeight) {
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

    for (const char* candidate : kCandidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (!in.is_open()) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        gfx::Font font = renderer_->CreateFontFromMemoryWithCodepoints(
            data.data(), data.size(), pixelHeight, kSystemCjkCodepoints,
            static_cast<int>(sizeof(kSystemCjkCodepoints) / sizeof(kSystemCjkCodepoints[0])));
        if (font.Valid()) {
            NEON_LOG_INFO("Asset: loaded system CJK font '%s' (%d pre-baked glyphs)", candidate,
                          static_cast<int>(sizeof(kSystemCjkCodepoints) /
                                           sizeof(kSystemCjkCodepoints[0])));
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
    stats.retiredTextures = retiredTextures_.size();
    stats.retiredMeshes = retiredMeshes_.size();
    stats.reclaimedTextures = reclaimedTextures_;
    stats.reclaimedMeshes = reclaimedMeshes_;
    return stats;
}

// ---------------------------------------------------------------------------
// Resource lifecycle (P0-3)
// ---------------------------------------------------------------------------
namespace {
// Frames a retired resource must survive before its GPU storage is destroyed.
// The renderer submits work asynchronously; a resource released mid-frame can
// still be referenced by commands queued this frame, so a 2-frame window keeps
// every in-flight command buffer safe.
constexpr uint64_t kReclaimDelayFrames = 2;
} // namespace

gfx::Texture AssetManager::AcquireTexture(const std::string& path,
                                          const TextureLoadOptions& opts) {
    const std::string key = TextureCacheKey(path, opts);
    // A retired (pending-reclaim) entry with the same key is revived instead of
    // being re-uploaded: pop it from the retire queue and keep the GPU data.
    for (auto it = retiredTextures_.begin(); it != retiredTextures_.end(); ++it) {
        if (it->key == key) {
            gfx::Texture tex = it->tex;
            textures_[key] = tex;
            textureRefs_[key] = 1;
            retiredTextures_.erase(it);
            return tex;
        }
    }
    return LoadTexture(path, opts);
}

void AssetManager::ReleaseTexture(const std::string& path, const TextureLoadOptions& opts) {
    ReleaseTextureKey(TextureCacheKey(path, opts));
}

// A9: release by full cache key (the only correct identity -- a path alone
// misses flip/wrap variants loaded by glTF or materials).
void AssetManager::ReleaseTextureKey(const std::string& key) {
    auto refIt = textureRefs_.find(key);
    if (refIt == textureRefs_.end() || refIt->second == 0) return;
    if (--refIt->second > 0) return;

    auto texIt = textures_.find(key);
    if (texIt == textures_.end()) {
        textureRefs_.erase(refIt);
        return;
    }
    retiredTextures_.push_back({key, texIt->second, static_cast<uint32_t>(pumpFrame_)});
    textureRefs_.erase(refIt);
    textures_.erase(texIt);
}

gfx::Mesh AssetManager::AcquireMeshOBJ(const std::string& path) {
    for (auto it = retiredMeshes_.begin(); it != retiredMeshes_.end(); ++it) {
        if (it->key == path) {
            gfx::Mesh mesh = it->mesh;
            meshes_[path] = mesh;
            meshRefs_[path] = 1;
            retiredMeshes_.erase(it);
            return mesh;
        }
    }
    return LoadMeshOBJ(path);
}

void AssetManager::ReleaseMeshOBJ(const std::string& path) {
    auto refIt = meshRefs_.find(path);
    if (refIt == meshRefs_.end() || refIt->second == 0) return;
    if (--refIt->second > 0) return;

    auto meshIt = meshes_.find(path);
    if (meshIt == meshes_.end()) {
        meshRefs_.erase(refIt);
        return;
    }
    retiredMeshes_.push_back({path, meshIt->second, static_cast<uint32_t>(pumpFrame_)});
    meshRefs_.erase(refIt);
    meshes_.erase(meshIt);
}

size_t AssetManager::AcquireChunkAssets(const std::vector<std::string>& refs) {
    size_t held = 0;
    for (const std::string& r : refs) {
        if (r.empty()) continue;
        if (r.compare(0, 4, "obj:") == 0) {
            if (AcquireMeshOBJ(r.substr(4)).Valid()) ++held;
            continue;
        }
        // Procedural mesh keys ("terrain", "cube", ...) are built by the
        // renderer, not cached here; glTF assets are multi-mesh and not
        // addressable by a single cache key - both are skipped.
        if (r.find('/') == std::string::npos && r.find('\\') == std::string::npos &&
            r.find('.') == std::string::npos)
            continue;
        if (r.compare(0, 5, "gltf:") == 0) continue;
        // A9: acquire every cached variant of the path (glTF loads textures
        // with Repeat wrap, materials may load flipped) so re-entering a chunk
        // revives exactly what the first visit loaded. Variants may sit in the
        // live cache or in the retire queue (both revive via AcquireTexture).
        std::vector<std::string> variantKeys;
        for (const auto& kv : textures_)
            if (KeyPathOf(kv.first) == r) variantKeys.push_back(kv.first);
        for (const auto& r2 : retiredTextures_)
            if (KeyPathOf(r2.key) == r &&
                std::find(variantKeys.begin(), variantKeys.end(), r2.key) == variantKeys.end())
                variantKeys.push_back(r2.key);
        if (variantKeys.empty()) {
            if (AcquireTexture(r).Valid()) ++held;
        } else {
            for (const std::string& k : variantKeys) {
                if (AcquireTexture(r, OptsFromKey(k)).Valid()) ++held;
            }
        }
    }
    return held;
}

void AssetManager::ReleaseChunkAssets(const std::vector<std::string>& refs) {
    for (const std::string& r : refs) {
        if (r.empty()) continue;
        if (r.compare(0, 4, "obj:") == 0) {
            ReleaseMeshOBJ(r.substr(4));
            continue;
        }
        if (r.compare(0, 5, "gltf:") == 0) continue;
        if (r.find('/') == std::string::npos && r.find('\\') == std::string::npos &&
            r.find('.') == std::string::npos)
            continue;
        // A9: release every cached variant -- releasing only the default-opts
        // key left glTF's Repeat-wrapped entries referenced forever (GPU
        // memory only grew while streaming chunks). Releasing an uncached
        // path is a harmless no-op inside ReleaseTextureKey.
        std::vector<std::string> keys;
        for (const auto& kv : textureRefs_)
            if (KeyPathOf(kv.first) == r) keys.push_back(kv.first);
        for (const std::string& k : keys) ReleaseTextureKey(k);
    }
}

size_t AssetManager::TextureRefCount(const std::string& path,
                                     const TextureLoadOptions& opts) const {
    auto it = textureRefs_.find(TextureCacheKey(path, opts));
    return it == textureRefs_.end() ? 0 : it->second;
}

size_t AssetManager::MeshRefCount(const std::string& path) const {
    auto it = meshRefs_.find(path);
    return it == meshRefs_.end() ? 0 : it->second;
}

void AssetManager::ReclaimRetired(uint64_t frame) {
    if (renderer_ && renderer_->Backend()) {
        auto& backend = *renderer_->Backend();
        for (auto it = retiredTextures_.begin(); it != retiredTextures_.end();) {
            if (frame - it->frame < kReclaimDelayFrames) {
                ++it;
                continue;
            }
            backend.DestroyTexture(it->tex.Handle());
            it = retiredTextures_.erase(it);
            ++reclaimedTextures_;
        }
        for (auto it = retiredMeshes_.begin(); it != retiredMeshes_.end();) {
            if (frame - it->frame < kReclaimDelayFrames) {
                ++it;
                continue;
            }
            backend.DestroyMesh(it->mesh.Handle());
            it = retiredMeshes_.erase(it);
            ++reclaimedMeshes_;
        }
    } else {
        // No renderer (unit-test fixture): drop the queue entries directly.
        retiredTextures_.clear();
        retiredMeshes_.clear();
    }
}

bool AssetManager::TextureChangedOnDisk(const std::string& path) const {
    // A9: mtime entries are keyed by full cache key; check every variant of
    // the path (raw-path lookups missed flip/wrap entries forever).
    for (const auto& kv : textureMtimes_) {
        if (KeyPathOf(kv.first) != path) continue;
        if (kv.second != IoMTime(path)) return true;
    }
    return false;
}

bool AssetManager::MeshChangedOnDisk(const std::string& path) const {
    auto it = meshMtimes_.find(path);
    if (it == meshMtimes_.end()) return false;
    return it->second != IoMTime(path);
}

void AssetManager::ReloadTexture(const std::string& path) {
    if (!TextureChangedOnDisk(path)) return;
    NEON_LOG_INFO("Asset: hot-reload texture '%s'", path.c_str());
    // A9: reload EVERY cached variant (flip/wrap suffixes) and retire the old
    // GPU handles instead of erasing them -- erasing could destroy a texture
    // still referenced by commands queued this frame (use-after-free window).
    std::vector<std::string> keys;
    for (const auto& kv : textures_)
        if (KeyPathOf(kv.first) == path) keys.push_back(kv.first);
    if (keys.empty()) keys.push_back(path); // never loaded: plain reload
    for (const std::string& key : keys) {
        const TextureLoadOptions opts = OptsFromKey(key);
        const uint32_t refs = textureRefs_[key];  // preserve owners across reload
        auto texIt = textures_.find(key);
        if (texIt != textures_.end()) {
            retiredTextures_.push_back({key, texIt->second, static_cast<uint32_t>(pumpFrame_)});
            textures_.erase(texIt);
        }
        textureMtimes_.erase(key);
        textureRefs_.erase(key);
        LoadTexture(path, opts);
        if (refs > 0) textureRefs_[key] = refs;
    }
}

void AssetManager::ReloadMeshOBJ(const std::string& path) {
    if (!MeshChangedOnDisk(path)) return;
    NEON_LOG_INFO("Asset: hot-reload OBJ '%s'", path.c_str());
    const uint32_t refs = meshRefs_[path];  // preserve owners across reload
    // A9: retire the old GPU mesh instead of dropping the handle outright.
    auto meshIt = meshes_.find(path);
    if (meshIt != meshes_.end()) {
        retiredMeshes_.push_back({path, meshIt->second, static_cast<uint32_t>(pumpFrame_)});
        meshes_.erase(meshIt);
    }
    meshMtimes_.erase(path);
    meshRefs_.erase(path);
    LoadMeshOBJ(path);
    if (refs > 0) meshRefs_[path] = refs;
}

} // namespace neon::assets
