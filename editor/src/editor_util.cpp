// Shared editor utilities. These file/string/color/transform/camera helpers
// used to live in editor.cpp's anonymous namespace (and panels.cpp kept its own
// copies of ColorFromHex etc.), so each editor TU reimplemented the same logic.
// Moving them to one translation unit gives every TU a single implementation.
#include "editor_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#undef DrawText // windows.h maps DrawText -> DrawTextA; keep the renderer API
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#endif

#include "neon/assets/mesh_format.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

// Project-relative asset path for a file (absolute or relative). Resolves the
// project dir against the cwd so absolute paths under it still convert.
std::string ToProjectRelPath(const std::string& path, const std::string& projectDir) {
    std::string base = projectDir.empty() ? "." : projectDir;
    if (base == ".") {
        // Keep paths already relative ("scripts/..."); strip a leading "./".
        if (path.compare(0, 2, "./") == 0) return path.substr(2);
        return path;
    }
    std::string absBase = base;
    const bool baseAbsolute = absBase.size() >= 2 && absBase[1] == ':' ||
                              (!absBase.empty() && (absBase[0] == '/' || absBase[0] == '\\'));
    if (!baseAbsolute) absBase = GetWorkingDir() + "/" + absBase;
    std::string normBase = base;
    if (normBase.back() != '/' && normBase.back() != '\\') normBase += '/';
    std::string normPath = path;
    if (normPath.rfind(normBase, 0) == 0) return normPath.substr(normBase.size());
    std::string normAbsBase = absBase;
    if (normAbsBase.back() != '/' && normAbsBase.back() != '\\') normAbsBase += '/';
    if (normPath.rfind(normAbsBase, 0) == 0) return normPath.substr(normAbsBase.size());
    std::string dotBase = "./" + base;
    if (normPath.rfind(dotBase, 0) == 0)
        return normPath.substr(dotBase.size() + 1); // skip "./" + base + "/"
    return path;
}

gfx::Color ColorFromHex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return gfx::Color::White;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        return 255u;
    };
    auto byte = [&](char hi, char lo) {
        return static_cast<float>(((nibble(hi) << 4) | nibble(lo)) / 255.0);
    };
    return {byte(hex[1], hex[2]), byte(hex[3], hex[4]), byte(hex[5], hex[6]), 1.0f};
}

std::string ColorToHex(const gfx::Color& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", static_cast<int>(c.r * 255.0f),
                  static_cast<int>(c.g * 255.0f), static_cast<int>(c.b * 255.0f));
    return buf;
}

std::string DirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos + 1);
}

std::string BaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Map an editor mesh key to the runtime-loadable key written into an exported
// scene. File-backed built-ins resolve to their asset paths; procedural
// primitives ("terrain", "cube") and already-prefixed keys ("obj:", "gltf:")
// pass through verbatim.
std::string ExportMeshKey(const std::string& key) {
    if (key == "helmet") return "gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf";
    // Procedural props (tree/house/npc/bush/rock/water/road/terrain) keep their
    // bare key: the runtime regenerates the same meshes via scene_props.
    return key;
}

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

// Create every missing path component (accepts '/' and '\' separators).
bool EnsureDirs(const std::string& path) {
    std::string acc;
    size_t i = 0;
    while (i < path.size()) {
        size_t next = path.find_first_of("/\\", i);
        if (next == std::string::npos) next = path.size();
        std::string comp = path.substr(i, next - i);
        i = next + 1;
        if (!acc.empty() && !comp.empty()) {
            acc += "/" + comp;
        } else if (acc.empty()) {
            acc = comp;
        } else {
            continue; // duplicate separator; keep going
        }
        if (acc.empty() || acc == ".") continue;
        // A Windows drive root like "C:" already exists; skip creation.
        if (acc.size() == 2 && acc[1] == ':') continue;
        if (!MakeDir(acc)) return false;
    }
    return true;
}

std::string GetTempDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string dir(buf);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir.empty() ? "." : dir;
#else
    const char* t = std::getenv("TMPDIR");
    if (t && *t) return t;
    return "/tmp";
#endif
}

// Write a file whose name may contain non-ASCII (CJK) characters. On Windows
// std::ofstream would use the ANSI codepage, and MinGW's libstdc++ has no
// std::wstring overload for fstream, so write through the wide API instead.
bool WriteFileUtf8(const std::string& path, const std::string& content) {
#if defined(_WIN32)
    const int wl = MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                       static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wl > 0 ? wl : 0), L'\0');
    if (wl > 0)
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                            &wpath[0], wl);
    FILE* f = _wfopen(wpath.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    const bool ok =
        content.empty() || std::fwrite(content.data(), 1, content.size(), f) == content.size();
    std::fclose(f);
    return ok;
}

std::string GetWorkingDir() {
#if defined(_WIN32)
    char buf[4096];
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    char buf[4096];
    if (::getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

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

// Pushes a file's mtime forward (hot-reload smoke test uses this to simulate
// an on-disk edit without relying on same-second filesystem timestamps).
bool TouchFileMTime(const std::string& path, int64_t offsetSeconds) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER ul;
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    ul.QuadPart += static_cast<unsigned __int64>(offsetSeconds) * 10000000ull;
    FILETIME shifted;
    shifted.dwLowDateTime = ul.LowPart;
    shifted.dwHighDateTime = ul.HighPart;
    SetFileTime(h, nullptr, &shifted, &shifted);
    CloseHandle(h);
    return true;
#else
    struct utimbuf ub;
    ub.actime = static_cast<time_t>(std::time(nullptr) + offsetSeconds);
    ub.modtime = ub.actime;
    return ::utime(path.c_str(), &ub) == 0;
#endif
}

// Lower-cased file extension with the leading dot, e.g. ".obj".
std::string ExtLower(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Scene picker label: base name WITHOUT the ".json" suffix. Scenes are the
// editor's primary objects; the extension is UI noise (storage keeps the full
// name so scene matching logic is unaffected).
std::string SceneDisplayName(const std::string& path) {
    std::string n = BaseName(path);
    if (ExtLower(n) == ".json") n = n.substr(0, n.size() - 5);
    return n;
}

// Resolves a scene mesh asset path to a loadable file. Scene files store
// project-relative paths ("assets/models/x.gltf"), so when a project is open
// probe <project>/<rel> first, then fall back to the raw path (repo-root
// assets used by the default sandbox and bundled demos). Absolute paths pass
// through unchanged.
std::string ResolveMeshAssetPath(const std::string& rel, const std::string& projectDir) {
    const bool absolute = rel.size() >= 2 && rel[1] == ':' ||
                          (!rel.empty() && (rel[0] == '/' || rel[0] == '\\'));
    if (absolute) return rel;
    auto exists = [](const std::string& f) {
        std::ifstream probe(f, std::ios::binary);
        return probe.is_open();
    };
    if (!projectDir.empty() && projectDir != "." && exists(projectDir + "/" + rel))
        return projectDir + "/" + rel;
    return rel;
}

// Maps a mesh key to the file it loads (file-prefixed keys verbatim and the
// file-backed built-in "helmet"). "" for procedural primitives ("terrain",
// "tree", "house", "npc", "bush", "rock", "water", "road", "cube") that have
// no on-disk asset to hot-reload.
std::string MeshKeyAssetPath(const std::string& key, const std::string& projectDir) {
    std::string rel;
    if (key == "helmet") rel = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
    else if (assets::MeshFormatRegistry::Instance().MatchPrefix(key, &rel).empty())
        return {};  // not a file-backed mesh key
    // obj/gltf/fbx: the meshKey's path suffix IS the asset path.
    return ResolveMeshAssetPath(rel, projectDir);
}

// True for props that bake their colors into vertex data (the lit shader
// multiplies uTint * vColor). Their material tint must stay WHITE so the baked
// colors show through instead of being double-tinted. npc:r,g,b is the runtime
// form of the villager (tunic tint encoded in the mesh key).
bool IsBakedColorKey(const std::string& key) {
    return key == "terrain" || key == "tree" || key == "house" || key == "npc" ||
           key == "bush" || key == "hero" || key == "wolf" || key.compare(0, 4, "npc:") == 0;
}

// Unprojects a point from clip space to a world ray for the given camera.
// ndcX/ndcY are in [-1, 1] (y-up, matching the renderer's screen→NDC mapping:
// ndcX = px*2/width-1, ndcY = 1 - py*2/height).
math::Ray RayFromNDC(const gfx::Camera& cam, float aspect, float ndcX, float ndcY) {
    math::Vec3 fwd = (cam.target - cam.position).Normalized();
    math::Vec3 right = math::Cross(fwd, cam.up).Normalized();
    math::Vec3 upv = math::Cross(right, fwd);
    if (cam.ortho) {
        // Ortho picking: every ray is parallel to the forward axis, through the
        // mouse point on the camera plane (not through the eye).
        float halfH = cam.orthoSize;
        float halfW = halfH * (aspect > 0.01f ? aspect : 0.01f);
        math::Vec3 origin = cam.position + right * (ndcX * halfW) + upv * (ndcY * halfH);
        return {origin, fwd};
    }
    float tanF = std::tan(cam.fovY * 0.5f);
    math::Vec3 dir = (fwd + right * ndcX * tanF * aspect + upv * ndcY * tanF).Normalized();
    return {cam.position, dir};
}

// Design-space variant used by the smoke tests (design resolution 1280x720).
// NOTE: picking uses RayFromNDC directly from client-pixel coordinates so it
// stays correct at any window aspect; converting design→NDC only matches the
// full-window render when the window is exactly 16:9.
math::Ray ScreenRay(const gfx::Camera& cam, float aspect, const math::Vec2& designPos) {
    const float ndcX = designPos.x / static_cast<float>(gfx::Renderer::kDesignWidth) * 2.0f - 1.0f;
    const float ndcY = 1.0f - designPos.y / static_cast<float>(gfx::Renderer::kDesignHeight) * 2.0f;
    return RayFromNDC(cam, aspect, ndcX, ndcY);
}

// Projects a world point to ImGui screen pixels (y-down) under the given camera
// and scene rect. Shared by the gizmo overlay (DrawSceneGizmos) and the drag
// smoke simulation; both used to inline the same view-projection math.
bool WorldToScreenImGui(const gfx::Camera& cam, float aspect, const math::Rect2& vp,
                        const math::Vec3& w, math::Vec2& out) {
    const math::Vec4 clip =
        cam.ViewProjection(aspect).TransformVec4(math::Vec4(w.x, w.y, w.z, 1.0f));
    if (clip.w <= 0.01f) return false;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    out.x = vp.x + (nx * 0.5f + 0.5f) * vp.w;
    out.y = vp.y + (0.5f - ny * 0.5f) * vp.h;
    return true;
}

// ImGuizmo matrix boundary: the engine's row-major Mat4 <-> ImGuizmo's
// column-major float[16] is a transpose: element (r, c) -> gizmo index c*4 + r.
void Mat4ToGizmo(const math::Mat4& m, float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = m.m[r * 4 + c];
    }
}

void GizmoToMat4(const float in[16], math::Mat4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m.m[r * 4 + c] = in[c * 4 + r];
    }
}

// Rebuild a SceneEntity's TRS from a decomposed row-major matrix. The engine
// composes model matrices as T*R*S (column-vector convention: v' = M*v), so
// scale is carried by the COLUMNS of the 3x3 block: column j = scale_j * R_j.
// Translation is m[3]/m[7]/m[11]. This is the inverse of
// Translation(pos) * rot.ToMat4() * Scale(scale).
void DecomposeModel(const math::Mat4& m, math::Vec3& pos, math::Vec3& scale, math::Quat& rot) {
    pos = {m.m[3], m.m[7], m.m[11]};
    math::Vec3 col0{m.m[0], m.m[4], m.m[8]};
    math::Vec3 col1{m.m[1], m.m[5], m.m[9]};
    math::Vec3 col2{m.m[2], m.m[6], m.m[10]};
    scale = {col0.Length(), col1.Length(), col2.Length()};
    math::Vec3 r0 = col0.Normalized();
    math::Vec3 r1 = col1.Normalized();
    math::Vec3 r2 = col2.Normalized();
    // A mirror (det < 0, e.g. a negative scale axis) must be folded into the
    // scale so the extracted rotation stays proper (det +1) and recomposing
    // T*R*S reproduces the source matrix exactly.
    if (math::Dot(r0, math::Cross(r1, r2)) < 0.0f) {
        r0 = -r0;
        scale.x = -scale.x;
    }
    // Feed Mat4ToQuat a pure rotation matrix built from the normalized columns;
    // Mat4ToQuat's row-based Shepperd extraction is exact on orthonormal rows.
    math::Mat4 rotM;
    rotM.m[0] = r0.x;  rotM.m[4] = r0.y;  rotM.m[8] = r0.z;
    rotM.m[1] = r1.x;  rotM.m[5] = r1.y;  rotM.m[9] = r1.z;
    rotM.m[2] = r2.x;  rotM.m[6] = r2.y;  rotM.m[10] = r2.z;
    rot = math::Mat4ToQuat(rotM);
}

// --- Play SFX: a tiny procedural synth so 2D games (NeonPvZ etc.) have
// sound without shipping audio files. PlaySfx(name) maps to generated PCM.
namespace {

constexpr double kSfxRate = 44100.0;
uint64_t g_sfxNoise = 0x9E3779B97F4A7C15ull;

double SfxNoise() {
    g_sfxNoise ^= g_sfxNoise << 13;
    g_sfxNoise ^= g_sfxNoise >> 7;
    g_sfxNoise ^= g_sfxNoise << 17;
    return static_cast<double>((g_sfxNoise >> 11) & 0x7FFFFF) / 4194303.0 * 2.0 - 1.0;
}

enum class SfxWave { Sine, Square, Saw, Noise };

void SfxTone(std::vector<int16_t>& out, double f0, double f1, float dur, float vol,
             SfxWave wave, float attack = 0.01f, float release = 0.05f) {
    const size_t count = static_cast<size_t>(dur * kSfxRate);
    const size_t start = out.size();
    out.resize(start + count, 0);
    for (size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / kSfxRate;
        const double frac = static_cast<double>(i) / static_cast<double>(count);
        const double freq = f0 + (f1 - f0) * frac;
        double s = 0.0;
        switch (wave) {
            case SfxWave::Sine: s = std::sin(math::kTwoPi * freq * t); break;
            case SfxWave::Square: s = std::sin(math::kTwoPi * freq * t) >= 0.0 ? 0.6 : -0.6; break;
            case SfxWave::Saw: s = frac * 2.0 - 1.0; break;
            case SfxWave::Noise: s = SfxNoise(); break;
        }
        double env = 1.0;
        if (i < count * attack) env = static_cast<double>(i) / (count * attack);
        if (count - i < count * release)
            env = static_cast<double>(count - i) / (count * release);
        out[start + i] = static_cast<int16_t>(s * vol * env * 32767.0);
    }
}

} // namespace

// Resample a SoundFx to the backend's fixed 44.1 kHz rate. The mixer consumes
// every voice's samples at the device rate (no per-voice resampling), so a WAV
// recorded at 22050 Hz must be upsampled or it plays back twice as fast.
neon::audio::SoundFx ResampleTo44100(neon::audio::SoundFx fx) {
    if (fx.sampleRate == 44100 || fx.samples.empty()) return fx;
    const double ratio = static_cast<double>(fx.sampleRate) / 44100.0;
    const size_t outCount = static_cast<size_t>(static_cast<double>(fx.samples.size()) / ratio);
    neon::audio::SoundFx out;
    out.name = std::move(fx.name);
    out.sampleRate = 44100;
    out.loop = fx.loop;
    out.volume = fx.volume;
    out.samples.reserve(outCount);
    for (size_t i = 0; i < outCount; ++i) {
        const double srcPos = static_cast<double>(i) * ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        const size_t i1 = std::min(i0 + 1, fx.samples.size() - 1);
        const double frac = srcPos - static_cast<double>(i0);
        const double s = static_cast<double>(fx.samples[i0]) * (1.0 - frac) +
                         static_cast<double>(fx.samples[i1]) * frac;
        out.samples.push_back(static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, s))));
    }
    return out;
}

neon::audio::SoundFx MakePvzSfx(const std::string& name, const std::string& projectDir) {
    // 真实音效优先：项目自带 assets/audio/<name>.(wav|ogg|mp3)（素材包直用，
    // ma_decoder 全格式解码）。命中缓存避免每次 stat/parse；
    // sampleRate<=0 表示"无文件"，回退程序合成。
    static std::unordered_map<std::string, neon::audio::SoundFx> s_pvzWav;
    const std::string cacheKey = projectDir + "#" + name;
    auto wavIt = s_pvzWav.find(cacheKey);
    if (wavIt == s_pvzWav.end()) {
        neon::audio::SoundFx wav;
        bool loaded = false;
        const std::string base =
            projectDir.empty() ? std::string("assets/audio/") : projectDir + "/assets/audio/";
        static const char* kExts[] = {".wav", ".ogg", ".mp3"};
        for (const char* ext : kExts) {
            if (neon::audio::LoadSoundFx(base + name + ext, wav)) {
                loaded = true;
                break;
            }
        }
        if (!loaded && projectDir != "projects/pvz") {
            // 兼容从仓库根运行的编辑器与项目目录混排。
            for (const char* ext : kExts) {
                if (neon::audio::LoadSoundFx(
                        "projects/" + projectDir + "/assets/audio/" + name + ext, wav)) {
                    loaded = true;
                    break;
                }
            }
        }
        if (loaded) wav = ResampleTo44100(std::move(wav));
        wav.sampleRate = loaded ? wav.sampleRate : 0; // 0 = 回退程序合成
        NEON_LOG_DEBUG("MakePvzSfx: '%s' %s (%zu samples @ %u Hz)", name.c_str(),
                       loaded ? "loaded from file" : "no file, synth fallback",
                       wav.samples.size(), wav.sampleRate);
        wavIt = s_pvzWav.emplace(cacheKey, std::move(wav)).first;
    }
    if (wavIt->second.sampleRate > 0) return wavIt->second;

    neon::audio::SoundFx fx;
    fx.sampleRate = 44100;
    if (name == "click") {
        SfxTone(fx.samples, 700, 700, 0.05f, 0.35f, SfxWave::Square);
    } else if (name == "sun") {
        SfxTone(fx.samples, 1200, 1900, 0.14f, 0.30f, SfxWave::Sine);
    } else if (name == "plant") {
        SfxTone(fx.samples, 280, 520, 0.09f, 0.45f, SfxWave::Square);
    } else if (name == "shoot") {
        SfxTone(fx.samples, 160, 110, 0.07f, 0.35f, SfxWave::Noise);
    } else if (name == "boom") {
        SfxTone(fx.samples, 90, 40, 0.45f, 0.7f, SfxWave::Noise);
        SfxTone(fx.samples, 220, 60, 0.35f, 0.5f, SfxWave::Sine, 0.01f, 0.3f);
    } else if (name == "eat") {
        SfxTone(fx.samples, 120, 90, 0.08f, 0.4f, SfxWave::Square, 0.02f, 0.05f);
        SfxTone(fx.samples, 110, 85, 0.10f, 0.4f, SfxWave::Square, 0.02f, 0.05f);
    } else if (name == "mower") {
        SfxTone(fx.samples, 220, 520, 0.5f, 0.35f, SfxWave::Saw);
    } else if (name == "zombie") {
        SfxTone(fx.samples, 90, 65, 0.35f, 0.5f, SfxWave::Sine, 0.05f, 0.2f);
        SfxTone(fx.samples, 70, 55, 0.4f, 0.4f, SfxWave::Sine, 0.05f, 0.25f);
    } else if (name == "wave") {
        SfxTone(fx.samples, 500, 500, 0.18f, 0.35f, SfxWave::Square, 0.01f, 0.15f);
        SfxTone(fx.samples, 700, 700, 0.18f, 0.35f, SfxWave::Square, 0.01f, 0.15f);
    } else if (name == "win") {
        SfxTone(fx.samples, 523, 523, 0.14f, 0.4f, SfxWave::Sine);
        SfxTone(fx.samples, 659, 659, 0.14f, 0.4f, SfxWave::Sine);
        SfxTone(fx.samples, 784, 784, 0.24f, 0.4f, SfxWave::Sine);
    } else if (name == "lose") {
        SfxTone(fx.samples, 392, 392, 0.18f, 0.4f, SfxWave::Sine);
        SfxTone(fx.samples, 330, 330, 0.18f, 0.4f, SfxWave::Sine);
        SfxTone(fx.samples, 262, 230, 0.45f, 0.4f, SfxWave::Sine);
    }
    return fx;
}

} // namespace neon::editor
