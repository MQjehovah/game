#include "editor.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <windows.h>
#undef DrawText // windows.h maps DrawText -> DrawTextA; keep the renderer API
#else
#include <sys/stat.h>
#include <utime.h>
#endif

#include "editor_history.hpp"
#include "font_data.hpp"

#include "imgui_internal.h"
#include "neon/core/json.hpp"
#include "neon/gfx/imgui_neon.hpp"
#include "neon/gfx/scene_props.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace neon::editor {
// Defined in panels.cpp (asset path helper shared by the editor).
std::string ToProjectRelPath(const std::string& path, const std::string& projectDir);

namespace {
// 2D level layout (must match projects/pvz/scripts/pvz.lua: 9x5 cells of 100
// at (190,160)). LoadScene parses plant/zombie entities into these vectors so
// 2D level data stays scene-driven (the editor does not draw a canvas itself).
constexpr int kPvzRows = 5;
constexpr int kPvzCols = 9;
const char* kPvzPlantNames[5] = {"sunflower", "peashooter", "wallnut", "snowpea", "cherry"};
const char* kPvzZombieNames[3] = {"basic", "cone", "bucket"};

// --- Playtest SFX: a tiny procedural synth so 2D games (NeonPvZ etc.) have
// sound without shipping audio files. PlaySfx(name) maps to generated PCM.
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

neon::audio::SoundFx MakePvzSfx(const std::string& name) {
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

// File modification time in seconds (0 when the file does not exist).
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

// Maps a mesh key to the file it loads (file-prefixed keys verbatim and the
// file-backed built-in "helmet"). "" for procedural primitives ("terrain",
// "tree", "house", "npc", "bush", "rock", "water", "road", "cube") that have
// no on-disk asset to hot-reload.
std::string MeshKeyAssetPath(const std::string& key) {
    if (key == "helmet") return "assets/models/DamagedHelmet/DamagedHelmet.gltf";
    if (key.rfind("obj:", 0) == 0) return key.substr(4);
    if (key.rfind("gltf:", 0) == 0) return key.substr(5);
    return {};
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

// ---------------------------------------------------------------------------
// ImGuizmo matrix boundary.
//
// The engine's math::Mat4 is row-major storage: element (row, col) lives at
// m[row * 4 + col] and translation at m[3]/m[7]/m[11]. ImGuizmo expects the
// classic column-major float[16] layout used by OpenGL/glm (right/up/forward
// basis in columns 0/1/2, translation at m[12]/m[13]/m[14]; see ImGuizmo.cpp's
// matrix_t where v.right = m16[0..3], v.position = m16[12..15]). Converting
// is therefore a transpose: element (r, c) -> gizmo index c*4 + r.
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

// Layout version persisted as a versioned marker window in the ImGui ini. When
// the ini is missing or predates the current layout version, the editor
// re-applies the Unity-style default docking layout once (the user's later
// customizations are still saved and respected).
// v3: the built-in script editor is docked into the bottom tab group instead
// of floating - its saved floating position (550,148) covered the left half
// of the Inspector (属性) and swallowed every click on component blocks.
constexpr int kNeonLayoutVersion = 3;
bool NeedsDefaultLayout() {
    static const bool needs = [] {
        const char* ini = ImGui::GetIO().IniFilename;
        if (!ini) return true; // no ini yet -> fresh default
        std::ifstream f(ini);
        if (!f) return true; // unreadable -> treat as fresh
    const std::string marker =
        std::string("[Window][##NeonLayoutVer") + std::to_string(kNeonLayoutVersion) + "]";
        std::string line;
        while (std::getline(f, line))
            if (line.find(marker) != std::string::npos) return false; // current layout saved
        return true; // ini exists but predates this layout version
    }();
    return needs;
}

// True for props that bake their colors into vertex data (the lit shader
// multiplies uTint * vColor). Their material tint must stay WHITE so the baked
// colors show through instead of being double-tinted. npc:r,g,b is the runtime
// form of the villager (tunic tint encoded in the mesh key).
bool IsBakedColorKey(const std::string& key) {
    return key == "terrain" || key == "tree" || key == "house" || key == "npc" ||
           key == "bush" || key == "hero" || key == "wolf" || key.compare(0, 4, "npc:") == 0;
}

} // namespace

bool EditorApp::OnCreate() {
    if (disableShadows_) renderer_.SetShadowsEnabled(false);
    renderer_.SetBackendName(backendName_);
    renderer_.SetBloomEnabled(bloomEnabled_);
    renderer_.SetMsaaEnabled(msaaEnabled_);
    renderer_.SetTonemapEnabled(tonemapEnabled_);
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Editor: renderer init failed");
        return false;
    }
    assetMgr_.Init(&renderer_);

    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
        // System CJK font with DYNAMIC glyphs: scene names, panels and
    // playtest HUD render any Chinese text without maintaining a list.
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24);
    if (!gfx::ImGuiNeon_Init(&renderer_, gfx::ImGuiNeon_SystemCJKPath())) {
        NEON_LOG_ERROR("Editor: Dear ImGui init failed");
        return false;
    }
    audioBackend_ = neon::audio::CreatePlatformAudioBackend();
    if (audioBackend_ && !audioBackend_->Init()) {
        audioBackend_->Shutdown();
        audioBackend_.reset();
        NEON_LOG_WARN("Editor: audio unavailable, playtest runs silent");
    }

    SetupScene();
    InitToolPanels();

    LoadEditorConfig();
    NEON_LOG_INFO("NeonEditor ready (%zu entities), project dir '%s'", entities_.size(),
                  projectDir_.c_str());
    // The smoke test is the canonical 3D-editor flow: --2d/--2d-play/--project
    // only matter for interactive sessions. Normalize before any render so the
    // gizmo/camera assertions see the default scene from frame 0.
    if (smokeMode_) {
        editMode_ = EditMode::Scene3D;
        pvzPlaytestOnStart_ = false;
        loadProjectOnStart_ = false;
    }
    // Godot-style: restore the last-opened project from the editor config so
    // the editor reopens where the user left off. Skipped for --project
    // (explicit path wins) and smoke runs (the smoke needs the deterministic
    // default sandbox scene). The 2D/3D button is only a camera change, so it
    // never blocks restoring the user's project.
    if (!smokeMode_ && projectDirOnStart_.empty() && projectDir_ != ".") {
        std::ifstream in(projectDir_ + "/game.json", std::ios::binary);
        if (in.is_open()) SwitchProject(projectDir_);
    }
    // --2d / --2d-play without a 2D project open defaults to the bundled PvZ
    // project so the demo canvas + playtest have plant/zombie content. The
    // toolbar view switcher never changes the project - 2D is just the camera.
    if (editMode_ == EditMode::Scene2D && projectMode_ != "2d" &&
        std::ifstream("projects/pvz/game.json").is_open()) {
        SwitchProject("projects/pvz");
    }
    if (!projectDirOnStart_.empty()) {
        projectDir_ = projectDirOnStart_;
        std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
        projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
        if (loadProjectOnStart_) LoadProjectScene();
    }
    // Start the playtest LAST: LoadProjectScene/SwitchProject above stop any
    // running playtest, so --2d-play + --project must start after both.
    if (pvzPlaytestOnStart_) StartPlaytest();
    LoadInputMapEdit(); // Godot-style input panel data
    return true;
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    if (audioBackend_) {
        audioBackend_->Shutdown();
        audioBackend_.reset();
    }
    // Release the offscreen thumbnail targets + their ImGui registrations
    // before the renderer shuts down.
    if (gfx::IRenderBackend* backend = renderer_.Backend()) {
        for (auto& kv : meshThumbs_) {
            if (kv.second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(kv.second.texHandle);
            if (kv.second.rt.Valid()) backend->DestroyRenderTarget(kv.second.rt);
        }
        for (auto& kv : materialThumbs_) {
            if (kv.second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(kv.second.texHandle);
            if (kv.second.rt.Valid()) backend->DestroyRenderTarget(kv.second.rt);
        }
    }
    meshThumbs_.clear();
    meshThumbQueue_.clear();
    materialThumbs_.clear();
    materialThumbQueue_.clear();
    gfx::ImGuiNeon_Shutdown();
    renderer_.Shutdown();
}

void EditorApp::SetupScene() {
    auto add = [&](const std::string& key, const std::string& name, const math::Vec3& pos,
                   const math::Vec3& scale, const gfx::Color& tint) {
        SceneEntity e;
        e.name = name;
        e.meshKey = key;
        e.pos = pos;
        e.scale = scale;
        e.tint = tint;
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            entities_.push_back(std::move(e));
        }
    };

    // --- Ground: rolling hills with a village pond carved in the SW corner ---
    add("terrain", "地面", {0, 0, 0}, {1, 1, 1}, gfx::Color::White);
    add("water", "湖泊", {-18, -1.15f, -18}, {1.05f, 1, 1.05f}, gfx::Color{0.15f, 0.45f, 0.85f, 1});

    // --- Village (centre): roads, houses, villagers ---
    const float kRoadW = 2.8f;
    add("road", "主干道", {0, 0.03f, 0}, {kRoadW, 1, 17.0f}, gfx::Color{0.44f, 0.39f, 0.32f, 1});
    add("road", "横街", {0, 0.03f, 0}, {15.0f, 1, kRoadW}, gfx::Color{0.40f, 0.36f, 0.30f, 1});
    add("road", "小路_东", {5.5f, 0.03f, -2.5f}, {2.0f, 1, 8.0f}, gfx::Color{0.38f, 0.34f, 0.29f, 1});
    add("road", "小路_西", {-5.5f, 0.03f, -2.5f}, {2.0f, 1, 8.0f}, gfx::Color{0.38f, 0.34f, 0.29f, 1});

    add("house", "农舍_东", {4.6f, 0, 3.2f}, {1.15f, 1.15f, 1.15f}, gfx::Color::White);
    add("house", "旅店", {7.2f, 0, -2.2f}, {1.35f, 1.35f, 1.35f}, gfx::Color::White);
    add("house", "农舍_西", {-4.6f, 0, 3.2f}, {1.15f, 1.15f, 1.15f}, gfx::Color::White);
    add("house", "铁匠铺", {-7.2f, 0, -2.2f}, {1.2f, 1.2f, 1.2f}, gfx::Color::White);

    add("npc", "村民_商人", {1.8f, 0, 1.2f}, {1, 1, 1}, gfx::Color{0.78f, 0.28f, 0.18f, 1});
    add("npc", "村民_农夫", {-1.8f, 0, 1.4f}, {1, 1, 1}, gfx::Color{0.30f, 0.55f, 0.78f, 1});
    add("npc", "村民_猎人", {0.6f, 0, -1.6f}, {1, 1, 1}, gfx::Color{0.48f, 0.42f, 0.20f, 1});
    add("npc", "村民_法师", {3.0f, 0, -0.8f}, {1, 1, 1}, gfx::Color{0.60f, 0.36f, 0.72f, 1});
    add("npc", "村民_卫兵", {-3.0f, 0, -0.8f}, {1.05f, 1.05f, 1.05f}, gfx::Color{0.55f, 0.55f, 0.58f, 1});
    // The DamagedHelmet sits on a plinth as the village's trophy.
    add("helmet", "展示头盔", {0.0f, 0.95f, 2.6f}, {1, 1, 1}, gfx::Color::White);
    add("cube", "展示台", {0.0f, 0.45f, 2.6f}, {1.4f, 0.9f, 1.4f}, gfx::Color{0.55f, 0.42f, 0.30f, 1});

    // --- Wilderness: deterministic scatter of trees / rocks / bushes ---
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFFu) / 65535.0f;
    };
    auto within = [&](float lo, float hi) { return lo + rnd() * (hi - lo); };
    auto nearVillage = [](const math::Vec3& p) { return p.x * p.x + p.z * p.z < 8.5f * 8.5f; };
    auto inLake = [](const math::Vec3& p) {
        float dx = p.x + 18.0f, dz = p.z + 18.0f;
        return dx * dx + dz * dz < 11.5f * 11.5f;
    };
    int treeN = 0, rockN = 0, bushN = 0;
    for (int i = 0; i < 80; ++i) {
        math::Vec3 p{within(-26.0f, 26.0f), 0.0f, within(-26.0f, 26.0f)};
        if (nearVillage(p) || inLake(p)) continue;
        const float r = rnd();
        if (r < 0.52f) {
            add("tree", "松树_" + std::to_string(treeN++), p,
                {0.9f + within(0.0f, 0.7f), 1, 0.9f + within(0.0f, 0.7f)}, gfx::Color::White);
        } else if (r < 0.82f) {
            add("rock", "岩石_" + std::to_string(rockN++), {p.x, 0, p.z},
                {within(0.55f, 1.4f), within(0.45f, 0.9f), within(0.55f, 1.4f)},
                gfx::Color{0.58f, 0.58f, 0.58f, 1});
        } else {
            add("bush", "灌木_" + std::to_string(bushN++), {p.x, 0.15f, p.z}, {1, 1, 1},
                gfx::Color::White);
        }
    }
    // A few trees inside the village edge for shade.
    add("tree", "村口老树", {3.2f, 0, 4.6f}, {1.5f, 1, 1.5f}, gfx::Color::White);
    add("tree", "村口老树_2", {-3.2f, 0, 4.6f}, {1.3f, 1, 1.3f}, gfx::Color::White);
    add("tree", "村口老树_3", {3.4f, 0, -4.4f}, {1.2f, 1, 1.2f}, gfx::Color::White);

    // --- Playable hero: blue-armored figure bound to the hero controller
    // script (WASD/jump/melee/fireball/heal), parked at the south end of the
    // main road. Its health lives in the scene so combat can damage it.
    {
        SceneEntity h;
        h.name = "英雄";
        h.meshKey = "hero";
        h.pos = {0.0f, 0.0f, 5.5f};
        h.scale = {1, 1, 1};
        h.tint = gfx::Color::White;
        h.scripts.push_back({"lua", "scripts/hero.lua", {}});
        h.hp = 100.0f;
        h.maxHp = 100.0f;
        if (ResolveMesh(h)) {
            ApplyMaterialParams(h);
            entities_.push_back(std::move(h));
        }
    }

    // --- Hostile wolves in the wilderness: combat targets for the hero's
    // skills (static for now; the hero can melee/fireball them).
    uint32_t wolfSeed = 0x6D2B79F5u;
    auto wrnd = [&wolfSeed]() {
        wolfSeed ^= wolfSeed << 13;
        wolfSeed ^= wolfSeed >> 17;
        wolfSeed ^= wolfSeed << 5;
        return static_cast<float>(wolfSeed & 0xFFFFu) / 65535.0f;
    };
    for (int i = 0; i < 8; ++i) {
        math::Vec3 wp{10.0f + wrnd() * 14.0f, 0.0f, -12.0f - wrnd() * 12.0f};
        if (i % 2) wp.x = -wp.x;
        add("wolf", "野狼_" + std::to_string(i), wp,
            {1.0f + wrnd() * 0.3f, 1, 1.0f + wrnd() * 0.3f}, gfx::Color::White);
        if (!entities_.empty()) {
            entities_.back().hp = 40.0f;
            entities_.back().maxHp = 40.0f;
        }
    }
    LoadScene("editor_scene.json");
    SetSelection(entities_.empty() ? -1 : 0);
}

void EditorApp::OnUpdate(float dt) {
    // Drain completed async texture decodes (uploads + callbacks, main thread).
    assetMgr_.PumpAsync();
    // The gizmo drag-sim (frame 30) needs the real mouse to hover the viewport
    // so ImGui's hover hit-test yields the dock host window (the window
    // SetAlternativeWindow points at); headless starts at (0,0) over the menu
    // bar, so park it on the viewport center for the smoke frame.
    if (smokeMode_ && TimeRef().frameIndex == 30) {
        platform::InputEvent e;
        e.type = platform::InputEvent::Type::MouseMove;
        e.x = renderer_.ScreenWidth() / 2;
        e.y = renderer_.ScreenHeight() / 2;
        Input()->HandleEvent(e);
    }
    if (showUIEditor_ && uiDocOpen_) {
        // UI editor: the viewport edits the UI document (select/move/resize).
        UpdateUIEditorViewport();
    } else {
        // 2D and 3D share one input path: 2D is just the front-ortho camera,
        // so middle-drag pans the camera target and the wheel zooms the ortho
        // size (the camera frame moves with it).
        UpdateViewport(dt);
        if (playtestActive_ && playtest_) {
            // P1-2 debugger: push edited breakpoints into the playtest host
            // (cheap, only when the set changed).
            if (scriptBreakpointsDirty_ && playtest_->ScriptHost()) {
                for (const auto& kv : scriptBreakpoints_) {
                    std::vector<int> lines(kv.second.begin(), kv.second.end());
                    playtest_->ScriptHost()->SetScriptBreakpoints(kv.first, lines);
                }
                scriptBreakpointsDirty_ = false;
            }
            playtest_->Tick(dt);
        }
    }
    // Hot reload (T4.8): throttled mtime poll for the playtest's scripts and
    // the scene's referenced assets. Off unless --hot / the toolbar toggle.
    if (hotReload_ && TimeRef().frameIndex - hotReloadFrame_ >= 30) {
        hotReloadFrame_ = TimeRef().frameIndex;
        PollHotReload();
    }
    gfx::ImGuiNeon_NewFrame(*Input(), pendingText_, dt);
    pendingText_.clear();
    ImGui::NewFrame();
    // ImGui's implicit fallback window ("Debug##Default") defaults to (60,60)
    // 400x400 - right on top of the viewport's upper-left corner, swallowing
    // the viewport dock tab and the camera input there. Park it off-screen.
    if (ImGuiWindow* fallback = ImGui::FindWindowByName("Debug##Default"))
        ImGui::SetWindowPos(fallback, ImVec2(-100000.0f, -100000.0f));
    // ImGui's hover resolution can report the DockSpace host instead of the
    // docked leaf window under the mouse, which makes every panel button
    // unclickable (ItemHoverable requires HoveredWindow == the item's window).
    // Re-resolve the hover to the topmost visible docked leaf under the mouse.
    {
        ImGuiContext& ictx = *ImGui::GetCurrentContext();
        ImGuiWindow* best = nullptr;
        for (int wi = ictx.Windows.Size - 1; wi >= 0; --wi) {
            ImGuiWindow* w = ictx.Windows[wi];
            if (!w || w->Hidden) continue;
            if (w->DockNodeAsHost != nullptr) continue; // dock host / tab bar
            if (w->ParentWindow != nullptr) continue;   // child windows
            if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue; // overlays
            // The transform gizmo binds to the dock host over the 视口, so
            // keep the host hover there; only re-resolve tool panels.
            if (std::strcmp(w->Name, "视口") == 0) continue;
            if (std::strncmp(w->Name, "##", 2) == 0) continue; // internal windows
            if (w->Rect().Contains(ictx.IO.MousePos)) {
                best = w;
                break;
            }
        }
        if (best && best != ictx.HoveredWindow) {
            ictx.HoveredWindow = best;
        }
    }
    BuildImGuiUI();
    ImGui::Render();

    // Smoke: the 网格 移除 button's action (the command the button pushes)
    // works through the undo stack. Real mouse input cannot be synthesized
    // reliably here - the physical cursor overrides synthetic events every
    // frame - so the action is driven through the same command the button
    // handler pushes, then undone.
    if (smokeMode_ && !smokeRemoveActionDone_ && TimeRef().frameIndex >= 45) {
        smokeRemoveActionDone_ = true;
        for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
            if (!entities_[static_cast<size_t>(i)].meshKey.empty() &&
                entities_[static_cast<size_t>(i)].spriteTex.empty()) {
                SetSelection(i);
                break;
            }
        }
        const bool actionOk =
            selected_ >= 0 && selected_ < static_cast<int>(entities_.size());
        bool removed = false;
        bool healthOk = true;
        bool scriptOk = true;
        if (actionOk) {
            SceneEntity& e = entities_[static_cast<size_t>(selected_)];
            const std::string oldKey = entities_[static_cast<size_t>(selected_)].meshKey;
            history_.Push(std::make_unique<EditMeshKeyCommand>(
                this, &entities_, selected_, oldKey, ""));
            removed = entities_[static_cast<size_t>(selected_)].meshKey.empty();
            history_.Undo();
            // 生命 remove: the command the 移除##health button pushes.
            if (e.maxHp > 0.0f) {
                const HealthValue oldV{e.hp, e.maxHp};
                history_.Push(std::make_unique<EditPropertyCommand<HealthValue>>(
                    &entities_, selected_, ApplyHealth, oldV, HealthValue{},
                    /*mergeable=*/false));
                healthOk = e.maxHp == 0.0f && e.hp == 0.0f;
                history_.Undo();
                healthOk = healthOk && e.maxHp == oldV.maxHp;
            }
            // 脚本 remove: append one script, then erase it via the command
            // the 移除##script_N button pushes.
            std::vector<SceneScriptFields> withScript = e.scripts;
            withScript.push_back({"lua", "scripts/smoke_remove.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, selected_, ApplyScriptList, e.scripts, withScript,
                /*mergeable=*/false));
            if (e.scripts.size() == 1) {
                std::vector<SceneScriptFields> after = e.scripts;
                after.clear();
                history_.Push(std::make_unique<
                    EditPropertyCommand<std::vector<SceneScriptFields>>>(
                    &entities_, selected_, ApplyScriptList, e.scripts, after,
                    /*mergeable=*/false));
                scriptOk = e.scripts.empty();
                history_.Undo();
                history_.Undo();
                scriptOk = scriptOk && e.scripts.empty();
            }
        }
        NEON_LOG_INFO("EDITOR-REMOVE-BTN-SMOKE: [%s] remove actions work "
                      "(mesh=%d health=%d script=%d sel=%d)",
                      actionOk && removed && healthOk && scriptOk ? "PASS" : "FAIL",
                      removed ? 1 : 0, healthOk ? 1 : 0, scriptOk ? 1 : 0, selected_);
        if (!actionOk || !removed || !healthOk || !scriptOk) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 29) {
        showHierarchy_ = true;
        showInspector_ = true;
        showAssets_ = true;
        showResources_ = true;
        showLog_ = true;
        showBt_ = true;
        showScripts_ = true;
        showPackage_ = true;
        showProfiler_ = true;
        // Seed a small tree so the BT canvas renders real nodes on the smoke
        // frame (frame 30) and the smoke can assert the canvas drew geometry.
        btGraph_ = btgraph::BtGraph{};
        const std::string r = btGraph_.AddNode("sequence", math::Vec2{20.f, 20.f});
        const std::string c = btGraph_.AddNode("in_range", math::Vec2{20.f, 180.f});
        const std::string a = btGraph_.AddNode("move_to", math::Vec2{240.f, 180.f});
        core::Json dist;
        dist.type_ = core::Json::Type::Number;
        dist.number_ = 8.0;
        core::Json speed;
        speed.type_ = core::Json::Type::Number;
        speed.number_ = 3.0;
        btGraph_.SetArg(c, "distance", dist);
        btGraph_.SetArg(a, "speed", speed);
        btGraph_.SetParent(c, r);
        btGraph_.SetParent(a, r);
        btSelected_ = r;
    }
    if (smokeMode_ && TimeRef().frameIndex == 30) RunUISmokeTest();

    // T4.8 smoke: the frame-30 OnRender generated the queued mesh thumbnail
    // (the asset panel selected the model during RunUISmokeTest); verify the
    // cache + profiler output here, then arm the ortho render check. The app
    // can run several fixed ticks between renders, so the checks key off
    // lastRenderTick_ (the tick the most recent OnRender processed) rather
    // than assuming a 1:1 tick/render correspondence.
    if (smokeMode_ && TimeRef().frameIndex == 31) {
        NEON_LOG_INFO("EDITOR-PROFILER-SMOKE: [%s] profiler panel populated",
                      profilerDrawn_ ? "PASS" : "FAIL");
        if (!profilerDrawn_) smokeFailed_ = true;
        viewCam_ = ViewCam::Top; // next OnRender renders the top ortho view
    }
    if (smokeMode_ && !thumbSmokeDone_ && !smokeThumbPath_.empty()) {
        // Once the queue has been processed by a render, the cache holds the
        // path (a valid texture = the mesh rendered; an invalid one = the
        // asset failed to load). Until then, keep waiting.
        if (lastRenderTick_ >= 32) {
            auto it = meshThumbs_.find(smokeThumbPath_);
            thumbSmokeDone_ = true;
            const bool ok = it != meshThumbs_.end() &&
                            it->second.texId != ImTextureID_Invalid && it->second.rt.Valid();
            NEON_LOG_INFO("EDITOR-THUMB-SMOKE: [%s] mesh thumbnail cached (%s)",
                          ok ? "PASS" : "FAIL", smokeThumbPath_.c_str());
            if (!ok) smokeFailed_ = true;
        }
    }
    if (smokeMode_ && editMode_ == EditMode::Scene3D && !camSmokeDone_ &&
        viewCam_ == ViewCam::Top) {
        // A render has now processed the Top arm (tick 31): lastRenderCamOrtho_
        // reflects that frame's camera and the scene draw-call count.
        if (lastRenderTick_ >= 32) {
            camSmokeDone_ = true;
            const bool ok = lastRenderCamOrtho_ && smokeDrawCalls_ > 0;
            NEON_LOG_INFO("EDITOR-CAM-SMOKE: [%s] top ortho camera rendered the viewport "
                          "(drawCalls=%u)",
                          ok ? "PASS" : "FAIL", smokeDrawCalls_);
            if (!ok) smokeFailed_ = true;
            viewCam_ = ViewCam::Perspective;
        }
    }

    // T4.8 smoke: hot reload. Frame 40 wires up a temp project with a script,
    // attaches it, starts the playtest and records the script mtime baseline.
    // Frame 41 bumps the file's mtime; frame 42 polls and asserts the playtest
    // was torn down and restarted.
    if (smokeMode_ && TimeRef().frameIndex == 40) {
        const std::string proj = GetTempDir() + "/hotreload_proj";
        EnsureDirs(proj + "/scripts");
        {
            std::ofstream out(proj + "/scripts/main.lua", std::ios::binary);
            out << "function on_start(ent)\nend\nfunction on_update(ent, dt)\nend\n";
        }
        hotReloadProj_ = proj;
        prevProjectDir_ = projectDir_;
        projectDir_ = proj;
        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            SceneEntity& sel = entities_[static_cast<size_t>(selected_)];
            core::Json vars;
            vars.type_ = core::Json::Type::Object;
            std::vector<SceneScriptFields> newList = sel.scripts;
            newList.push_back({"lua", "scripts/main.lua", vars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, selected_, ApplyScriptList, sel.scripts, newList,
                /*mergeable=*/false));
        }
        hotReload_ = true;
        StartPlaytest();
        PollHotReload(); // baseline: record the script's mtime (no restart)
        const bool active = playtestActive_ && playtest_ && playtest_->Running();
        NEON_LOG_INFO("EDITOR-HOTRELOAD-SMOKE: [%s] playtest running for hot reload",
                      active ? "PASS" : "FAIL");
        if (!active) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 41) {
        TouchFileMTime(hotReloadProj_ + "/scripts/main.lua", 2);
    }
    if (smokeMode_ && TimeRef().frameIndex == 42) {
        const int before = hotReloadCount_;
        PollHotReload();
        const bool restarted = hotReloadCount_ > before && playtestActive_ && playtest_;
        NEON_LOG_INFO("EDITOR-HOTRELOAD-SMOKE: [%s] script mtime change restarted the playtest",
                      restarted ? "PASS" : "FAIL");
        if (!restarted) smokeFailed_ = true;
        hotReload_ = false;
        projectDir_ = prevProjectDir_;
    }

    // Godot-style project switcher smoke: ScanProjects discovers both bundled
    // projects, SwitchProject enters the 2D project's canvas with its level
    // loaded, the 3D project loads its start scene, then we normalize back to
    // the canonical sandbox (editor_scene.json) so the playtest smoke at
    // frame 60 sees the deterministic 3D scene regardless of the saved config.
    if (smokeMode_ && TimeRef().frameIndex == 43) {
        ScanProjects();
        bool has2D = false, has3D = false;
        for (const EditorProject& p : projects_) {
            if (p.mode == "2d" && !p.scenes.empty()) has2D = true;
            if (p.mode == "3d" && !p.scenes.empty()) has3D = true;
        }
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] discovered 2D+3D projects (%zu)",
                      has2D && has3D ? "PASS" : "FAIL", projects_.size());
        if (!has2D || !has3D) smokeFailed_ = true;
        SwitchProject("projects/pvz");
        const bool pvzOk = editMode_ == EditMode::Scene2D && !entities_.empty() &&
                           pvzPlants_.size() > 0 && currentSceneName_ == "pvz.json";
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] 2D project switch -> 2D view (parsed %zu plants)",
                      pvzOk ? "PASS" : "FAIL", pvzPlants_.size());
        if (!pvzOk) smokeFailed_ = true;
        const bool assetOk = assetDir_ == "projects/pvz/assets";
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] asset panel follows the project assets dir",
                      assetOk ? "PASS" : "FAIL");
        if (!assetOk) smokeFailed_ = true;
        SwitchProject("projects/neon_realm");
        const bool realmOk = editMode_ == EditMode::Scene3D && !entities_.empty();
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] 3D project switch loaded its scene (%zu)",
                      realmOk ? "PASS" : "FAIL", entities_.size());
        if (!realmOk) smokeFailed_ = true;
        StopPlaytest();
        editMode_ = EditMode::Scene3D;
        LoadScene("editor_scene.json");
        // The sandbox scene is user data (SaveScene writes it), so only assert
        // the 3D scene tree is back and non-empty - not a fixed entity count.
        const bool backOk = editMode_ == EditMode::Scene3D && !entities_.empty();
        NEON_LOG_INFO("EDITOR-PROJECT-SMOKE: [%s] normalized to the 3D sandbox (%zu)",
                      backOk ? "PASS" : "FAIL", entities_.size());
        if (!backOk) smokeFailed_ = true;
    }

    // Material-ball sphere preview: queued at frame 30, the offscreen render
    // runs in a later frame's OnRender; verify the cached texture landed.
    if (smokeMode_ && TimeRef().frameIndex == 44) {
        const std::string path = GetTempDir() + "/asset_proj/materials/smoke_mat.mat.json";
        const auto it = materialThumbs_.find(path);
        const bool ok = it != materialThumbs_.end() &&
                        it->second.texId != ImTextureID_Invalid;
        NEON_LOG_INFO("EDITOR-MATERIAL-SMOKE: [%s] material ball sphere preview generated",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
        const std::string zhPath =
            GetTempDir() + "/asset_proj/materials/\u6d4b\u8bd5\u7403.mat.json";
        const auto itZh = materialThumbs_.find(zhPath);
        const bool zhOk = itZh != materialThumbs_.end() &&
                          itZh->second.texId != ImTextureID_Invalid;
        NEON_LOG_INFO("EDITOR-MATERIAL-SMOKE-CJK: [%s] CJK-named material ball preview",
                      zhOk ? "PASS" : "FAIL");
        if (!zhOk) smokeFailed_ = true;
    }
    // Play/Stop smoke: start a playtest at frame 60, verify it ticks, stop at
    // the last frame (119; OnUpdate never runs at 120). Kept at "Play/Stop
    // doesn't crash the editor" level; the real script/BT verification lives
    // in tests/test_game_runtime.cpp.
    if (smokeMode_ && TimeRef().frameIndex == 60) StartPlaytest();
    if (smokeMode_ && TimeRef().frameIndex == 90) {
        const bool ok = playtestActive_ && playtest_ && playtest_->Running();
        NEON_LOG_INFO("EDITOR-PLAYTEST-SMOKE: [%s] playtest active (entities=%zu)",
                      ok ? "PASS" : "FAIL", ok ? playtest_->EntityCount() : 0u);
        if (!ok) smokeFailed_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 119) { // last OnUpdate before exit
        const bool wasActive = playtestActive_ && playtest_;
        StopPlaytest();
        const bool clean = !playtest_ && !playtestActive_;
        NEON_LOG_INFO("EDITOR-PLAYTEST-SMOKE: [%s] playtest stopped cleanly (was %s)",
                      clean ? "PASS" : "FAIL", wasActive ? "active" : "inactive");
        if (!wasActive || !clean) smokeFailed_ = true;
    }
}

void EditorApp::OnRender() {
    renderer_.BeginFrame({0.06f, 0.08f, 0.13f, 1.0f});
    gfx::Camera cam = ActiveCamera(); // the scene branch re-reads after SetCamera
    if (showUIEditor_ && uiDocOpen_) {
        static bool uiEdLogged = false;
        if (!uiEdLogged) {
            uiEdLogged = true;
            NEON_LOG_INFO("UI-EDITOR-PREVIEW: active (doc='%s')", uiDocPath_.c_str());
        }
        // UI editor preview: render the edited document into the viewport dock
        // (1:1 design pixels), with the selected node's outline + resize
        // handles. No 3D/2D scene is drawn while the UI editor is active.
        const math::Rect2& vp = viewportScreenRect_;
        if (vp.w > 0.0f && vp.h > 0.0f && renderer_.Backend()) {
            renderer_.Backend()->SetScissor(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                            static_cast<int>(vp.w), static_cast<int>(vp.h), true);
            renderer_.Set2DViewportPixels(vp.x, vp.y);
            if (cjkFont_.Valid()) uiDoc_.Draw(renderer_, cjkFont_);
            if (uiSelected_) {
                const math::Rect2 sel = uiSelected_->AbsoluteRect();
                renderer_.DrawRectOutline(sel, 2.0f,
                                          {0.4f, 0.9f, 1.0f, 0.9f});
                const float hs = 8.0f;
                const math::Vec2 corners[4] = {
                    {sel.x - hs, sel.y - hs},
                    {sel.x + sel.w, sel.y - hs},
                    {sel.x - hs, sel.y + sel.h},
                    {sel.x + sel.w, sel.y + sel.h}};
                for (const math::Vec2& c : corners)
                    renderer_.DrawRect(c, {hs * 2, hs * 2}, {0.4f, 0.9f, 1.0f, 1.0f});
            }
            renderer_.Flush2D();
            renderer_.Backend()->SetScissor(0, 0, 0, 0, false);
            renderer_.Reset2DViewport();
        }
        renderer_.EndScene();
    } else if (playtestActive_ && playtest_ && projectMode_ == "2d") {
        // 2D game playtest: fit the 1280x720 design space into the viewport
        // dock so the runtime's on_render (the actual game) draws where the
        // player sees it. Entities render through a FIXED design-space camera
        // (1 world unit = 1 design pixel) so the whole view - sprites, UI and
        // the camera frame - zooms together via canvasZoom_; the editor's own
        // camera never leaks into the playtest.
        const math::Rect2& vp = viewportScreenRect_;
        gfx::Camera gameCam;
        gameCam.target = {640.0f, 360.0f, 0.0f};
        gameCam.position = gameCam.target + math::Vec3{0.0f, 0.0f, 14.0f};
        gameCam.up = {0.0f, 1.0f, 0.0f};
        gameCam.ortho = true;
        gameCam.orthoSize = vp.h * 0.5f / canvasZoom_;
        if (vp.w > 0.0f && vp.h > 0.0f && renderer_.Backend()) {
            renderer_.Set2DViewport(vp.x, vp.y, vp.w, vp.h, canvasZoom_, canvasPan_);
            renderer_.Backend()->SetScissor(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                            static_cast<int>(vp.w), static_cast<int>(vp.h), true);
            playtest_->Draw(renderer_, gameCam);
            // Mark the game's camera view (the full 1280x720 design area).
            renderer_.DrawRectOutline(
                {0.0f, 0.0f, static_cast<float>(gfx::Renderer::kDesignWidth),
                 static_cast<float>(gfx::Renderer::kDesignHeight)},
                2.0f, gfx::Color{0.4f, 0.9f, 1.0f, 0.65f});
            renderer_.Flush2D();
            renderer_.Backend()->SetScissor(0, 0, 0, 0, false);
        } else {
            // No viewport rect yet (first frame): full-window fallback.
            playtest_->Draw(renderer_, gameCam);
        }
        renderer_.Reset2DViewport();
    } else {
        // Render the 3D scene INTO the viewport dock (same idea as the 2D
        // canvas): the camera projection uses the viewport aspect, the
        // rasterization viewport is the dock rect, and the 2D overlay (sky,
        // billboards, playtest HUD) is clipped to it. Nothing bleeds into the
        // dock panels anymore.
        const math::Rect2& vp = viewportScreenRect_;
        const bool hasVp = vp.w > 0.0f && vp.h > 0.0f && renderer_.Backend();
        if (hasVp) {
            renderer_.Backend()->SetScissor(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                            static_cast<int>(vp.w), static_cast<int>(vp.h), true);
            // 3D HUD/billboards draw at design pixel size (1:1) inside the
            // viewport, matching the packed game; the scissor clips anything
            // outside the panel. (2D games keep the fit+center design-space
            // mapping in the playtest branch above.)
            renderer_.Set2DViewportPixels(vp.x, vp.y);
        }
        // Day sky: the old near-black zenith made the IBL ambient ~0, so
        // backfaces and shadowed areas were crushed to black (roofs looked
        // incomplete, shadows harsh). A bright sky keeps shadows readable.
        renderer_.SetSky({0.28f, 0.38f, 0.58f, 1.0f}, {0.55f, 0.65f, 0.8f, 1.0f});
        renderer_.SetFog({0.45f, 0.55f, 0.7f, 1.0f}, 60.0f, 140.0f);
        renderer_.DrawSky();

        const float aspect = ViewportAspect();
        cam = ActiveCamera();
        renderer_.SetCamera(cam, aspect);
        if (hasVp) {
            // SetCamera may run the shadow pass (which rebinds targets and
            // resets the backend viewport), so apply the scene rect after it.
            renderer_.SetSceneViewport(vp.x, vp.y, vp.w, vp.h);
        }
        renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.45f);
        // 2D view = camera lock: mark the ortho camera's visible rect so the
        // user sees exactly what the front/top camera frames (Unity-style
        // camera border), no matter which project the view belongs to.
        if (viewCam_ == ViewCam::Front) DrawCameraFrame();

        if (playtestActive_ && playtest_) {
            // Play mode: the viewport renders the runtime's world (a snapshot
            // of the scene taken at Play). The editor scene is untouched.
            playtest_->Draw(renderer_, cam);
            // Physics debug view: wireframe every collider so the collision
            // shapes are visible while playtesting (dynamic = cyan, static =
            // gray). Uses the physics world's live positions, so resting and
            // bouncing bodies show exactly where the simulation puts them.
            for (const physics::World::DebugBody& db :
                 playtest_->PhysicsWorld().DebugBodies()) {
                const gfx::Color c = db.dynamic ? gfx::Color{0.2f, 0.9f, 1.0f, 0.9f}
                                                : gfx::Color{0.55f, 0.62f, 0.7f, 0.85f};
                if (db.kind == physics::World::ShapeKind::Sphere)
                    renderer_.DrawSphere(db.pos, db.radius, c);
                else
                    renderer_.DrawBox({db.pos - db.halfExtents, db.pos + db.halfExtents}, c);
            }
        } else {
            for (const SceneEntity& e : entities_) {
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (!e.spriteTex.empty() && e.spriteMesh.Valid()) {
                    // 2D sprite: image quad; flips mirror it around its center
                    // via a negative local scale (keeps the texture upright).
                    if (e.spriteFlipX || e.spriteFlipY)
                        model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                           e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                    renderer_.DrawMesh(e.spriteMesh, e.spriteMaterial, model);
                } else {
                    renderer_.DrawMesh(e.mesh, e.material, model);
                }
            }
            static bool dbg = false;
            if (!dbg && smokeMode_) {
                dbg = true;
                NEON_LOG_INFO("EDITOR-DRAW: entities=%zu drawCalls=%u", entities_.size(),
                              renderer_.Stats().drawCalls);
            }
            if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
                const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (e.spriteFlipX || e.spriteFlipY)
                    model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                       e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                const gfx::Mesh& pickMesh = e.spriteMesh.Valid() ? e.spriteMesh : e.mesh;
                math::AABB world = math::TransformAABB(pickMesh.Bounds(), model);
                renderer_.DrawBox(world, gfx::Color{0.3f, 0.8f, 1.0f, 1.0f});
            }
        }

        // Rasterize the scene's 2D overlay (sky / billboards / playtest HUD)
        // while the scissor is active so it stays inside the viewport, then
        // restore the full-window mapping for the composite + tool UI.
        if (hasVp) {
            renderer_.ResetSceneViewport();
            renderer_.Flush2D();
            renderer_.Backend()->SetScissor(0, 0, 0, 0, false);
            renderer_.Reset2DViewport();
        }
    }
    // End the 3D scene phase: composite the HDR frame to the backbuffer and
    // bind the backbuffer so the tool UI (engine UI demo + ImGui) below renders
    // crisp and unbloomed on top.
    renderer_.EndScene();

    // The playtest's data-driven UI (UIShow menus/HUD) draws on top of the
    // composited frame so it keeps the authored colors (the 2D canvas / scene
    // content stays in the HDR target). Clip it to the viewport like the scene
    // so it never spills over the dock panels, matching the packed game.
    if (playtestActive_ && playtest_) {
        const math::Rect2& vp = viewportScreenRect_;
        if (vp.w > 0.0f && vp.h > 0.0f && renderer_.Backend()) {
            if (projectMode_ == "2d")
                renderer_.Set2DViewport(vp.x, vp.y, vp.w, vp.h, canvasZoom_, canvasPan_);
            else
                renderer_.Set2DViewportPixels(vp.x, vp.y);
            renderer_.Backend()->SetScissor(static_cast<int>(vp.x), static_cast<int>(vp.y),
                                            static_cast<int>(vp.w), static_cast<int>(vp.h), true);
            playtest_->DrawUI(renderer_);
            renderer_.Flush2D();
            renderer_.Backend()->SetScissor(0, 0, 0, 0, false);
            renderer_.Reset2DViewport();
        } else {
            playtest_->DrawUI(renderer_);
            renderer_.Flush2D();
        }
    }

    // Game HUD (HP/mana/skill hotbar) overlays the playtest scene. A
    // data-driven game that defines on_render draws its OWN HUD on the 2D
    // canvas, so the built-in HUD is only a fallback for legacy scenes.
    if (playtestActive_ && playtest_ && !playtest_->HasScriptFunction("on_render"))
        DrawPlaytestHUD();

    // Scene pass draw calls (before the thumbnail pass adds its own counts).
    if (smokeMode_) {
        lastRenderTick_ = TimeRef().frameIndex;
        smokeDrawCalls_ = renderer_.Stats().drawCalls;
        lastRenderCamOrtho_ = cam.ortho;
    }

    // Asset thumbnails: meshes selected in the asset panel render into small
    // offscreen targets here, after the scene is composited, so the ImGui pass
    // below can sample them on the next frame. Flush any pending 2D first: on a
    // non-HDR driver EndScene is a no-op and the sky's quads must reach the
    // backbuffer, not the thumbnail target.
    renderer_.Flush2D();
    GenerateMeshThumbnails();
    GenerateMaterialThumbnails();

    renderer_.Flush2D();
    gfx::ImGuiNeon_RenderDrawData(ImGui::GetDrawData());

    if (!screenshotPath_.empty() && TimeRef().frameIndex >= screenshotFrame_) {
        std::vector<uint8_t> pixels;
        if (renderer_.CaptureFrame(pixels)) {
            stbi_write_png(screenshotPath_.c_str(), renderer_.ScreenWidth(),
                           renderer_.ScreenHeight(), 4, pixels.data(),
                           renderer_.ScreenWidth() * 4);
            NEON_LOG_INFO("Editor screenshot: %s", screenshotPath_.c_str());
        }
        screenshotPath_.clear();
    }
    renderer_.EndFrame();
}

void EditorApp::OnEvent(const platform::InputEvent& event) {
    // Input-map panel: while listening for a rebind, the next raw key wins
    // (checked before the F5/gizmo shortcuts so rebinding works while playing).
    if (!inputMapListenAction_.empty() &&
        event.type == platform::InputEvent::Type::KeyDown &&
        event.key != platform::Key::Unknown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (inputMapEdit_.SetPrimaryKey(inputMapListenAction_, event.key))
            NEON_LOG_INFO("Editor: input action '%s' -> %s", inputMapListenAction_.c_str(),
                          script::InputMap::KeyToName(event.key).c_str());
        inputMapListenAction_ = "";
        return;
    }
    // Ctrl+Z (undo) / Ctrl+Y or Ctrl+Shift+Z (redo) on the KeyDown edge only,
    // and never while ImGui owns the keyboard (e.g. typing in the name field)
    // -- same gating as the F5 playtest shortcut below. When the 行为树 panel
    // has focus AND its graph history has steps, undo/redo drive the BT graph;
    // otherwise they drive the scene history (an empty BT history never
    // swallows the scene shortcuts).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (Input()->IsDown(platform::Key::Control)) {
            if (event.key == platform::Key::Z) {
                if (Input()->IsDown(platform::Key::Shift)) {
                    if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                    else history_.Redo();
                } else {
                    if (btPanelFocused_ && btHistory_.CanUndo()) btHistory_.Undo();
                    else history_.Undo();
                }
                ClampSelection();
                return;
            }
            if (event.key == platform::Key::Y) {
                if (btPanelFocused_ && btHistory_.CanRedo()) btHistory_.Redo();
                else history_.Redo();
                ClampSelection();
                return;
            }
        }
    }
    // F5 toggles playtest on the KeyDown edge only (Win32 auto-repeats KeyDown
    // while held, which would otherwise oscillate Play/Stop), and never while
    // ImGui owns the keyboard (e.g. typing in a text field).
    if (event.key == platform::Key::F5) {
        if (event.type == platform::InputEvent::Type::KeyDown) {
            if (!f5Pressed_ && !gfx::ImGuiNeon_WantCaptureKeyboard()) {
                TogglePlaytest();
            }
            f5Pressed_ = true;
        } else if (event.type == platform::InputEvent::Type::KeyUp) {
            f5Pressed_ = false;
        }
    }
    // Delete removes the selected asset (armed here, opened inside the panel
    // on the next frame because ImGui popups need an active frame).
    if (event.type == platform::InputEvent::Type::KeyDown &&
        event.key == platform::Key::Delete && selectedAsset_ >= 0 &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        NEON_LOG_INFO("Asset: Delete key pressed (selected=%d)", selectedAsset_);
        deleteAssetRequested_ = true;
    }
    // Tab cycles the viewport camera preset (透视 -> 顶视 -> 前视 -> ...), the
    // same list the toolbar combo exposes.
    if (event.type == platform::InputEvent::Type::KeyDown && event.key == platform::Key::Tab &&
        !gfx::ImGuiNeon_WantCaptureKeyboard()) {
        SetViewCam(static_cast<ViewCam>((static_cast<int>(viewCam_) + 1) % 3));
    }
    if (event.type == platform::InputEvent::Type::TextInput) {
        pendingText_ += event.text;
    }
}

void EditorApp::UpdateUIEditorViewport() {
    const math::Rect2& vp = viewportScreenRect_;
    if (vp.w <= 0.0f || vp.h <= 0.0f) return;
    platform::IInput* input = Input();
    if (!input) return;
    renderer_.Set2DViewportPixels(vp.x, vp.y);
    const math::Vec2 mouse = renderer_.ScreenToUI(input->MousePos());

    auto cornerAt = [](const math::Rect2& r, const math::Vec2& p) {
        const float k = 10.0f;
        const math::Vec2 corners[4] = {
            {r.x, r.y}, {r.x + r.w, r.y}, {r.x, r.y + r.h}, {r.x + r.w, r.y + r.h}};
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(corners[i].x - p.x) <= k && std::fabs(corners[i].y - p.y) <= k)
                return i;
        }
        return -1;
    };

    if (input->MousePressed(platform::MouseButton::Left)) {
        uiDragging_ = false;
        uiResizeHandle_ = -1;
        const math::Rect2 selRect =
            uiSelected_ ? uiSelected_->AbsoluteRect() : math::Rect2{};
        if (uiSelected_ && cornerAt(selRect, mouse) >= 0) {
            uiResizeHandle_ = cornerAt(selRect, mouse);
            uiDragging_ = true;
            return;
        }
        ui::UiNode* hit = uiDoc_.HitTest(mouse);
        uiSelected_ = (hit && hit != &uiDoc_.root) ? hit : nullptr;
        uiDragging_ = uiSelected_ != nullptr;
        return;
    }

    if (input->MouseDown(platform::MouseButton::Left) && uiDragging_ && uiSelected_) {
        const math::Vec2 delta = input->MouseDelta() / renderer_.UIScale();
        math::Rect2& r = uiSelected_->rect;
        if (uiResizeHandle_ >= 0) {
            switch (uiResizeHandle_) {
                case 0: r.x += delta.x; r.y += delta.y; r.w -= delta.x; r.h -= delta.y; break;
                case 1: r.w += delta.x; r.y += delta.y; r.h -= delta.y; break;
                case 2: r.x += delta.x; r.w -= delta.x; r.h += delta.y; break;
                default: r.w += delta.x; r.h += delta.y; break;
            }
            r.w = std::max(r.w, 8.0f);
            r.h = std::max(r.h, 8.0f);
        } else {
            r.x += delta.x;
            r.y += delta.y;
        }
        MarkUIDirty();
        return;
    }

    if (input->MouseReleased(platform::MouseButton::Left)) {
        uiDragging_ = false;
        uiResizeHandle_ = -1;
    }
}

void EditorApp::MarkUIDirty() {
    uiDirty_ = true;
    if (!uiDocOpen_ || uiDocPath_.empty()) return;
    // "untitled" documents have no real path yet; the explicit 保存 button
    // assigns one. Everything else auto-saves on every edit so closing the
    // panel or restarting the editor never loses changes.
    const bool isUntitled =
        uiDocPath_.size() >= 15 &&
        uiDocPath_.compare(uiDocPath_.size() - 15, 15, "untitled.ui.json") == 0;
    if (isUntitled) return;
    if (uiDoc_.Save(uiDocPath_)) uiDirty_ = false;
}

gfx::Camera EditorApp::ActiveCamera() const {
    gfx::Camera cam;
    switch (viewCam_) {
        case ViewCam::Top: // 顶视: orthographic looking down -Y
            cam.position = camTarget_ + math::Vec3{0, camDist_, 0};
            cam.target = camTarget_;
            cam.up = {0, 0, -1};
            cam.ortho = true;
            cam.orthoSize = orthoSize_;
            break;
        case ViewCam::Front: // 前视: orthographic looking down -Z
            cam.position = camTarget_ + math::Vec3{0, 0, camDist_};
            cam.target = camTarget_;
            cam.up = {0, 1, 0};
            cam.ortho = true;
            cam.orthoSize = orthoSize_;
            break;
        case ViewCam::Perspective:
        default:
            cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                                   std::sin(pitch_),
                                                   std::cos(yaw_) * std::cos(pitch_)} *
                                            camDist_;
            cam.target = camTarget_;
            break;
    }
    return cam;
}

void EditorApp::DrawCameraFrame() {
    // The border marks the ACTUAL runtime view. In 2D projects that is the
    // fixed 1280x720 design space (the playtest shows exactly this), so the
    // frame is that rectangle on the content plane - zooming scales the frame
    // together with the sprites (whole-view zoom). In 3D front view there is
    // no design space; draw the ortho camera's visible rect instead so the
    // user can tell what the locked camera frames.
    const float z = 0.0f; // sprite content plane
    if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) {
        const float w = static_cast<float>(gfx::Renderer::kDesignWidth);
        const float h = static_cast<float>(gfx::Renderer::kDesignHeight);
        const gfx::Renderer::LineVertex verts[8] = {
            {{0.0f, 0.0f, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{w, 0.0f, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{w, 0.0f, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{w, h, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{w, h, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{0.0f, h, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{0.0f, h, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
            {{0.0f, 0.0f, z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        };
        renderer_.DrawLines(verts, 8, math::Mat4::Identity());
        return;
    }
    const float halfH = orthoSize_;
    const float halfW = halfH * ViewportAspect();
    const math::Vec3 c = camTarget_;
    const gfx::Renderer::LineVertex verts[8] = {
        {{c.x - halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x + halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y + halfH, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
        {{c.x - halfW, c.y, c.z}, {0.4f, 0.9f, 1.0f, 0.9f}},
    };
    renderer_.DrawLines(verts, 8, math::Mat4::Identity());
}

void EditorApp::UpdateViewport(float dt) {
    platform::IInput* input = Input();
    math::Vec2 mp = renderer_.ScreenToUI(input->MousePos());
    // ImGui tool windows capture mouse when hovered/active; the 3D viewport
    // area itself has no ImGui window, so camera controls stay responsive.
    // The DockSpace host spans the workspace and this code runs before
    // ImGui::NewFrame, so HoveredWindow/WantCaptureMouse are stale here.
    // Instead: a click belongs to a panel (and never to the viewport picker)
    // when the mouse is inside ANY visible docked leaf window except the
    // viewport itself - position-based, independent of hover bookkeeping.
    ImGuiContext& ictx = *ImGui::GetCurrentContext();
    const math::Vec2 mousePx = input->MousePos();
    bool overPanel = false;
    for (int wi = 0; wi < ictx.Windows.Size; ++wi) {
        ImGuiWindow* w = ictx.Windows[wi];
        if (!w || w->Hidden) continue;
        if (w->DockNodeAsHost != nullptr) continue; // dock host / tab bar
        if (w->ParentWindow != nullptr) continue;   // child windows
        if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue; // overlays (gizmo)
        if (std::strcmp(w->Name, "视口") == 0) continue; // the 3D viewport
        if (std::strncmp(w->Name, "##", 2) == 0) continue; // internal windows
        if (mousePx.x >= w->Pos.x && mousePx.x <= w->Pos.x + w->Size.x &&
            mousePx.y >= w->Pos.y && mousePx.y <= w->Pos.y + w->Size.y) {
            overPanel = true;
            break;
        }
    }
    bool inViewport = mp.x >= viewportRect_.x && mp.x <= viewportRect_.x + viewportRect_.w &&
                      mp.y >= viewportRect_.y && mp.y <= viewportRect_.y + viewportRect_.h;

    // 2D view: until the user zooms/pans, keep the camera framed so the
    // 1280x720 design space maps 1:1 onto the viewport (ortho half-height =
    // half the viewport height). This keeps edit view and playtest identical.
    if (editMode_ == EditMode::Scene2D && viewCam_ == ViewCam::Front &&
        !cameraUserAdjusted_) {
        const float vpH = viewportScreenRect_.h;
        if (vpH > 0.0f) orthoSize_ = vpH * 0.5f;
    }

    // 2D playtest: the whole view (entities + UI + camera frame) zooms as one
    // via canvasZoom_ - never scale sprites without the rest of the view.
    if (playtestActive_ && editMode_ == EditMode::Scene2D) {
        const float wheel = input->WheelDelta();
        if (std::fabs(wheel) > 0.01f) {
            canvasZoom_ = math::Clamp(canvasZoom_ * std::pow(1.15f, -wheel), 0.2f, 8.0f);
            input->ConsumeWheel();
        }
        return; // fixed whole-view framing; no per-entity camera navigation
    }

    const bool ortho = viewCam_ != ViewCam::Perspective;
    if (!overPanel && inViewport) {
        // While the transform gizmo is hovered or being dragged the mouse
        // belongs to it: camera orbit/pan and left-click picking must not run.
        // UpdateViewport runs before the gizmo's Manipulate() each frame, so
        // gizmoDragActive_/IsOver() report the previous frame's gizmo state.
        const bool gizmoBusy =
            selected_ >= 0 && (gizmoDragActive_ || ImGuizmo::IsOver());
        if (!gizmoBusy) {
            if (!ortho && input->MouseDown(platform::MouseButton::Right)) {
                yaw_ += -input->MouseDelta().x * 0.005f;
                pitch_ = math::Clamp(pitch_ + input->MouseDelta().y * 0.005f, 0.05f, 1.4f);
            }
            if (input->MouseDown(platform::MouseButton::Middle)) {
                // Pan in the ACTIVE camera's plane (the perspective orbit or a
                // static ortho view): middle-drag moves the target along the
                // camera's right/up axes.
                gfx::Camera cam = ActiveCamera();
                math::Vec3 fwd = (cam.target - cam.position).Normalized();
                math::Vec3 right = math::Cross(fwd, cam.up).Normalized();
                math::Vec3 upv = math::Cross(right, fwd);
                const float worldPerPixel =
                    ortho ? orthoSize_ * 2.0f /
                                (viewportScreenRect_.h > 0.0f
                                     ? viewportScreenRect_.h
                                     : static_cast<float>(renderer_.ScreenHeight()))
                          : 1.0f;
                const float k = ortho ? worldPerPixel : 0.02f;
                camTarget_ -= right * input->MouseDelta().x * k;
                camTarget_ += upv * input->MouseDelta().y * k;
                cameraUserAdjusted_ = true;
            }
            float wheel = input->WheelDelta();
            if (std::fabs(wheel) > 0.01f) {
                if (ortho) {
                    // Proportional zoom: each wheel notch scales the view by
                    // 1.15x. (Absolute deltas were useless at the 2D design
                    // space's ~360-unit starting size.) Range covers far out
                    // (8192) to 360x zoom-in (1) from any starting size.
                    // Wheel up (positive) zooms IN: scale the half-height down.
                    const float factor = std::pow(1.15f, -wheel);
                    orthoSize_ = math::Clamp(orthoSize_ * factor, 1.0f, 8192.0f);
                    cameraUserAdjusted_ = true;
                } else {
                    camDist_ = math::Clamp(camDist_ - wheel * 1.2f, 3.0f, 60.0f);
                }
            }
        }
        // Play mode keeps camera navigation but not scene editing: left-click
        // picking would mutate the editor scene selection mid-playtest.
        if (input->MousePressed(platform::MouseButton::Left) && !playtestActive_ &&
            !gizmoBusy) {
            const float aspect = ViewportAspect();
            gfx::Camera cam = ActiveCamera();
            // Build the ray from the mouse position RELATIVE to the viewport
            // dock, matching the viewport-aspect projection the scene uses.
            const math::Vec2 mousePx = input->MousePos();
            const math::Rect2& vp = viewportScreenRect_;
            const float vpW = vp.w > 0.0f ? vp.w : static_cast<float>(renderer_.ScreenWidth());
            const float vpH = vp.h > 0.0f ? vp.h : static_cast<float>(renderer_.ScreenHeight());
            const float vpX = vp.w > 0.0f ? vp.x : 0.0f;
            const float vpY = vp.h > 0.0f ? vp.y : 0.0f;
            const float ndcX = (mousePx.x - vpX) / vpW * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mousePx.y - vpY) / vpH * 2.0f;
            math::Ray ray = RayFromNDC(cam, aspect, ndcX, ndcY);
            float best = 1e30f;
            int picked = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& e = entities_[i];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (e.spriteFlipX || e.spriteFlipY)
                    model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                       e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                const gfx::Mesh& pickMesh = e.spriteMesh.Valid() ? e.spriteMesh : e.mesh;
                math::AABB world = math::TransformAABB(pickMesh.Bounds(), model);
                float t = 0.0f;
                if (math::IntersectRayAABB(ray, world, t) && t < best) {
                    best = t;
                    picked = static_cast<int>(i);
                }
            }
            SetSelection(picked);
        }
    }
    // Data-driven playtest scripts use the orbit yaw for camera-relative
    // movement (same GameVar the neon_game player publishes).
    if (playtestActive_ && playtest_)
        playtest_->GameVars().Set("cameraYaw", script::Value::Num(yaw_));
    (void)dt;
}

void EditorApp::SetSelection(int index) {
    selected_ = index;
    scriptSyncEntity_ = -1; // script panel caches by index: force a re-sync
}

void EditorApp::DrawPlaytestHUD() {
    if (!playtest_ || !playtestActive_) return;
    scene::GameRuntime& rt = *playtest_;
    const ecs::Entity hero = rt.FindNamedEntity("英雄");
    const auto heroHp = rt.EntityHealth(hero);
    const float hp = heroHp.first, maxHp = heroHp.second;
    const float mana = rt.GameVar("hero_mana");
    const float maxMana = rt.GameVar("hero_max_mana");
    const float fireCd = rt.GameVar("hero_fire_cd");
    const float healCd = rt.GameVar("hero_heal_cd");
    const float meleeCd = rt.GameVar("hero_melee_cd");
    const int level = static_cast<int>(rt.GameVar("hero_level"));
    const int gold = static_cast<int>(rt.GameVar("hero_gold"));

    ui::Theme theme;
    theme.font = cjkFont_.Valid() ? cjkFont_ : pixelFont_;

    const int w = renderer_.ScreenWidth();
    const int h = renderer_.ScreenHeight();

    // HP bar (top-left).
    ui::DrawLabel(renderer_, theme, "生命", {24, 20}, 14, theme.text, false, true);
    const float hpFrac = maxHp > 0.0f ? math::Saturate(hp / maxHp) : 0.0f;
    const gfx::Color hpColor = hpFrac > 0.5f ? gfx::Color{0.2f, 1.0f, 0.35f, 1.0f}
                               : hpFrac > 0.25f ? gfx::Color{1.0f, 0.85f, 0.2f, 1.0f}
                                                : gfx::Color{1.0f, 0.2f, 0.2f, 1.0f};
    ui::DrawBar(renderer_, theme, {70, 14, 280, 22}, hpFrac, hpColor);

    // Mana bar.
    ui::DrawLabel(renderer_, theme, "法力", {24, 48}, 14, theme.text, false, true);
    const float manaFrac = maxMana > 0.0f ? math::Saturate(mana / maxMana) : 0.0f;
    ui::DrawBar(renderer_, theme, {70, 42, 200, 14}, manaFrac, gfx::Color{0.25f, 0.45f, 1.0f, 1.0f});

    char buf[96];
    std::snprintf(buf, sizeof(buf), "等级 %d", level);
    ui::DrawLabel(renderer_, theme, buf, {24, 64}, 13, theme.text, false, false);
    std::snprintf(buf, sizeof(buf), "金币 %d", gold);
    const math::Vec2 gs = ui::MeasureText(theme.font, buf, 16);
    ui::DrawLabel(renderer_, theme, buf, {static_cast<float>(w) - gs.x - 8, 18}, 16,
                  gfx::Color{1.0f, 0.85f, 0.3f, 1.0f}, false, false);

    // Skill hotbar (bottom-left): melee / fireball / heal with cooldowns.
    float sx = 24.0f, sy = static_cast<float>(h) - 66.0f;
    const float slotW = 54.0f, slotH = 54.0f, gap = 8.0f;
    auto slot = [&](const char* name, const char* key, float cd, const gfx::Color& color) {
        const math::Rect2 r{sx, sy, slotW, slotH};
        ui::DrawPanel(renderer_, theme, r);
        ui::DrawLabel(renderer_, theme, key, {sx + 4, sy + 2}, 12, theme.dim, false, false);
        ui::DrawLabel(renderer_, theme, name, {sx + slotW * 0.5f, sy + slotH * 0.5f}, 15, color,
                      true, true);
        if (cd > 0.0f) {
            ui::DrawBar(renderer_, theme, r, 1.0f, theme.panelBg.WithAlpha(0.65f));
            std::snprintf(buf, sizeof(buf), "%.1f", cd);
            ui::DrawLabel(renderer_, theme, buf, {sx + slotW * 0.5f, sy + slotH * 0.5f}, 15,
                          theme.text, true, true);
        }
        sx += slotW + gap;
    };
    slot("近战", "左键", meleeCd, gfx::Color{0.92f, 0.92f, 1.0f, 1.0f});
    slot("火球", "1", fireCd, gfx::Color{1.0f, 0.55f, 0.20f, 1.0f});
    slot("治疗", "2", healCd, gfx::Color{0.4f, 1.0f, 0.5f, 1.0f});
}

void EditorApp::DrawTransformGizmo() {
    // ImGuizmo::BeginFrame() must run every frame before Manipulate(): it
    // resets mbOverGizmoHotspot (ImGuizmo.cpp:1084) so a handle can re-arm for
    // hover/activation each frame, and snapshots the last frame's hover for
    // IsOver(). Without it the activation check `CanActivate() && type !=
    // MT_NONE` can never fire again after the first hover.
    ImGuizmo::BeginFrame();
    gizmoBeginFrame_ = true;

    if (playtestActive_ || selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
        gizmoDragActive_ = false;
        return;
    }
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];

    // Draw the gizmo into the viewport window's draw list. The viewport is an
    // ordinary input-active docked panel (NoInputs was removed so it can be
    // undocked/re-docked), so ImGui's hover hit-test resolves to the viewport
    // window itself; point ImGuizmo's hover check at it via
    // SetAlternativeWindow.
    ImGuiWindow* viewportWindow = ImGui::GetCurrentWindow();
    ImGuizmo::SetAlternativeWindow(viewportWindow);
    gizmoAltWindowSet_ = viewportWindow != nullptr;

    const float aspect = ViewportAspect();
    gfx::Camera cam = ActiveCamera();
    ImGuizmo::SetOrthographic(cam.ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

    // The 3D scene now renders INTO the viewport dock with the viewport aspect
    // and rasterization rect, so the gizmo uses the same rect + aspect.
    const math::Rect2& vp = viewportScreenRect_;
    const float rx = vp.w > 0.0f ? vp.x : 0.0f;
    const float ry = vp.h > 0.0f ? vp.y : 0.0f;
    const float rw = vp.w > 0.0f ? vp.w : static_cast<float>(renderer_.ScreenWidth());
    const float rh = vp.h > 0.0f ? vp.h : static_cast<float>(renderer_.ScreenHeight());
    ImGuizmo::SetRect(rx, ry, rw, rh);
    gizmoRect_[0] = rx;
    gizmoRect_[1] = ry;
    gizmoRect_[2] = rw;
    gizmoRect_[3] = rh;

    float view[16], proj[16];
    Mat4ToGizmo(cam.View(), view);
    Mat4ToGizmo(cam.Projection(aspect), proj);

    math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                       math::Mat4::Scale(e.scale);
    float gizmoModel[16];
    Mat4ToGizmo(model, gizmoModel);

    // Smoke instrumentation: the gizmo must emit geometry into the viewport's
    // draw list (not just run without crashing).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int cmdsBefore = dl->CmdBuffer.Size;
    const int vtxBefore = dl->VtxBuffer.Size;

    // Manipulate reads ImGui's mouse state directly and mutates gizmoModel on
    // drag; when it returns true the transform changed. The write-back is
    // routed through the history command stack: each changed frame pushes an
    // EditTransformCommand whose ORIGINAL is the last applied value (a
    // continuous merge chain), so frames within one drag collapse into a
    // single undo step that reverts to the pre-drag transform.
    if (ImGuizmo::Manipulate(view, proj, gizmoOp_, gizmoMode_, gizmoModel)) {
        math::Mat4 m;
        GizmoToMat4(gizmoModel, m);
        math::Vec3 pos, scale;
        math::Quat rot;
        DecomposeModel(m, pos, scale, rot);
        if (!(Vec3Eq(pos, e.pos) && Vec3Eq(scale, e.scale) && QuatEq(rot, e.rot))) {
            gizmoDragOriginValid_ = true;
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, e.rot, e.scale, pos, rot, scale,
                EditTransformCommand::kAll));
        }
    }
    gizmoDragActive_ = ImGuizmo::IsUsing();
    if (!gizmoDragActive_) {
        // The drag just ended: seal the command it produced so a FUTURE drag of
        // the same entity starts its own undo step (one drag = one undo step).
        if (gizmoDragOriginValid_) {
            if (EditTransformCommand* top =
                    dynamic_cast<EditTransformCommand*>(history_.TopUndo())) {
                if (top->Matches(selected_, EditTransformCommand::kAll)) top->Seal();
            }
        }
        gizmoDragOriginValid_ = false;
    }

    if (smokeMode_ && !gizmoDrawn_) {
        gizmoDrawn_ = true;
        const bool drewGeometry = dl->CmdBuffer.Size > cmdsBefore &&
                                  dl->VtxBuffer.Size > vtxBefore;
        NEON_LOG_INFO("EDITOR-GIZMO-SMOKE: [%s] gizmo drawn (op=%d mode=%d entity='%s' cmds+%d vtx+%d)",
                      drewGeometry ? "PASS" : "FAIL", static_cast<int>(gizmoOp_),
                      static_cast<int>(gizmoMode_), e.name.c_str(),
                      dl->CmdBuffer.Size - cmdsBefore, dl->VtxBuffer.Size - vtxBefore);
        if (!drewGeometry) smokeFailed_ = true;
    }

    // On the smoke frame, synthesize the full ImGuizmo input path (hover the
    // dock host, press, drag, release) to verify the gizmo is actually
    // grabbable. Runs here, inside the viewport window scope, because
    // ImGuizmo::Manipulate needs a current window to draw into.
    if (smokeMode_ && TimeRef().frameIndex == 30 && !gizmoDragSimulated_) {
        gizmoDragSimulated_ = true;
        RunGizmoDragSim();
    }
}

void EditorApp::RunGizmoDragSim() {
    auto report = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-GIZMO-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
        report(false, "drag sim needs a selected entity");
        return;
    }
    SceneEntity& sel = entities_[static_cast<size_t>(selected_)];
    math::Mat4 modelBefore = math::Mat4::Translation(sel.pos) * sel.rot.ToMat4() *
                             math::Mat4::Scale(sel.scale);

    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    ImGuiIO& io = ctx.IO;

    // Preserve the real frame's input state; restored at the end.
    const bool savedDown = io.MouseDown[0];
    const float savedDur = io.MouseDownDuration[0];
    const ImVec2 savedPos = io.MousePos;
    const ImGuiID savedActive = ctx.ActiveId;
    const ImGuiID savedHovered = ctx.HoveredId;
    const ImGuiID savedHoveredPrev = ctx.HoveredIdPreviousFrame;
    ImGuiWindow* savedHoveredWin = ctx.HoveredWindow;

    const float aspect = ViewportAspect();
    gfx::Camera cam = ActiveCamera();
    float view[16], proj[16];
    Mat4ToGizmo(cam.View(), view);
    Mat4ToGizmo(cam.Projection(aspect), proj);

    // Screen position of the entity origin under the viewport rect (the same
    // rect the gizmo now uses), in y-down ImGui pixels.
    math::Mat4 vp = cam.ViewProjection(aspect);
    math::Vec4 clip = vp.TransformVec4({sel.pos.x, sel.pos.y, sel.pos.z, 1.0f});
    const math::Rect2& vr = viewportScreenRect_;
    const float vrW = vr.w > 0.0f ? vr.w : static_cast<float>(renderer_.ScreenWidth());
    const float vrH = vr.h > 0.0f ? vr.h : static_cast<float>(renderer_.ScreenHeight());
    const float gx = vr.x + (clip.x / clip.w * 0.5f + 0.5f) * vrW;
    const float gy = vr.y + (0.5f - clip.y / clip.w * 0.5f) * vrH;

    // The viewport is an input-active docked panel: ImGui reports IT as the
    // hovered window over the viewport, and SetAlternativeWindow points the
    // gizmo at it too.
    ImGuiWindow* vpWin = ImGui::FindWindowByName("视口");
    ImGuiWindow* hostWin = vpWin;
    report(hostWin != nullptr, "drag sim resolves the viewport window");
    if (!hostWin) return;

    // The real hover path relies on ImGui reporting the viewport window as
    // hovered when the mouse is over it (OnUpdate parked the mouse on the
    // viewport center for this smoke frame). If it doesn't match the window
    // the gizmo is bound to, SetAlternativeWindow is wrong/removed and the
    // gizmo would be undraggable - fail the smoke here.
    report(ctx.HoveredWindow == hostWin,
           "real hover over the viewport resolves to the viewport window");

    // Clear hover/active so CanActivate() sees no other ImGui item.
    ctx.HoveredWindow = hostWin;
    ctx.ActiveId = 0;
    ctx.HoveredId = 0;
    ctx.HoveredIdPreviousFrame = 0;

    float gizmoModel[16];
    Mat4ToGizmo(modelBefore, gizmoModel);

    // Press over the entity origin -> the screen-space translate handle.
    io.MousePos = ImVec2(gx, gy);
    io.MouseDown[0] = true;
    io.MouseDownDuration[0] = 0.0f; // pressed this frame: IsMouseClicked fires
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(cam.ortho);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(vr.w > 0.0f ? vr.x : 0.0f, vr.h > 0.0f ? vr.y : 0.0f,
                      vr.w > 0.0f ? vr.w : static_cast<float>(renderer_.ScreenWidth()),
                      vr.h > 0.0f ? vr.h : static_cast<float>(renderer_.ScreenHeight()));
    // NOTE: deliberately NOT re-arming SetAlternativeWindow here. The real
    // DrawTransformGizmo set it this frame; the activation below only succeeds
    // if that setting matches ctx.HoveredWindow (the dock host), so a removed
    // or retargeted SetAlternativeWindow is caught by this smoke.
    ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, gizmoModel);
    report(ImGuizmo::IsUsing(), "gizmo activates on click over a handle");

    // Drag: hold the button and move the mouse off the origin.
    io.MouseDownDuration[0] = 0.1f;
    io.MousePos = ImVec2(gx + 40.0f, gy);
    const bool dragged = ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE,
                                              ImGuizmo::WORLD, gizmoModel);
    report(ImGuizmo::IsUsing(), "gizmo stays active while dragging");
    math::Mat4 m;
    GizmoToMat4(gizmoModel, m);
    report(dragged && (m.m[3] != modelBefore.m[3] || m.m[7] != modelBefore.m[7] ||
                       m.m[11] != modelBefore.m[11]),
           "gizmo drag moves the model matrix");

    // Release.
    io.MouseDown[0] = false;
    ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, gizmoModel);
    report(!ImGuizmo::IsUsing(), "gizmo deactivates on release");

    // Restore the frame's input state so the rest of the frame sees it as-is.
    io.MouseDown[0] = savedDown;
    io.MouseDownDuration[0] = savedDur;
    io.MousePos = savedPos;
    ctx.ActiveId = savedActive;
    ctx.HoveredId = savedHovered;
    ctx.HoveredIdPreviousFrame = savedHoveredPrev;
    ctx.HoveredWindow = savedHoveredWin;
}

void EditorApp::BuildImGuiUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存场景", "Ctrl+S")) SaveScene();
            if (ImGui::MenuItem("加载场景", "Ctrl+L")) LoadScene("editor_scene.json");
            if (ImGui::MenuItem("另存为子场景")) SaveSceneAsChild();
            ImGui::Separator();
            if (ImGui::MenuItem("退出")) {
                if (Window()) Window()->RequestClose();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景", nullptr, &showHierarchy_);
            ImGui::MenuItem("属性", nullptr, &showInspector_);
            ImGui::MenuItem("资产", nullptr, &showAssets_);
            ImGui::MenuItem("资源", nullptr, &showResources_);
            ImGui::MenuItem("日志", nullptr, &showLog_);
            ImGui::MenuItem("行为树", nullptr, &showBt_);
            ImGui::MenuItem("脚本", nullptr, &showScripts_);
            ImGui::MenuItem("脚本编辑器", nullptr, &showScriptEditor_);
            ImGui::MenuItem("打包", nullptr, &showPackage_);
            ImGui::MenuItem("性能", nullptr, &showProfiler_);
            ImGui::MenuItem("输入映射", nullptr, &showInputMap_);
            ImGui::MenuItem("导航", nullptr, &showNav_);
            ImGui::MenuItem("UI 编辑器", nullptr, &showUIEditor_);
            ImGui::MenuItem("本地化", nullptr, &showLoc_);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showImGuiDemo_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("项目")) {
            if (projects_.empty()) ScanProjects();
            ImGui::TextDisabled("打开项目");
            for (size_t i = 0; i < projects_.size(); ++i) {
                const EditorProject& p = projects_[i];
                char label[256];
                std::snprintf(label, sizeof(label), "%s  [%s]###mproj%d", p.name.c_str(),
                              p.mode == "2d" ? "2D" : "3D", static_cast<int>(i));
                if (ImGui::MenuItem(label, nullptr, projectSel_ == static_cast<int>(i)))
                    SwitchProject(p.dir);
            }
            ImGui::Separator();
            ImGui::TextDisabled("当前项目场景");
            for (const std::string& s : projectScenes_) {
                if (ImGui::MenuItem(BaseName(s).c_str())) LoadProjectScene(s);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("项目目录");
            if (ImGui::InputText("##project_dir", projectDirBuf_, sizeof(projectDirBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                SwitchProject(projectDirBuf_);
            }
            ImGui::TextDisabled("导出场景写入 %s/scenes/exported_scene.json",
                                projectDir_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("重新加载项目")) SwitchProject(projectDir_);
            if (ImGui::MenuItem("导出游戏场景")) ExportScene();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::MenuItem("关于", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Transform-gizmo shortcuts: W/E/R switch the operation while an entity is
    // selected (ignored while the user is typing text, e.g. the name field).
    if (selected_ >= 0 && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = ImGuizmo::SCALE;
    }

    // Docking layout: full-workspace dock space below the menu bar.
    const float menuH = ImGui::GetFrameHeight();
    const float toolH = 36.0f;
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH + toolH), ImGuiCond_Always);
    // Use Size.y (the full window height), NOT WorkSize.y: BeginMainMenuBar
    // shrinks the main viewport's WorkSize by the menu bar height, so sizing
    // the DockSpace off WorkSize.y would end it ~menuH px above the window
    // bottom and let the full-screen 3D scene leak out below the panels.
    ImGui::SetNextWindowSize(
        ImVec2(mainVp->Size.x, mainVp->Size.y - menuH - toolH),
                             ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGuiWindowFlags dsFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##NeonDockSpace", nullptr, dsFlags);
    ImGui::PopStyleVar(3);
    ImGuiID dockId = ImGui::GetID("NeonDockSpace");
    dockspaceId_ = dockId;
    // NOTE: no ImGuiDockNodeFlags_PassthruCentralNode here. That flag makes the
    // DockSpace root paint an opaque ImGuiCol_WindowBg rectangle over the WHOLE
    // workspace when the central node is non-empty (and the 3D viewport window
    // IS docked into the central node, so the passthru "hole" is never
    // registered) - which would cover the full-screen 3D scene. Without the
    // flag the host window (NoBackground) + the 视口 window (NoBackground) stay
    // transparent, so the scene shows through the central viewport while the
    // opaque tool panels cover the rest.
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    // Persist a layout-version marker in the ini (offscreen, invisible). Its
    // absence means "no saved layout yet" or "saved before the layout changed",
    // which triggers the Unity-style default below exactly once.
    {
        char verName[32];
        std::snprintf(verName, sizeof(verName), "##NeonLayoutVer%d", kNeonLayoutVersion);
        ImGui::SetNextWindowPos(ImVec2(-100000.0f, -100000.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
        ImGui::Begin(verName, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav);
        ImGui::End();
    }

    // First-run default docking layout (applied when there is no saved layout,
    // the saved ini predates this layout version, or the dock space is empty).
    static bool layoutAttempted = false;
    if (!layoutAttempted) {
        layoutAttempted = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (node == nullptr || !node->IsSplitNode() || NeedsDefaultLayout()) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId,
                                          ImVec2(mainVp->WorkSize.x,
                                                 mainVp->WorkSize.y - menuH - toolH));
            // Unity-style layout: Hierarchy (场景) left, Inspector (属性) right,
            // Scene view (视口) center, Project/tools (资产/资源/日志/行为树/脚本/
            // 脚本编辑器/打包/性能) docked across the bottom.
            ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.22f,
                                                        nullptr, &dockId);
            ImGuiID left = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.20f,
                                                       nullptr, &dockId);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.28f,
                                                         nullptr, &dockId);
            ImGui::DockBuilderDockWindow("场景", left);
            ImGui::DockBuilderDockWindow("属性", right);
            ImGui::DockBuilderDockWindow("资产", bottom);
            ImGui::DockBuilderDockWindow("资源", bottom);
            ImGui::DockBuilderDockWindow("日志", bottom);
            ImGui::DockBuilderDockWindow("行为树", bottom);
            ImGui::DockBuilderDockWindow("脚本", bottom);
            ImGui::DockBuilderDockWindow("脚本编辑器", bottom);
            ImGui::DockBuilderDockWindow("打包", bottom);
            ImGui::DockBuilderDockWindow("性能", bottom);
            ImGui::DockBuilderDockWindow("视口", dockId);
            ImGui::DockBuilderFinish(dockId);
        }
    }

    // Toolbar row below the menu bar.
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 34.0f), ImGuiCond_Always);
    ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toolbar", nullptr, tbFlags)) {
        if (ImGui::Button("保存")) SaveScene();
        ImGui::SameLine();
        if (ImGui::Button("加载")) LoadScene("editor_scene.json");
        ImGui::SameLine();
        // Godot-style project switcher: pick a project (NeonRealm 3D /
        // NeonPvZ 2D / default sandbox), then pick a scene or 2D level.
        ImGui::SetNextItemWidth(178.0f);
        const char* projPreview = projectName_.empty()
                                      ? (projectDir_ == "." ? "默认场景" : projectDir_.c_str())
                                      : projectName_.c_str();
        if (ImGui::BeginCombo("##project_picker", projPreview)) {
            if (ImGui::Selectable("默认场景", projectDir_ == ".")) SwitchProject(".");
            ImGui::Separator();
            if (projects_.empty()) ScanProjects();
            for (size_t i = 0; i < projects_.size(); ++i) {
                const EditorProject& p = projects_[i];
                char label[256];
                std::snprintf(label, sizeof(label), "%s  [%s]###proj%d", p.name.c_str(),
                              p.mode == "2d" ? "2D" : "3D", static_cast<int>(i));
                if (ImGui::Selectable(label, projectSel_ == static_cast<int>(i)))
                    SwitchProject(p.dir);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("重新扫描项目")) ScanProjects();
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        // Scene / level picker for the active project.
        ImGui::SetNextItemWidth(196.0f);
        if (ImGui::BeginCombo("##scene_picker", currentSceneName_.empty()
                                                    ? "选择场景…"
                                                    : currentSceneName_.c_str())) {
            if (projectDir_ == ".")
                if (ImGui::Selectable("editor_scene.json", currentSceneName_ == "editor_scene.json"))
                    LoadScene("editor_scene.json");
            for (const std::string& s : projectScenes_) {
                if (ImGui::Selectable(s.c_str(), currentSceneName_ == BaseName(s)))
                    LoadProjectScene(s);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
                history_.Push(std::make_unique<DeleteEntityCommand>(
                    &entities_, static_cast<size_t>(selected_)));
                SetSelection(-1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(playtestActive_ ? "■ 停止试玩" : "▶ 试玩")) TogglePlaytest();
        ImGui::SameLine();
        // Hot reload toggle (T4.8): off by default. When on, script/asset mtime
        // changes restart the playtest / reload the cached assets (throttled).
        if (ImGui::Button(hotReload_ ? "● 热重载" : "○ 热重载")) hotReload_ = !hotReload_;
        ImGui::SameLine();
        // Multi-camera viewport preset (T4.8): 透视 / 顶视 / 前视 (also Tab).
        const char* camLabels[] = {"透视", "顶视", "前视"};
        int camSel = static_cast<int>(viewCam_);
        ImGui::SetNextItemWidth(88.0f);
        if (ImGui::Combo("##viewport_cam", &camSel, camLabels, 3))
            SetViewCam(static_cast<ViewCam>(camSel));
        ImGui::SameLine();
        if (ImGui::Button("导出场景")) ExportScene();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // Gizmo operation (W/E/R) and mode toggle for the selected entity.
        if (ImGui::Button(gizmoOp_ == ImGuizmo::TRANSLATE ? "[移动] W" : "移动 W"))
            gizmoOp_ = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoOp_ == ImGuizmo::ROTATE ? "[旋转] E" : "旋转 E"))
            gizmoOp_ = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoOp_ == ImGuizmo::SCALE ? "[缩放] R" : "缩放 R"))
            gizmoOp_ = ImGuizmo::SCALE;
        ImGui::SameLine();
        if (ImGui::Button(gizmoMode_ == ImGuizmo::LOCAL ? "[本地]" : "本地"))
            gizmoMode_ = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::Button(gizmoMode_ == ImGuizmo::WORLD ? "[世界]" : "世界"))
            gizmoMode_ = ImGuizmo::WORLD;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // View switcher (Unity/Godot style): 2D is the front-ortho camera
        // locked to the SAME scene, 3D is the perspective camera. Only the
        // camera changes - the project, scene and content stay identical.
        if (ImGui::Button(editMode_ == EditMode::Scene2D ? "[视图 2D] 切到 3D 透视"
                                                       : "[3D 透视] 切到 2D 视图")) {
            Set2DMode(editMode_ != EditMode::Scene2D);
        }
        ImGui::SameLine();
        ImGui::Text("实体 %zu", entities_.size());
    }
    ImGui::End();

    // The DockSpace's Begin/End (above) can overwrite the hover we resolved
    // after NewFrame, so re-resolve it right before the tool panels build.
    {
        ImGuiContext& ictx = *ImGui::GetCurrentContext();
        ImGuiWindow* best = nullptr;
        for (int wi = ictx.Windows.Size - 1; wi >= 0; --wi) {
            ImGuiWindow* w = ictx.Windows[wi];
            if (!w || w->Hidden) continue;
            if (w->DockNodeAsHost != nullptr) continue;
            if (w->ParentWindow != nullptr) continue;
            if (w->Flags & ImGuiWindowFlags_NoMouseInputs) continue;
            if (std::strcmp(w->Name, "视口") == 0) continue;
            if (std::strncmp(w->Name, "##", 2) == 0) continue;
            if (w->Rect().Contains(ictx.IO.MousePos)) {
                best = w;
                break;
            }
        }
        if (best) ictx.HoveredWindow = best;
    }
    BuildScenePanel();
    BuildAssetPanel();
    BuildResourcePanel();
    BuildInspectorPanel();
    BuildLogPanel();
    BuildBtPanel();
    BuildScriptPanel();
    BuildScriptEditorPanel();
    BuildPackagePanel();
    BuildProfilerPanel();
    BuildInputMapPanel();
    BuildNavPanel();
    BuildUIEditorPanel();
    BuildLocPanel();
    BuildViewportPanel();

    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
}

void EditorApp::LoadInputMapEdit() {
    inputMapEdit_ = script::InputMap::Defaults();
    std::ifstream in(projectDir_ + "/input.json", std::ios::binary);
    if (in.is_open()) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        if (!inputMapEdit_.Load(text, &err))
            NEON_LOG_ERROR("Editor: input.json parse failed: %s", err.c_str());
    }
    inputMapListenAction_ = "";
}

void EditorApp::SaveInputMapEdit() {
    std::ofstream out(projectDir_ + "/input.json", std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write '%s/input.json'", projectDir_.c_str());
        return;
    }
    out << inputMapEdit_.ToJson();
    NEON_LOG_INFO("Editor: input.json saved (%zu actions)", inputMapEdit_.Names().size());
}

void EditorApp::BuildInputMapPanel() {
    if (!showInputMap_) return;
    if (!ImGui::Begin("输入映射", &showInputMap_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("项目: %s/input.json", projectDir_.c_str());
    ImGui::SameLine();
    if (ImGui::Button("重新加载")) LoadInputMapEdit();
    ImGui::SameLine();
    if (ImGui::Button("保存")) SaveInputMapEdit();
    ImGui::Separator();
    if (ImGui::BeginTable("##inputmap", 3, ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("动作");
        ImGui::TableSetupColumn("按键");
        ImGui::TableSetupColumn("绑定");
        ImGui::TableHeadersRow();
        for (const std::string& name : inputMapEdit_.Names()) {
            const script::InputAction* a = inputMapEdit_.Find(name);
            if (!a) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(1);
            std::string keys;
            for (platform::Key k : a->positive)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k) + "+";
            for (platform::Key k : a->negative)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k) + "-";
            for (platform::Key k : a->keys)
                keys += (keys.empty() ? "" : " / ") + script::InputMap::KeyToName(k);
            ImGui::TextUnformatted(keys.empty() ? "(无)" : keys.c_str());
            ImGui::TableSetColumnIndex(2);
            const bool listening = inputMapListenAction_ == name;
            if (ImGui::Button(listening ? "等待按键..." : "改键", ImVec2(92.0f, 0.0f))) {
                inputMapListenAction_ = listening ? "" : name;
            }
        }
        ImGui::EndTable();
    }
    if (!inputMapListenAction_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           "请按一个新按键绑定到 '%s'...", inputMapListenAction_.c_str());
    ImGui::End();
}

void EditorApp::RunUISmokeTest() {
    auto check = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-UI-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };

    if (editMode_ != EditMode::Scene3D) {
        StopPlaytest();
        editMode_ = EditMode::Scene3D;
        NEON_LOG_INFO("EDITOR-UI-SMOKE: forced 3D mode for the canonical smoke run");
    }
    // The sandbox editor_scene.json is USER data (it can hold sprites etc.),
    // so the canonical smoke runs against a deterministic temp scene with
    // UTF-8 entity names and a mesh entity for the material round-trip checks.
    const std::string smokePrevScene = currentScenePath_;
    {
        const std::string smokeScene = GetTempDir() + "/smoke_base_scene.json";
        {
            std::ofstream out(smokeScene, std::ios::binary);
            // "\u5730\u9762" = 地面, "\u82F1\u96C4" = 英雄 (JSON escapes keep
            // this C++ source ASCII; the parsed names are real UTF-8).
            out << R"({"entities":[{"name":"\u5730\u9762","mesh":"cube",)"
                   R"("pos":[0,0,0],"scale":[10,1,10],"tint":[0.8,0.8,0.8,1],)"
                   R"("metallic":0,"roughness":0.8,"ao":1,"emissiveIntensity":1,)"
                   R"("albedoTex":"","mrTex":"","aoTex":"","emissiveTex":""},)"
                   R"({"name":"\u82F1\u96C4","mesh":"hero","pos":[0,0,5],)"
                   R"("scale":[1,1,1],"tint":[1,1,1,1],"metallic":0,"roughness":0.8,)"
                   R"("ao":1,"emissiveIntensity":1,"albedoTex":"","mrTex":"",)"
                   R"("aoTex":"","emissiveTex":""}]})";
        }
        LoadScene(smokeScene);
    }

    // --- Scene entity names stay UTF-8 (no mojibake regression) ---
    {
        const std::string ground = std::string("\xE5\x9C\xB0\xE9\x9D\xA2"); // 地面
        const std::string hero = std::string("\xE8\x8B\xB1\xE9\x9B\x84");   // 英雄
        bool foundHero = false;
        bool foundGround = false;
        for (const SceneEntity& e : entities_) {
            if (e.name == hero) foundHero = true;
            if (e.name == ground) foundGround = true;
        }
        check(!entities_.empty() && foundGround,
              "editor scene contains 地面 (UTF-8 intact)");
        check(foundHero, "editor scene contains 英雄 (UTF-8 intact)");
    }

    // --- Dear ImGui tool layer ---
    check(ImGui::GetCurrentContext() != nullptr, "ImGui context created");
    check(ImGui::GetIO().Fonts->IsBuilt(), "ImGui font atlas built");
    check(ImGui::GetIO().Fonts->Fonts.Size >= 1, "ImGui has at least one font");
    ImDrawData* dd = ImGui::GetDrawData();
    check(dd != nullptr && dd->CmdListsCount > 0, "ImGui produced draw data");

    // --- Add/remove component command round-trip ---
    {
        std::vector<SceneEntity> tmp(1);
        HistoryManager h;
        core::Json rb;
        rb.type_ = core::Json::Type::Object;
        core::Json shape;
        shape.type_ = core::Json::Type::String;
        shape.string_ = "box";
        rb.object_["shape"] = std::move(shape);
        h.Push(std::make_unique<AddComponentCommand>(&tmp, 0, "rigidbody", rb,
                                                     /*remove=*/false));
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "add-component command applies");
        h.Undo();
        check(tmp[0].extraComponents.empty(), "add-component command undoes");
        h.Redo();
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "add-component command redoes");
        h.Push(std::make_unique<AddComponentCommand>(&tmp, 0, "rigidbody", rb,
                                                     /*remove=*/true));
        check(tmp[0].extraComponents.empty(), "remove-component command applies");
        h.Undo();
        check(tmp[0].extraComponents.count("rigidbody") == 1u,
              "remove-component command undoes");
    }

    // --- UI editor auto-save: editing a doc with a real path persists ---
    {
        const std::string tmpDoc = GetTempDir() + "/ui_autosave.ui.json";
        uiDoc_ = ui::UiDocument{};
        uiDoc_.root.rect = {0, 0, 1280, 720};
        ui::UiNode* label = uiDoc_.root.AddChild(ui::UiNodeType::Label, "T");
        label->text = "hello";
        uiDocPath_ = tmpDoc;
        uiDocOpen_ = true;
        label->text = "world"; // simulate an edit through the inspector
        MarkUIDirty();         // should write the file immediately
        ui::UiDocument reloaded;
        check(reloaded.Load(tmpDoc) && reloaded.Find("T") &&
                  reloaded.Find("T")->text == "world",
              "UI auto-save persists edits to disk");
        uiDocOpen_ = false;
        uiDocPath_.clear();
        uiSelected_ = nullptr;
    }

    // --- Tool panels ---
    check(!core::GetRecentLogs(16).empty(), "log panel has engine log entries");
    check(!assetEntries_.empty(), "asset panel enumerated files");
    assetGridView_ = true; // the grid view renders from the next frame on;
                           // a crash here fails the smoke run
    check(assetGridView_, "asset panel thumbnail grid view enabled");

    // --- Transform gizmo ---
    // The gizmo renders every frame while an entity is selected; verify the
    // setup path ran and the matrix boundary (engine row-major Mat4 <-> ImGuizmo
    // column-major float[16]) round-trips a synthetic TRS without drift.
    if (editMode_ == EditMode::Scene3D) {
        check(gizmoDrawn_, "transform gizmo drawn in the viewport");
        check(gizmoBeginFrame_, "ImGuizmo::BeginFrame called every frame");
        check(gizmoAltWindowSet_, "gizmo hover bound to the dock host window");
        {
            const math::Rect2& vr = viewportScreenRect_;
            const float rw = vr.w > 0.0f ? vr.w : static_cast<float>(renderer_.ScreenWidth());
            const float rh = vr.h > 0.0f ? vr.h : static_cast<float>(renderer_.ScreenHeight());
            check(gizmoRect_[0] == (vr.w > 0.0f ? vr.x : 0.0f) &&
                      gizmoRect_[1] == (vr.h > 0.0f ? vr.y : 0.0f) &&
                      gizmoRect_[2] == rw && gizmoRect_[3] == rh,
                  "gizmo rect matches the viewport dock (scene render + picker)");
        }
    }
    auto nearVec = [](const math::Vec3& a, const math::Vec3& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f &&
               std::fabs(a.z - b.z) < 1e-4f;
    };
    {
        math::Vec3 pos{1.25f, -2.5f, 3.75f};
        math::Vec3 scale{2.0f, 0.5f, 1.5f};
        math::Quat rot = math::Quat::FromEuler(0.4f, -0.7f, 0.2f);
        math::Mat4 model = math::Mat4::Translation(pos) * rot.ToMat4() *
                           math::Mat4::Scale(scale);
        float gizmo[16];
        Mat4ToGizmo(model, gizmo);
        math::Mat4 back;
        GizmoToMat4(gizmo, back);
        math::Vec3 p, s;
        math::Quat q;
        DecomposeModel(back, p, s, q);
        check(nearVec(p, pos), "gizmo round-trip preserves translation");
        check(nearVec(s, scale), "gizmo round-trip preserves scale");
        check(math::Distance(rot.Rotate({0, 0, -1}), q.Rotate({0, 0, -1})) < 1e-3f,
              "gizmo round-trip preserves rotation");
    }
    // --- Undo/redo: scene edits route through the history command stack ---
    // Do -> undo -> redo on the real editor scene: push transform edits,
    // verify the merge policy (consecutive same-field edits = one undo step)
    // and the drag-end seal (the next drag = a new undo step), then drive
    // Ctrl+Z / Ctrl+Y through the real keyboard event path.
    {
        const size_t idx = 0; // deterministic: the first scene entity
        check(idx < entities_.size(), "undo/redo: smoke has an entity to edit");
        if (idx < entities_.size()) {
            SceneEntity& sel = entities_[idx];
            const math::Vec3 orig = sel.pos;
            const math::Vec3 step1 = orig + math::Vec3{0.5f, -0.25f, 0.125f};
            const math::Vec3 step2 = step1 + math::Vec3{0.1f, 0.2f, 0.3f};
            const math::Vec3 step3 = step2 + math::Vec3{0.2f, -0.3f, 0.4f};
            const math::Vec3 step4 = step3 + math::Vec3{0.3f, 0.1f, -0.2f};
            const size_t depthBefore = history_.UndoDepth();

            auto editPos = [&](const math::Vec3& from, const math::Vec3& to) {
                history_.Push(std::make_unique<EditTransformCommand>(
                    &entities_, static_cast<int>(idx), from, sel.rot, sel.scale, to, sel.rot,
                    sel.scale, EditTransformCommand::kPos));
            };

            editPos(orig, step1);
            check(nearVec(sel.pos, step1),
                  "undo/redo: transform edit applies through the command stack");
            editPos(step1, step2); // continuous chain -> coalesces
            check(history_.UndoDepth() == depthBefore + 1,
                  "undo/redo: consecutive same-field edits merge into one undo step");
            check(nearVec(sel.pos, step2),
                  "undo/redo: merged command holds the final value");

            // Value-chain guard: an edit whose ORIGINAL does not equal the last
            // applied value (e.g. a programmatic set between two separate
            // inspector drags) must NOT merge into the (unsealed) top step.
            editPos(step1, step3);
            check(history_.UndoDepth() == depthBefore + 2,
                  "undo/redo: discontinuous chain opens its own undo step");
            check(nearVec(sel.pos, step3), "undo/redo: discontinuous edit applies");

            // Seal the top command (what the gizmo does when a drag ends): the
            // next edit must open a fresh undo step too.
            if (EditTransformCommand* top =
                    dynamic_cast<EditTransformCommand*>(history_.TopUndo())) {
                top->Seal();
            }
            editPos(step3, step4);
            check(history_.UndoDepth() == depthBefore + 3,
                  "undo/redo: sealed command opens a new undo step");
            check(nearVec(sel.pos, step4), "undo/redo: post-seal edit applies");

            // Ctrl+Z / Ctrl+Y through the real keyboard event path.
            auto shortcut = [this](platform::Key key, bool withCtrl) {
                if (withCtrl) {
                    platform::InputEvent ctrlDown;
                    ctrlDown.type = platform::InputEvent::Type::KeyDown;
                    ctrlDown.key = platform::Key::Control;
                    Input()->HandleEvent(ctrlDown);
                    OnEvent(ctrlDown);
                }
                platform::InputEvent press;
                press.type = platform::InputEvent::Type::KeyDown;
                press.key = key;
                Input()->HandleEvent(press);
                OnEvent(press);
                if (withCtrl) {
                    platform::InputEvent ctrlUp;
                    ctrlUp.type = platform::InputEvent::Type::KeyUp;
                    ctrlUp.key = platform::Key::Control;
                    Input()->HandleEvent(ctrlUp);
                    OnEvent(ctrlUp);
                }
            };
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, step3), "undo/redo: Ctrl+Z undoes the post-seal edit");
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, step1),
                  "undo/redo: Ctrl+Z undoes the discontinuous edit");
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, orig), "undo/redo: Ctrl+Z undoes the merged drag");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step2), "undo/redo: Ctrl+Y redoes the merged drag");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step3),
                  "undo/redo: Ctrl+Y redoes the discontinuous edit");
            shortcut(platform::Key::Y, true);
            check(nearVec(sel.pos, step4), "undo/redo: Ctrl+Y redoes the post-seal edit");
            // Leave the scene as it was: undo everything we just did.
            shortcut(platform::Key::Z, true);
            shortcut(platform::Key::Z, true);
            shortcut(platform::Key::Z, true);
            check(nearVec(sel.pos, orig),
                  "undo/redo: restores the original transform");
        }
    }

    // --- Add/delete index stability through the command stack ---
    // add -> delete -> undo (restore) -> redo (delete again) must keep every
    // other entity index valid: commands record the index + an entity copy and
    // rely on LIFO undo / FIFO redo to execute against the exact layout they
    // captured.
    {
        const size_t baseCount = entities_.size();
        check(baseCount > 1, "undo/redo: index-stability smoke needs entities");
        if (baseCount > 1) {
            const size_t mid = 1;
            const std::string nameAtMid = entities_[mid].name;
            const SceneEntity sample = entities_[mid]; // valid-mesh stand-in
            history_.Push(std::make_unique<AddEntityCommand>(&entities_, sample, mid));
            check(entities_.size() == baseCount + 1 && entities_[mid].name == sample.name,
                  "undo/redo: add inserts at the recorded index");
            history_.Push(std::make_unique<DeleteEntityCommand>(&entities_, mid));
            check(entities_.size() == baseCount && entities_[mid].name == nameAtMid,
                  "undo/redo: delete removes the inserted entity (indices stable)");
            history_.Undo();
            check(entities_.size() == baseCount + 1 && entities_[mid].name == sample.name,
                  "undo/redo: undo delete restores the entity at its recorded index");
            history_.Redo();
            check(entities_.size() == baseCount && entities_[mid].name == nameAtMid,
                  "undo/redo: redo delete removes it again (indices stable)");
        }
    }

    // --- Gizmo activation/drag (deterministic, drives ImGuizmo's input path) ---
    // A real pointer drag can't be automated headlessly, but the activation
    // path is: RunGizmoDragSim() (called inside the viewport window scope on
    // the smoke frame) synthesizes a hover over the dock host, a press on the
    // entity's screen position, a drag, and a release, and verifies IsUsing()
    // follows and the model matrix moves. Assert here that it ran.
    check(gizmoDragSimulated_, "gizmo drag simulation ran");

    assets::AssetStats stats = assetMgr_.Stats();
    check(stats.textures >= 4, "resource panel: PBR textures cached");
    check(stats.meshes >= 1, "resource panel: meshes cached");

    size_t beforeImport = entities_.size();
    ImportAssetPath("assets/models/DamagedHelmet/DamagedHelmet.gltf");
    check(entities_.size() == beforeImport + 1, "asset import adds glTF entity");
    if (entities_.size() > beforeImport) {
        const SceneEntity& last = entities_.back();
        check(last.meshKey.rfind("gltf:", 0) == 0 && last.mesh.Valid(),
              "imported entity resolves glTF mesh");
    }

    // --- Editor config round-trip: save then load the project dir ---
    {
        const std::string cfgDir = GetTempDir() + "/cfg_proj";
        const std::string cfgPrev = projectDir_;
        projectDir_ = cfgDir;
        SaveEditorConfig();
        LoadEditorConfig();
        check(projectDir_ == cfgDir, "editor config project dir round-trips");
        // Restore the REAL project dir (and persist it) so the temp cfg_proj
        // directory is never written into the user's editor config.
        projectDir_ = cfgPrev;
        SaveEditorConfig();
    }

    // --- Material editor: metallic / AO / texture-slot edits via undo ---
    // Set a texture path + metallic + AO on a selected entity through the
    // command stack, verify undo/redo restores, then leave the edits applied so
    // the export round-trip below asserts the material JSON + restored
    // SceneMesh carry them.
    const std::string kAlbedoTex = "assets/models/DamagedHelmet/Default_albedo.jpg";
    const float kMetallic = 0.45f;
    const float kAO = 0.7f;
    {
        const size_t idx = 0;
        check(idx < entities_.size(), "material: smoke has an entity to edit");
        if (idx < entities_.size()) {
            SceneEntity& sel = entities_[idx];
            const float origMetallic = sel.metallic;
            const float origAO = sel.ao;
            const std::string origAlbedo = sel.albedoTex;

            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, static_cast<int>(idx), ApplyMetallicProp, origMetallic, kMetallic));
            check(sel.metallic == kMetallic && sel.material.metallic == kMetallic,
                  "material: metallic edit applies through the command stack");
            history_.Undo();
            check(sel.metallic == origMetallic && sel.material.metallic == origMetallic,
                  "material: metallic undo restores the original value");
            history_.Redo();
            check(sel.metallic == kMetallic && sel.material.metallic == kMetallic,
                  "material: metallic redo reapplies the edit");

            gfx::Texture tex = assetMgr_.LoadTexture(kAlbedoTex);
            check(tex.Valid(), "material: albedo texture loads through the AssetManager");
            if (tex.Valid()) {
                const TextureSlotValue oldVal{origAlbedo, sel.material.albedo};
                const TextureSlotValue newVal{kAlbedoTex, tex.Handle()};
                history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                    &entities_, static_cast<int>(idx), ApplyAlbedoTexSlot, oldVal, newVal));
                check(sel.albedoTex == kAlbedoTex &&
                          sel.material.albedo.id == tex.Handle().id,
                      "material: albedo texture edit applies through the command stack");
                history_.Undo();
                check(sel.albedoTex == origAlbedo && !sel.material.albedo.Valid(),
                      "material: albedo undo restores the empty slot");
                history_.Redo();
                check(sel.albedoTex == kAlbedoTex && sel.material.albedo.Valid(),
                      "material: albedo redo reapplies the path + handle");
            }

            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, static_cast<int>(idx), ApplyAOProp, origAO, kAO));
            check(sel.ao == kAO && sel.material.aoStrength == kAO,
                  "material: AO edit applies through the command stack");
            history_.Undo();
            check(sel.ao == origAO, "material: AO undo restores the original value");
            history_.Redo();
            check(sel.ao == kAO && sel.material.aoStrength == kAO,
                  "material: AO redo reapplies the edit");
        }
    }

    // --- Export → load round-trip (temp project dir; no repo pollution) ---
    const size_t exportCount = entities_.size();
    const std::string oldProjectDir = projectDir_;
    projectDir_ = GetTempDir();
    core::Status exportStatus = ExportScene();
    projectDir_ = oldProjectDir;
    check(exportStatus.Ok(), "export scene writes componentized JSON");
    if (exportStatus.Ok()) {
        std::string exportedPath = GetTempDir() + "/scenes/exported_scene.json";
        std::ifstream fin(exportedPath);
        std::stringstream fss;
        fss << fin.rdbuf();
        auto parsed = scene::SceneFile::Parse(fss.str());
        check(parsed.Ok(), "exported scene parses with SceneFile::Parse");
        if (parsed.Ok()) {
            check(parsed.Value().entities.size() == exportCount,
                  "exported scene contains every editor entity");
            if (!parsed.Value().entities.empty()) {
                check(parsed.Value().entities[0].name == entities_[0].name,
                      "exported entity name matches editor entity");
                // The material edit round-trips into the exported material JSON.
                const scene::ComponentDef* meshComp = nullptr;
                for (const auto& c : parsed.Value().entities[0].components) {
                    if (c.name == "mesh") {
                        meshComp = &c;
                        break;
                    }
                }
                const core::Json* matJson =
                    meshComp ? meshComp->data.Get("material") : nullptr;
                const core::Json* alb = matJson ? matJson->Get("albedoTex") : nullptr;
                const core::Json* met = matJson ? matJson->Get("metallic") : nullptr;
                const core::Json* ao = matJson ? matJson->Get("ao") : nullptr;
                check(meshComp != nullptr && matJson != nullptr && alb != nullptr &&
                          met != nullptr && ao != nullptr &&
                          alb->GetString() == kAlbedoTex &&
                          std::fabs(met->GetNumber() - kMetallic) < 1e-6f &&
                          std::fabs(ao->GetNumber() - kAO) < 1e-6f,
                      "exported material JSON carries the texture path + metallic + AO");
            }
            // Import back: Instantiate the exported scene and verify SceneMesh
            // restores the material texture path + scalar edits.
            scene::ComponentRegistry reg;
            scene::RegisterBuiltinComponents(reg);
            ecs::World world;
            scene::PrefabLibrary prefs;
            auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
            check(inst.Ok(), "imported exported scene instantiates");
            auto view = world.ViewAll<scene::SceneMesh>();
            check(view.Size() > 0, "imported scene has mesh components");
            if (view.Size() > 0) {
                ecs::Entity e0 = world.EntityAt<scene::SceneMesh>(0);
                const scene::SceneMesh* m = world.Get<scene::SceneMesh>(e0);
                check(m != nullptr && m->albedoTex == kAlbedoTex &&
                          std::fabs(m->metallic - kMetallic) < 1e-6f &&
                          std::fabs(m->ao - kAO) < 1e-6f,
                      "imported SceneMesh carries the texture path + metallic + AO");
            }
        }
    }

    // --- Behavior tree editor: canvas + save/load + link path + undo ---
    {
        check(btCanvasDrawn_, "bt canvas renders the seeded tree");
        check(btGraph_.NodeCount() == 3u && btGraph_.LinkCount() == 2u,
              "bt panel seeded a 3-node linked tree");

        // Save/load through the editor's own panel functions (temp dir), not a
        // raw file write: the whole point is that what the editor saves the
        // editor (and the runtime) can read back.
        const std::string btPath = GetTempDir() + "/bt_smoke.bt.json";
        const std::string savedJson = btGraph_.Serialize();
        check(!savedJson.empty(), "bt smoke: tree serialized");
        check(BtSaveToFile(btPath), "bt smoke: editor save writes .bt.json");
        btGraph_ = btgraph::BtGraph{};
        check(BtLoadFromFile(btPath), "bt smoke: editor load reads .bt.json back");
        check(btGraph_.Serialize() == savedJson,
              "bt smoke: editor save/load round-trip identical");

        // The unloadable-tree guard: a lone empty composite must be refused.
        btgraph::BtGraph solo;
        solo.AddNode("sequence", math::Vec2{});
        btGraph_ = std::move(solo);
        check(!BtSaveToFile(btPath), "bt smoke: empty composite save refused");

        // Canvas link path: select node A, Ctrl+click node B -> B is linked as
        // A's child (exercises the real click handler, not the raw model).
        btGraph_ = btgraph::BtGraph{};
        const std::string a = btGraph_.AddNode("sequence", math::Vec2{20.f, 20.f});
        const std::string b = btGraph_.AddNode("wait", math::Vec2{240.f, 240.f});
        btSelected_ = a;
        BtCanvasClick(math::Vec2{245.f, 245.f}, /*ctrl=*/true, /*shift=*/false);
        check(btGraph_.LinkCount() == 1u, "bt smoke: ctrl+click creates a link");
        if (btGraph_.LinkCount() == 1u) {
            const btgraph::BtGraphLink& link = btGraph_.Links()[0];
            check(link.parent == a && link.child == b,
                  "bt smoke: ctrl+click links the clicked node under the selected");
        }
        {
            core::Json tree = core::Json::Parse(btGraph_.Serialize(), nullptr);
            const core::Json* root = tree.Get("root");
            const core::Json* kids = root ? root->Get("children") : nullptr;
            check(kids != nullptr && kids->Size() == 1u &&
                      kids->At(0)->Get("type")->GetString() == std::string("wait"),
                  "bt smoke: serialized tree nests the linked child");
        }

        // Editor graph edits route through the undo stack: add -> undo -> redo.
        const size_t nodesBefore = btGraph_.NodeCount();
        const btgraph::BtGraph before = btGraph_;
        const std::string nid = btGraph_.AddNode("in_range", math::Vec2{0.f, 0.f});
        BtPushSnapshot(before);
        check(!nid.empty() && btGraph_.NodeCount() == nodesBefore + 1,
              "bt smoke: canvas add node");
        btHistory_.Undo();
        check(btGraph_.NodeCount() == nodesBefore, "bt smoke: undo restores the graph");
        btHistory_.Redo();
        check(btGraph_.NodeCount() == nodesBefore + 1, "bt smoke: redo reapplies the add");
        btHistory_.Undo();
        check(btGraph_.NodeCount() == nodesBefore, "bt smoke: graph left clean after undo");
    }

    // --- Script panel: syntax check + attach/configure via command stack + export ---
    // Point the project at a temp dir with one valid + one broken script, run
    // the real check path, attach the valid script to a selected entity through
    // the undo command, export, and assert the JSON carries the script
    // component (the same flow the user drives in the 脚本 panel).
    {
        const std::string proj = GetTempDir() + "/script_smoke_proj";
        EnsureDirs(proj + "/scripts");
        {
            std::ofstream out(proj + "/scripts/good.lua", std::ios::binary);
            out << "function on_update(ent, dt)\n  Log('tick')\nend\n";
        }
        {
            std::ofstream out(proj + "/scripts/broken.lua", std::ios::binary);
            out << "function on_update(ent, dt)\n  this is not lua !!!\nend\n";
        }
        const std::string prevProj = projectDir_;
        projectDir_ = proj;
        RefreshScriptChecks();
        check(!scriptFiles_.empty(), "script panel: project scripts enumerated");
        bool sawGood = false, sawBroken = false, brokenHasLine = false;
        for (size_t i = 0; i < scriptFiles_.size(); ++i) {
            if (scriptFiles_[i] == "scripts/good.lua")
                sawGood = scriptChecks_[i].ok && scriptChecks_[i].message.empty();
            else if (scriptFiles_[i] == "scripts/broken.lua") {
                sawBroken = !scriptChecks_[i].ok && !scriptChecks_[i].message.empty();
                brokenHasLine = scriptChecks_[i].line > 0;
            }
        }
        check(sawGood && sawBroken && brokenHasLine,
              "script panel: syntax check passes valid and flags broken with a line");

        if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
            const int idx = selected_;
            SceneEntity& sel = entities_[static_cast<size_t>(idx)];
            core::Json vars;
            vars.type_ = core::Json::Type::Object;
            core::Json speed;
            speed.type_ = core::Json::Type::Number;
            speed.number_ = 1.5;
            vars.object_["speed"] = speed;
            std::vector<SceneScriptFields> newList = sel.scripts;
            newList.push_back({"lua", "scripts/good.lua", vars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, sel.scripts, newList,
                /*mergeable=*/false));
            check(sel.scripts.size() == 1 && sel.scripts[0].path == "scripts/good.lua" &&
                      sel.scripts[0].backend == "lua" && sel.scripts[0].vars.IsObject() &&
                      sel.scripts[0].vars.Get("speed")->GetNumber() == 1.5,
                  "script panel: attach applies through the command stack");
            history_.Undo();
            check(sel.scripts.empty(),
                  "script panel: undo detaches the script component");
            history_.Redo();
            check(sel.scripts.size() == 1 && sel.scripts[0].path == "scripts/good.lua",
                  "script panel: redo re-attaches the script component");

            // Export and assert the mounted scripts land in the scene JSON as
            // the "scripts" list component (flat, one entry per mounted script).
            const std::string expProj = GetTempDir();
            projectDir_ = expProj;
            core::Status exp = ExportScene();
            projectDir_ = prevProj;
            check(exp.Ok(), "script panel: export with an attached script succeeds");
            if (exp.Ok()) {
                std::ifstream fin(expProj + "/scenes/exported_scene.json");
                std::stringstream ss;
                ss << fin.rdbuf();
                auto parsed = scene::SceneFile::Parse(ss.str());
                check(parsed.Ok(), "script panel: exported scene parses");
                if (parsed.Ok() && static_cast<size_t>(idx) < parsed.Value().entities.size()) {
                    const scene::ComponentDef* sc = nullptr;
                    for (const auto& c : parsed.Value().entities[static_cast<size_t>(idx)].components) {
                        if (c.name == "scripts") {
                            sc = &c;
                            break;
                        }
                    }
                    const core::Json* items = sc ? sc->data.Get("items") : nullptr;
                    bool scriptOk = items && items->IsArray() && items->Size() == 1;
                    const core::Json* first = scriptOk ? items->At(0) : nullptr;
                    scriptOk = scriptOk && first && first->Get("backend") &&
                               first->Get("path") && first->Get("vars") &&
                               first->Get("backend")->GetString() == "lua" &&
                               first->Get("path")->GetString() == "scripts/good.lua" &&
                               first->Get("vars")->Get("speed")->GetNumber() == 1.5;
                    check(scriptOk,
                          "script panel: exported JSON carries the script component");
                }
            }
        }
        projectDir_ = prevProj;
        // Leave the script attached on the entity: it exercises the playtest
        // path (a missing script file is skipped non-fatally by the runtime).
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: script panel checks done");
    }

    // --- Script panel sync invalidation on entity-list mutation (T4.5 review) ---
    // The panel caches its dropdown + vars buffer by the selected INDEX. Any
    // mutation that appends/removes/moves entities (or reselects after a load)
    // must invalidate that cache, or the panel shows the PREVIOUS occupant's
    // script and 附加 silently attaches it to the entity that now sits at the
    // index. This reproduces the reported flow: select the last entity and sync
    // the panel to a distinctive script, AddEntity (appends + reselects the new
    // last), then verify the cache was invalidated and a fresh attach lands on
    // the NEW entity, not the stale one.
    {
        const int last = static_cast<int>(entities_.size()) - 1;
        check(last >= 0, "script sync: smoke has an entity to select");
        if (last >= 0) {
            SetSelection(last);
            // Emulate the panel having synced to the last entity + a script
            // attached to it (the stale state that must not leak forward).
            SceneEntity& oldLast = entities_[static_cast<size_t>(last)];
            core::Json staleVars;
            staleVars.type_ = core::Json::Type::Object;
            core::Json staleMarker;
            staleMarker.type_ = core::Json::Type::Number;
            staleMarker.number_ = 9.0;
            staleVars.object_["stale"] = staleMarker;
            // Replace the entity's mounted list with one distinctive script
            // (the stale panel state that must not leak to the next entity).
            std::vector<SceneScriptFields> staleList;
            staleList.push_back({"lua", "scripts/stale.lua", staleVars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, last, ApplyScriptList, oldLast.scripts, staleList,
                /*mergeable=*/false));
            check(oldLast.scripts.size() == 1 && oldLast.scripts[0].path == "scripts/stale.lua",
                  "script sync: distinctive script attached to the last entity");
            // The insert below may reallocate the vector, so keep the stale
            // path by value (never hold a reference across AddEntity).
            const std::string stalePath = oldLast.scripts[0].path;
            scriptSyncEntity_ = last; // panel cache now points at the last index
            scriptAttachIndex_ = 0;

            const size_t countBefore = entities_.size();
            AddEntity("cube"); // appends + reselects the new last entity
            check(entities_.size() == countBefore + 1,
                  "script sync: AddEntity appends a new entity");
            check(selected_ == static_cast<int>(entities_.size()) - 1,
                  "script sync: AddEntity selects the new last entity");
            check(scriptSyncEntity_ == -1,
                  "script sync: entity-list mutation invalidates the panel sync cache");

            // Attach through the real command path: must land on the NEW entity.
            const int freshIdx = static_cast<int>(entities_.size()) - 1;
            SceneEntity& fresh = entities_[static_cast<size_t>(freshIdx)];
            core::Json freshVars;
            freshVars.type_ = core::Json::Type::Object;
            core::Json freshMarker;
            freshMarker.type_ = core::Json::Type::Number;
            freshMarker.number_ = 3.0;
            freshVars.object_["hp"] = freshMarker;
            std::vector<SceneScriptFields> freshList = fresh.scripts;
            freshList.push_back({"lua", "scripts/good.lua", freshVars});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, freshIdx, ApplyScriptList, fresh.scripts, freshList,
                /*mergeable=*/false));
            check(fresh.scripts.size() == 1 && fresh.scripts[0].path == "scripts/good.lua" &&
                      fresh.scripts[0].vars.Get("hp")->GetNumber() == 3.0,
                  "script sync: attach lands on the new entity");
            check(entities_[static_cast<size_t>(last)].scripts.size() == 1 &&
                      entities_[static_cast<size_t>(last)].scripts[0].path == stalePath,
                  "script sync: the previous entity keeps its own script (no stale attach)");
            history_.Undo(); // leave the new cube script-less
            check(fresh.scripts.empty(),
                  "script sync: undo clears the new entity's script");
        }
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: sync invalidation checks done");
    }

    // --- Script mount list (multiple scripts, component-style add/remove) ---
    // The mounted scripts behave like other components: one list where the
    // every entry is equal - add appends, remove erases, multiple allowed,
    // and each change is a single undo step.
    {
        const int idx = static_cast<int>(entities_.size()) - 1;
        check(idx >= 0, "script list: smoke has an entity");
        if (idx >= 0) {
            SetSelection(idx);
            SceneEntity& ent = entities_[static_cast<size_t>(idx)];
            check(ent.scripts.empty(),
                  "script list: fresh entity mounts no scripts");

            // First add appends one entry.
            std::vector<SceneScriptFields> one = ent.scripts;
            one.push_back({"lua", "scripts/good.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, one,
                /*mergeable=*/false));
            check(ent.scripts.size() == 1 && ent.scripts[0].path == "scripts/good.lua",
                  "script list: first mount appends an entry");

            // Second add appends another entry (multiple scripts).
            std::vector<SceneScriptFields> two = ent.scripts;
            two.push_back({"lua", "scripts/stale.lua", {}});
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, two,
                /*mergeable=*/false));
            check(ent.scripts.size() == 2 && ent.scripts[0].path == "scripts/good.lua" &&
                      ent.scripts[1].path == "scripts/stale.lua",
                  "script list: second mount appends (multiple scripts)");

            // Remove the first entry: the rest stay put, no promotion concept.
            std::vector<SceneScriptFields> afterRemove = ent.scripts;
            afterRemove.erase(afterRemove.begin());
            history_.Push(std::make_unique<
                EditPropertyCommand<std::vector<SceneScriptFields>>>(
                &entities_, idx, ApplyScriptList, ent.scripts, afterRemove,
                /*mergeable=*/false));
            check(ent.scripts.size() == 1 && ent.scripts[0].path == "scripts/stale.lua",
                  "script list: removing an entry leaves the rest unchanged");

            // Undo/redo replay the whole list in single steps.
            history_.Undo();
            check(ent.scripts.size() == 2 && ent.scripts[0].path == "scripts/good.lua",
                  "script list: undo restores both mounts");
            history_.Undo();
            check(ent.scripts.size() == 1 && ent.scripts[0].path == "scripts/good.lua",
                  "script list: undo restores the first mount");
            history_.Redo();
            check(ent.scripts.size() == 2 && ent.scripts[1].path == "scripts/stale.lua",
                  "script list: redo replays the second mount");
            history_.Redo();
            check(ent.scripts.size() == 1 && ent.scripts[0].path == "scripts/stale.lua",
                  "script list: redo replays the removal");

            // Leave the entity unmounted so the playtest sandbox stays clean.
            history_.Undo();
            history_.Undo();
            history_.Undo();
            check(ent.scripts.empty(),
                  "script list: smoke leaves the entity unmounted");
        }
        NEON_LOG_INFO("EDITOR-SCRIPT-SMOKE: script list checks done");
    }

    // --- Profiler panel (T4.8): the panel opened at frame 29 and populated its
    // stats + rolling frame-time buffer during this frame's UI build. ---
    check(profilerDrawn_, "profiler panel rendered its stats");
    {
        bool anyMs = false;
        for (float v : profilerMs_) {
            if (v > 0.0f) {
                anyMs = true;
                break;
            }
        }
        check(anyMs, "profiler panel recorded frame-time samples");
    }

    // --- Multi-camera viewport (T4.8): the three presets expose the right
    // projection + look direction, and the ortho pick ray stays parallel to the
    // forward axis (not through the eye). Frame 31/32 verify the top view
    // actually renders. ---
    {
        const gfx::Camera persp = ActiveCamera();
        check(!persp.ortho, "multi-cam: perspective preset is perspective");
        viewCam_ = ViewCam::Top;
        const gfx::Camera top = ActiveCamera();
        check(top.ortho, "multi-cam: top preset is orthographic");
        const math::Vec3 topFwd = (top.target - top.position).Normalized();
        check(std::fabs(topFwd.y + 1.0f) < 1e-4f && std::fabs(topFwd.x) < 1e-4f &&
                  std::fabs(topFwd.z) < 1e-4f,
              "multi-cam: top preset looks down -Y");
        viewCam_ = ViewCam::Front;
        const gfx::Camera front = ActiveCamera();
        check(front.ortho, "multi-cam: front preset is orthographic");
        const math::Vec3 frontFwd = (front.target - front.position).Normalized();
        check(std::fabs(frontFwd.z + 1.0f) < 1e-4f && std::fabs(frontFwd.x) < 1e-4f &&
                  std::fabs(frontFwd.y) < 1e-4f,
              "multi-cam: front preset looks down -Z");
        const math::Ray orthoRay = ScreenRay(top, 1.5f, {640.0f, 360.0f});
        const math::Vec3 rayDir = orthoRay.dir.Normalized();
        check(std::fabs(rayDir.y + 1.0f) < 1e-4f,
              "multi-cam: ortho pick ray is parallel to the forward axis");
        viewCam_ = ViewCam::Perspective;
    }

    // --- Asset thumbnails (T4.8): select a model asset in the asset panel so
    // the next OnRender generates its offscreen thumbnail (verified by the
    // frame poll above). A texture asset registers the image-preview texture
    // id. ---
    {
        const std::string kThumbPath = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
        smokeThumbPath_ = kThumbPath;
        // Navigate the panel into the model's directory so the listing (one
        // level, like the real UI) contains the asset, then select it.
        std::string thumbParent = kThumbPath;
        const size_t slash = thumbParent.find_last_of('/');
        if (slash != std::string::npos) thumbParent = thumbParent.substr(0, slash);
        assetDir_ = thumbParent;
        RefreshAssetDir();
        bool found = false;
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            if (assetEntries_[i].path == kThumbPath) {
                selectedAsset_ = static_cast<int>(i);
                found = true;
                break;
            }
        }
    check(found, "asset thumbnail: helmet glTF present in the asset panel listing");
        if (found) RequestMeshThumbnail(kThumbPath);
        check(std::find(meshThumbQueue_.begin(), meshThumbQueue_.end(), kThumbPath) !=
                  meshThumbQueue_.end(),
              "asset thumbnail: mesh thumbnail render requested");

        gfx::Texture tex =
            assetMgr_.LoadTexture("assets/models/DamagedHelmet/Default_albedo.jpg");
        check(tex.Valid(), "asset thumbnail: texture asset loads for the preview");
        ImportAssetPath("assets/models/DamagedHelmet/Default_albedo.jpg");
        check(previewTexId_ != ImTextureID_Invalid,
              "asset thumbnail: image preview texture registered for ImGui");
    }

    // --- Prefab workflow (Godot-style): library load + instantiate + save ---
    {
        const std::string proj = GetTempDir() + "/prefab_proj";
        EnsureDirs(proj + "/prefabs");
        {
            std::ofstream out(proj + "/prefabs/watchtower.json", std::ios::binary);
            out << R"({"components":{"transform":{"pos":[0,0,0],"scale":[1,1,1]},
                      "mesh":{"meshKey":"cube","colorHex":"#AABBCC"},
                      "health":{"hp":50,"maxHp":50}}})";
        }
        const std::string prev = projectDir_;
        projectDir_ = proj;
        LoadPrefabLibrary();
        check(prefabLib_.Has("watchtower"),
              "prefab: library loads prefabs/watchtower.json");
        const size_t before = entities_.size();
        AddEntity("prefab:watchtower");
        check(entities_.size() == before + 1 && entities_.back().prefab == "watchtower",
              "prefab: instantiate appends an entity with the prefab reference");
        SetSelection(static_cast<int>(entities_.size()) - 1);
        SavePrefab("watchtower_copy");
        check(prefabLib_.Has("watchtower_copy"),
              "prefab: SavePrefab registers a new template");
        history_.Undo(); // drop the instanced entity so later smoke checks
                         // (playtest) run against the canonical scene
        projectDir_ = prev;
    }

    // --- Asset panel: create + import actions (temp project) ---
    {
        const std::string proj = GetTempDir() + "/asset_proj";
        MakeDir(proj);
        const std::string prevDir = assetDir_;
        const std::string prevProj = projectDir_;
        projectDir_ = proj;
        assetDir_ = proj + "/assets";
        MakeDir(assetDir_);
        CreateAssetFile("test.lua", 1);
        check(std::ifstream(assetDir_ + "/test.lua").is_open(),
              "asset: create lua file");
        CreateAssetFile("data.json", 2);
        check(std::ifstream(assetDir_ + "/data.json").is_open(),
              "asset: create json file");
        CreateAssetFile("subdir", 0);
        const std::string subdir = assetDir_ + "/subdir";
        struct _stat64 st;
        check(_stat64(subdir.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR),
              "asset: create directory");
        const std::string src = GetTempDir() + "/asset_src.png";
        {
            std::ofstream out(src, std::ios::binary);
            out << "fake png bytes";
        }
        ImportAssetFile(src);
        check(std::ifstream(assetDir_ + "/asset_src.png").is_open(),
              "asset: import copies the file into the project");
        ImportAssetFile(src); // duplicate -> numbered name
        check(std::ifstream(assetDir_ + "/asset_src_1.png").is_open(),
              "asset: duplicate import gets a numbered name");
        // Directory import (a model resource pack with textures + subfolders).
        const std::string pack = GetTempDir() + "/asset_pack";
        MakeDir(pack);
        MakeDir(pack + "/models");
        MakeDir(pack + "/models/tex");
        {
            std::ofstream out(pack + "/models/foo.obj", std::ios::binary);
            out << "v 0 0 0\n";
            std::ofstream out2(pack + "/models/foo.mtl", std::ios::binary);
            out2 << "newmtl mat\n";
            std::ofstream out3(pack + "/models/tex/foo.png", std::ios::binary);
            out3 << "png";
        }
        ImportAssetFile(pack);
        check(std::ifstream(assetDir_ + "/asset_pack/models/foo.obj").is_open(),
              "asset: directory import copies nested model files");
        check(std::ifstream(assetDir_ + "/asset_pack/models/tex/foo.png").is_open(),
              "asset: directory import copies nested texture files");
        // Delete the imported file (recycle bin on Windows; removed here).
        selectedAsset_ = -1;
        RefreshAssetDir();
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            if (assetEntries_[i].name == "asset_src.png") {
                selectedAsset_ = static_cast<int>(i);
                break;
            }
        }
        check(selectedAsset_ >= 0, "asset: delete target found in the listing");
        assetDeletePending_ = selectedAsset_;
        if (selectedAsset_ >= 0) {
            const std::string victim =
                assetEntries_[static_cast<size_t>(selectedAsset_)].path;
            check(DeletePathRecursive(victim), "asset: delete removes the file");
            check(!std::ifstream(victim).is_open(), "asset: deleted file is gone");
        }
        // Relative-path delete: after a project switch the asset panel points
        // at "projects/xxx/assets" (relative); SHFileOperationW silently fails
        // on relative paths, so DeletePathRecursive must resolve them first.
        {
            const std::string relFile = "build/rel_del_test.txt";
            {
                std::ofstream out(relFile, std::ios::binary);
                out << "x";
            }
            check(std::ifstream(relFile).is_open(), "asset: relative test file exists");
            check(DeletePathRecursive(relFile),
                  "asset: delete works with a relative path");
            check(!std::ifstream(relFile).is_open(),
                  "asset: relative-path file actually removed");
        }
        // DeleteSelectedAsset end-to-end (the path the 删除 button / Delete
        // key / right-click menu use): select + delete removes the file.
        {
            const std::string tmpFile = assetDir_ + "/delete_me.txt";
            {
                std::ofstream out(tmpFile, std::ios::binary);
                out << "x";
            }
            RefreshAssetDir();
            selectedAsset_ = -1;
            for (size_t i = 0; i < assetEntries_.size(); ++i) {
                if (assetEntries_[i].name == "delete_me.txt") {
                    selectedAsset_ = static_cast<int>(i);
                    break;
                }
            }
            check(selectedAsset_ >= 0, "asset: delete target selected");
            DeleteSelectedAsset();
            check(!std::ifstream(tmpFile).is_open(),
                  "asset: DeleteSelectedAsset removes the selected file");
        }
        projectDir_ = prevProj;
        assetDir_ = prevDir;
    }

    // --- Material-ball assets (Unity .mat / Godot Material style) ---
    {
        const std::string proj = GetTempDir() + "/asset_proj";
        const std::string prevProj = projectDir_;
        const std::string prevAsset = assetDir_;
        projectDir_ = proj;
        assetDir_ = proj + "/assets";
        const size_t before = entities_.size();
        AddEntity("cube");
        const int idx = static_cast<int>(entities_.size()) - 1;
        SetSelection(idx);
        entities_[static_cast<size_t>(idx)].metallic = 0.42f;
        entities_[static_cast<size_t>(idx)].roughness = 0.31f;
        SaveMaterialAsset("smoke_mat");
        check(std::ifstream(proj + "/materials/smoke_mat.mat.json").is_open(),
              "material: SaveMaterialAsset writes the .mat.json");
        check(entities_[static_cast<size_t>(idx)].materialRef ==
                  "materials/smoke_mat.mat.json",
              "material: entity links the asset reference");
        {
            std::ofstream out(proj + "/materials/other.mat.json", std::ios::binary);
            out << R"({"colorHex":"#112233","metallic":0.9,"roughness":0.2})";
        }
        ApplyMaterialAsset(proj + "/materials/other.mat.json");
        SceneEntity& applied = entities_[static_cast<size_t>(idx)];
        check(applied.materialRef == "materials/other.mat.json" &&
                  std::fabs(applied.metallic - 0.9f) < 1e-5f &&
                  std::fabs(applied.roughness - 0.2f) < 1e-5f,
              "material: ApplyMaterialAsset updates the entity");
        // CJK material name: SaveMaterialAsset must write the file even when
        // the asset name is Chinese (the inspector's default is entity name).
        {
            SceneEntity& saveE = entities_[static_cast<size_t>(idx)];
            saveE.name = "\u519c\u820d_\u4e1c";
            SaveMaterialAsset("\u519c\u820d_\u4e1c");
            const std::string zhMat =
                proj + "/materials/\u519c\u820d_\u4e1c.mat.json";
            SceneEntity probe;
            check(LoadMaterialParamsInto(probe, zhMat),
                  "material: CJK-named material ball saved to disk");
        }
        RequestMaterialThumbnail(proj + "/materials/smoke_mat.mat.json");
        check(!materialThumbQueue_.empty(),
              "material: sphere preview queued for the material ball");
        // CJK filename: ifstream on Windows must still open the asset (the
        // realm project's material balls are Chinese-named).
        {
            const std::string zhFile =
                proj + "/materials/\u6d4b\u8bd5\u7403.mat.json";
            WriteFileUtf8(zhFile, R"({"colorHex":"#FF8800","metallic":0.5,"roughness":0.3})");
        }
        RequestMaterialThumbnail(proj + "/materials/\u6d4b\u8bd5\u7403.mat.json");
        // Scene export carries the reference; reloading expands it again.
        history_.Undo(); // remove the temp cube so later checks see the sandbox
        projectDir_ = prevProj;
        assetDir_ = prevAsset;
    }

    // --- Built-in script editor (open / save / syntax check) ---
    {
        const std::string path = GetTempDir() + "/editor_script.lua";
        {
            std::ofstream out(path, std::ios::binary);
            out << "function on_start(ent)\nend\n";
        }
        OpenScriptEditor(path);
        check(showScriptEditor_ && scriptEditorPath_ == path,
              "script editor: opens the file");
        check(scriptEditorCheck_.ok, "script editor: syntax passes on open");
        // Break the syntax, save, and expect the error to surface.
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_), "function broken( then\n");
        SaveScriptEditor();
        check(!scriptEditorCheck_.ok && !scriptEditorCheck_.message.empty(),
              "script editor: syntax error detected after save");
        // Fix and save again.
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_),
                      "function on_start(ent)\nend\n");
        SaveScriptEditor();
        check(scriptEditorCheck_.ok, "script editor: syntax passes after fix");
        std::ifstream verify(path, std::ios::binary);
        std::string saved((std::istreambuf_iterator<char>(verify)),
                          std::istreambuf_iterator<char>());
        check(saved.find("function on_start(ent)") != std::string::npos,
              "script editor: save writes the edited content");
    }

    // --- 2D sprite: the editor loader parses a componentized sprite entity,
    // resolves its texture into a quad mesh and keeps the flip flags ---
    {
        const std::string tmpScene = GetTempDir() + "/sprite_smoke.json";
        {
            std::ofstream out(tmpScene, std::ios::binary);
            out << R"({"entities":[{"name":"TreeSprite","components":{)"
                   R"("transform":{"pos":[1,2,0],"scale":[2,2,1]},)"
                   R"("sprite":{"texture":"projects/pvz/assets/sprites/bucket.png",)"
                   R"("flipX":true,"flipY":false,"colorHex":"#00ff88"}}}]})";
        }
        const std::string prevScene = currentScenePath_;
        LoadScene(tmpScene);
        bool spriteOk = false;
        for (const SceneEntity& e : entities_) {
            if (!e.spriteTex.empty() && e.spriteFlipX && !e.spriteFlipY &&
                e.spriteMesh.Valid() && e.spriteMaterial.albedo.Valid()) {
                spriteOk = true;
            }
        }
        NEON_LOG_INFO("EDITOR-SPRITE-SMOKE: [%s] sprite entity parsed, texture + quad resolved",
                      spriteOk ? "PASS" : "FAIL");
        if (!spriteOk) smokeFailed_ = true;
        if (!prevScene.empty()) LoadScene(prevScene);
    }
    // Restore the editor's actual scene (user data) after the deterministic
    // checks; later smoke frames (project switch / playtest) re-derive theirs.
    if (!smokePrevScene.empty()) LoadScene(smokePrevScene);

    NEON_LOG_INFO("EDITOR-UI-SMOKE: all checks done");
}

void EditorApp::AddEntity(const std::string& meshKey) {
    static int counter = 1;
    math::Vec3 pos = camTarget_ + math::Vec3{0, 1.0f, -3.0f};
    std::string name;
    if (meshKey.rfind("prefab:", 0) == 0) {
        // Instantiate a project prefab (prefabs/<name>.json): materialize its
        // component template into a new editable entity.
        const std::string pfName = meshKey.substr(7);
        auto tpl = prefabLib_.Get(pfName);
        if (!tpl.Ok()) {
            NEON_LOG_ERROR("Editor: prefab '%s' not found in '%s/prefabs'", pfName.c_str(),
                           projectDir_.c_str());
            return;
        }
        SceneEntity e;
        e.prefab = pfName;
        e.name = pfName + std::to_string(counter++);
        e.pos = pos;
        {
            // PrefabLibrary stores the component map directly (no wrapper).
            const core::Json* comps = tpl.Value();
            if (comps && comps->IsObject()) {
                if (const core::Json* m = comps->Get("mesh")) {
                    if (m->IsObject()) {
                        e.meshKey =
                            m->Get("meshKey") ? m->Get("meshKey")->GetString("cube") : "cube";
                        if (const core::Json* c = m->Get("colorHex"))
                            e.tint = ColorFromHex(c->GetString());
                        if (const core::Json* v = m->Get("metallic"))
                            e.metallic = static_cast<float>(v->GetNumber());
                        if (const core::Json* v = m->Get("roughness"))
                            e.roughness = static_cast<float>(v->GetNumber());
                    }
                }
                if (const core::Json* h = comps->Get("health")) {
                    if (h->IsObject()) {
                        if (const core::Json* v = h->Get("hp"))
                            e.hp = static_cast<float>(v->GetNumber());
                        if (const core::Json* v = h->Get("maxHp"))
                            e.maxHp = static_cast<float>(v->GetNumber());
                    }
                }
                if (const core::Json* s = comps->Get("script")) {
                    if (s->IsObject()) {
                        SceneScriptFields f;
                        f.path = s->Get("path") ? s->Get("path")->GetString() : "";
                        f.backend = s->Get("backend") ? s->Get("backend")->GetString("lua")
                                                      : "lua";
                        if (const core::Json* v = s->Get("vars")) f.vars = *v;
                        if (!f.path.empty()) e.scripts.push_back(std::move(f));
                    }
                }
                if (const core::Json* list = comps->Get("scripts")) {
                    if (const core::Json* items = list->Get("items")) {
                        if (items->IsArray()) {
                            for (const core::Json& it : items->Items()) {
                                SceneScriptFields f;
                                f.backend =
                                    it.Get("backend") ? it.Get("backend")->GetString("lua") : "lua";
                                f.path = it.Get("path") ? it.Get("path")->GetString() : "";
                                if (const core::Json* v = it.Get("vars")) f.vars = *v;
                                if (!f.path.empty()) e.scripts.push_back(std::move(f));
                            }
                        }
                    }
                }
                for (const auto& [cname, cdata] : comps->Members()) {
                    if (cname == "transform" || cname == "mesh" || cname == "health" ||
                        cname == "script")
                        continue;
                    e.extraComponents[cname] = cdata;
                }
            }
        }
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            const size_t insertAt = entities_.size();
            history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        return;
    }
    if (meshKey.rfind("obj:", 0) == 0 || meshKey.rfind("gltf:", 0) == 0) {
        std::string path = meshKey.substr(meshKey.find(':') + 1);
        size_t slash = path.find_last_of("/\\");
        size_t dot = path.find_last_of('.');
        size_t begin = slash == std::string::npos ? 0 : slash + 1;
        size_t len = (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = pos;
    if (meshKey == "tree") {
        e.scale = {1.6f, 1.6f, 1.6f};
    }
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
    }
}

void EditorApp::AddSpriteEntity(const std::string& texPath) {
    static int counter = 1;
    const std::string rel = ToProjectRelPath(texPath, projectDir_);
    SceneEntity e;
    e.name = BaseName(rel) + std::to_string(counter++);
    e.spriteTex = rel;
    // Spawn at the camera target on the front-ortho plane (z = 0), a visible
    // default size; the gizmo/inspector can move and scale it from there.
    e.pos = {camTarget_.x, camTarget_.y, 0.0f};
    e.scale = {2.0f, 2.0f, 1.0f};
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        NEON_LOG_INFO("Editor: sprite added '%s' (%s)", e.name.c_str(), e.spriteTex.c_str());
    }
}

core::Result<core::Json> EditorApp::BuildPlaySceneJson() {
    if (entities_.empty())
        return core::Result<core::Json>::Err("editor: scene is empty");
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        if (!e.spriteTex.empty()) {
            // 2D sprite: name + transform + sprite components (no mesh). The
            // runtime renders the image quad with the unlit texture material.
            auto mkStr = [](const std::string& s) {
                core::Json j;
                j.type_ = core::Json::Type::String;
                j.string_ = s;
                return j;
            };
            auto mkNum = [](double v) {
                core::Json j;
                j.type_ = core::Json::Type::Number;
                j.number_ = v;
                return j;
            };
            auto mkArr = [&mkNum](const std::initializer_list<double>& vals) {
                core::Json j;
                j.type_ = core::Json::Type::Array;
                for (double v : vals) j.array_.push_back(mkNum(v));
                return j;
            };
            obj.type_ = core::Json::Type::Object;
            obj.object_["name"] = mkStr(e.name);
            core::Json tf;
            tf.type_ = core::Json::Type::Object;
            tf.object_["pos"] = mkArr({e.pos.x, e.pos.y, e.pos.z});
            tf.object_["rot"] = mkArr({e.rot.x, e.rot.y, e.rot.z, e.rot.w});
            tf.object_["scale"] = mkArr({e.scale.x, e.scale.y, e.scale.z});
            if (!e.parent.empty()) tf.object_["parent"] = mkStr(e.parent);
            core::Json sp;
            sp.type_ = core::Json::Type::Object;
            sp.object_["texture"] = mkStr(e.spriteTex);
            if (e.spriteFlipX) {
                core::Json b;
                b.type_ = core::Json::Type::Bool;
                b.bool_ = true;
                sp.object_["flipX"] = std::move(b);
            }
            if (e.spriteFlipY) {
                core::Json b;
                b.type_ = core::Json::Type::Bool;
                b.bool_ = true;
                sp.object_["flipY"] = std::move(b);
            }
            sp.object_["colorHex"] = mkStr(ColorToHex(e.tint));
            core::Json comps;
            comps.type_ = core::Json::Type::Object;
            comps.object_["transform"] = std::move(tf);
            comps.object_["sprite"] = std::move(sp);
            obj.object_["components"] = std::move(comps);
        } else if (!e.meshKey.empty()) {
        std::string meshKey = ExportMeshKey(e.meshKey);
        if (e.meshKey == "npc") {
            // Encode the villager's tunic tint into the mesh key so the runtime
            // bakes it into the NPC mesh (its material tint stays white).
            char buf[48];
            std::snprintf(buf, sizeof(buf), "npc:%d,%d,%d",
                          static_cast<int>(e.tint.r * 255.0f),
                          static_cast<int>(e.tint.g * 255.0f),
                          static_cast<int>(e.tint.b * 255.0f));
            meshKey = buf;
        }
        auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale, meshKey,
                                                e.metallic, e.roughness, e.tint, e.albedoTex,
                                                e.mrTex, e.aoTex, e.emissiveTex, e.ao,
                                                e.emissiveIntensity, "", "", core::Json{}, {},
                                                e.hp, e.maxHp, e.parent);
        if (!res.Ok()) {
            return core::Result<core::Json>::Err("editor: " + res.Error());
        }
        obj = res.Value();
        if (!e.materialRef.empty()) {
            // Write the material-ball reference alongside the expanded params
            // (runtime reads the params; the editor keeps the asset link).
            core::Json& mesh = obj.object_["components"].object_["mesh"];
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = e.materialRef;
            mesh.object_["materialRef"] = std::move(j);
        }
        } else {
        // Logical entity (mesh renderer removed or never added): name +
        // transform + health only; scripts/extra components merge below.
        auto mkStr = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto mkNum = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto mkArr = [&mkNum](const std::initializer_list<double>& vals) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            for (double v : vals) j.array_.push_back(mkNum(v));
            return j;
        };
        obj.type_ = core::Json::Type::Object;
        obj.object_["name"] = mkStr(e.name);
        core::Json tf;
        tf.type_ = core::Json::Type::Object;
        tf.object_["pos"] = mkArr({e.pos.x, e.pos.y, e.pos.z});
        tf.object_["rot"] = mkArr({e.rot.x, e.rot.y, e.rot.z, e.rot.w});
        tf.object_["scale"] = mkArr({e.scale.x, e.scale.y, e.scale.z});
        if (!e.parent.empty()) tf.object_["parent"] = mkStr(e.parent);
        core::Json comps;
        comps.type_ = core::Json::Type::Object;
        comps.object_["transform"] = std::move(tf);
        if (e.maxHp > 0.0f) {
            core::Json health;
            health.type_ = core::Json::Type::Object;
            health.object_["hp"] = mkNum(e.hp);
            health.object_["maxHp"] = mkNum(e.maxHp);
            comps.object_["health"] = std::move(health);
        }
        obj.object_["components"] = std::move(comps);
        }
        if (!e.prefab.empty()) obj.object_["prefab"] = [&e]() {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = e.prefab;
            return j;
        }();
        // Merge schema-editable extra components into the exported entity so
        // project scenes round-trip without data loss.
        if (!e.extraComponents.empty()) {
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            for (const auto& [cname, cdata] : e.extraComponents)
                comps.object_[cname] = cdata;
            obj.object_["components"] = std::move(comps);
        }
        // Mounted scripts: one flat "scripts" component [{backend,path,vars},
        // ...]. Every entry is equal; the runtime attaches each in order.
        if (!e.scripts.empty()) {
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            auto mkStr2 = [](const std::string& v) {
                core::Json j;
                j.type_ = core::Json::Type::String;
                j.string_ = v;
                return j;
            };
            core::Json items;
            items.type_ = core::Json::Type::Array;
            for (const SceneScriptFields& f : e.scripts) {
                if (f.path.empty()) continue; // unconfigured script block
                core::Json it;
                it.type_ = core::Json::Type::Object;
                it.object_["backend"] = mkStr2(f.backend.empty() ? "lua" : f.backend);
                it.object_["path"] = mkStr2(f.path);
                if (f.vars.IsObject()) it.object_["vars"] = f.vars;
                items.array_.push_back(std::move(it));
            }
            core::Json scripts;
            scripts.type_ = core::Json::Type::Object;
            scripts.object_["items"] = std::move(items);
            comps.object_["scripts"] = std::move(scripts);
            obj.object_["components"] = std::move(comps);
        }
        arr.array_.push_back(std::move(obj));
    }
    root.object_["entities"] = std::move(arr);
    return core::Result<core::Json>::Ok(std::move(root));
}

void EditorApp::StartPlaytest() {
    StopPlaytest(); // restart semantics: a fresh snapshot each time

    scene::GameRuntimeConfig cfg;
    cfg.assets = &assetMgr_;
#ifdef NEON_ENABLE_JOLT
    cfg.physicsBackend = "jolt"; // playtest uses Jolt rigid bodies when compiled
#endif
    cfg.scriptBaseDir = projectDir_.empty() ? "." : projectDir_;
    cfg.localesDir = projectDir_.empty() ? "./locales" : projectDir_ + "/locales";
    cfg.input = Input(); // hero controller reads live WASD/mouse input
    cfg.font2d = cjkFont_.Valid() ? cjkFont_ : pixelFont_; // 2D HUD / on_render
    // PlaySfx(name) from game scripts routes to the procedural synth.
    cfg.playSfx = [this](const std::string& name) {
        if (audioBackend_) audioBackend_->Play(MakePvzSfx(name), 0.7f);
    };
    std::string json;

    if (projectMode_ == "2d") {
        // 2D project (NeonPvZ / NeonSnake): play the project's start scene
        // directly - the scene file is the single source of truth (there is
        // no separate editor canvas copy). The runtime's on_render draws the
        // actual game. Follows the PROJECT type, not the current camera view:
        // a 2D game plays as a 2D game even in a perspective editor camera.
        cfg.assetBaseDir = projectDir_;
        const std::string sceneRel =
            projectStartScene_.empty() ? "scenes/pvz.json" : projectStartScene_;
        const std::string scenePath = projectDir_ + "/" + sceneRel;
        std::ifstream in(scenePath, std::ios::binary);
        if (!in.is_open()) {
            NEON_LOG_ERROR("Editor: cannot read play scene '%s'", scenePath.c_str());
            return;
        }
        json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        // 3D scene: play the editor's current entities (serialized snapshot).
        if (entities_.empty()) {
            NEON_LOG_WARN("Editor: nothing to play (scene is empty)");
            return;
        }
        auto root = BuildPlaySceneJson();
        if (!root.Ok()) {
            NEON_LOG_ERROR("Editor: cannot build play scene: %s", root.Error().c_str());
            return;
        }
        json = core::JsonWriter::Write(root.Value());
    }

    playtest_ = std::make_unique<scene::GameRuntime>();
    core::Status st = playtest_->Start(json, cfg);
    if (!st.Ok()) {
        NEON_LOG_ERROR("Editor: playtest failed to start: %s", st.Error().c_str());
        playtest_.reset();
        return;
    }
    playtestActive_ = true;
    // Detach the input method while the playtest runs so game keys (WASD,
    // digits, space) arrive as raw key events even with a Chinese/Japanese IME
    // in composition mode; StopPlaytest re-attaches it for ImGui text input.
    if (Window()) Window()->SetImeEnabled(false);
    NEON_LOG_INFO("Editor: playtest started (%zu entities, %zu scripts, %zu trees)",
                  playtest_->EntityCount(), playtest_->ScriptCount(),
                  playtest_->BehaviorTreeCount());
}

void EditorApp::StopPlaytest() {
    if (!playtest_) return;
    playtest_->Stop();
    playtest_.reset();
    playtestActive_ = false;
    if (Window()) Window()->SetImeEnabled(true);
    NEON_LOG_INFO("Editor: playtest stopped");
}

void EditorApp::TogglePlaytest() {
    if (playtestActive_) {
        StopPlaytest();
    } else {
        StartPlaytest();
    }
}

void EditorApp::RequestMeshThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = meshThumbs_.find(path);
    if (it != meshThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(meshThumbQueue_.begin(), meshThumbQueue_.end(), path) ==
        meshThumbQueue_.end()) {
        meshThumbQueue_.push_back(path);
    }
}

void EditorApp::GenerateMeshThumbnails() {
    if (meshThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        meshThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    // A thumbnail is tooling, not scene geometry: never record its mesh as a
    // shadow caster for the main scene's next shadow pass.
    renderer_.SetShadowRecording(false);
    for (const std::string& path : meshThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = meshThumbs_.find(path);
        if (it != meshThumbs_.end() && it->second.mtime == m) continue; // already fresh

        // Resolve the asset's first mesh; a failed load caches a "miss" (same
        // mtime) so the panel only retries when the file actually changes.
        const std::string ext = ExtLower(path);
        gfx::Mesh mesh;
        gfx::Material mat = gfx::Material::Lit({}, gfx::Color{0.85f, 0.85f, 0.92f, 1.0f}, 16.0f);
        if (ext == ".obj") {
            mesh = assetMgr_.LoadMeshOBJ(path);
        } else if (ext == ".gltf") {
            assets::GltfAsset gltf = assetMgr_.LoadGLTF(path);
            if (!gltf.nodes.empty()) {
                mesh = gltf.nodes[0].mesh;
                mat = gltf.nodes[0].material;
            }
        }
        if (!mesh.Valid()) {
            if (it != meshThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                meshThumbs_.erase(it);
            }
            meshThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }

        // Orthographic front camera framing the mesh's bounds.
        const math::AABB& b = mesh.Bounds();
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const math::Vec3 extents = b.max - b.min;
        const float size = std::max({extents.x, extents.y, extents.z, 0.001f}) * 1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        // RGBA16F so the target carries a depth attachment (a plain RGBA8
        // target has none); the lit mesh then occludes correctly.
        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt); // sets the 96x96 viewport
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        renderer_.DrawMesh(mesh, mat, math::Mat4::Identity());
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);

        if (it != meshThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            meshThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        meshThumbs_[path] = nt;
    }
    meshThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    // Leave the backbuffer bound + the viewport at window size for the ImGui
    // pass (EndScene already composited to it).
    backend->BindDefaultTarget();
}

// Queues a material-ball sphere preview (mtime-gated, like mesh thumbnails).
void EditorApp::RequestMaterialThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = materialThumbs_.find(path);
    if (it != materialThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(materialThumbQueue_.begin(), materialThumbQueue_.end(), path) ==
        materialThumbQueue_.end()) {
        materialThumbQueue_.push_back(path);
    }
}

// Renders each queued material ball as a lit sphere (Unity/UE-style preview)
// into a small offscreen target; the ImGui pass samples it next frame.
void EditorApp::GenerateMaterialThumbnails() {
    if (materialThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        materialThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    renderer_.SetShadowRecording(false);
    for (const std::string& path : materialThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = materialThumbs_.find(path);
        if (it != materialThumbs_.end() && it->second.mtime == m) continue;

        // Expand the material asset into entity-style params, then build a
        // PBR material from them (texture slots resolve through the cache).
        SceneEntity params;
        if (!LoadMaterialParamsInto(params, path)) {
            // Cache a miss so the panel only retries when the file changes.
            if (it != materialThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                materialThumbs_.erase(it);
            }
            materialThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }
        gfx::Material mat = gfx::Material::Lit({}, params.tint, 8.0f);
        mat.metallic = params.metallic;
        mat.roughness = params.roughness;
        mat.aoStrength = params.ao;
        mat.emissiveIntensity = params.emissiveIntensity;
        if (!params.albedoTex.empty())
            mat.albedo = assetMgr_.LoadTexture(params.albedoTex).Handle();
        if (!params.mrTex.empty())
            mat.metallicRoughness = assetMgr_.LoadTexture(params.mrTex).Handle();
        if (!params.aoTex.empty())
            mat.occlusion = assetMgr_.LoadTexture(params.aoTex).Handle();
        if (!params.emissiveTex.empty())
            mat.emissive = assetMgr_.LoadTexture(params.emissiveTex).Handle();

        gfx::Mesh sphere = gfx::Mesh::CreateSphere(renderer_, 0.8f, 16, 12, "matball");
        const math::AABB& b = sphere.Bounds();
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const float size = std::max({b.max.x - b.min.x, b.max.y - b.min.y,
                                     b.max.z - b.min.z, 0.001f}) *
                           1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt);
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        renderer_.DrawMesh(sphere, mat, math::Mat4::Identity());
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);

        if (it != materialThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            materialThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        materialThumbs_[path] = nt;
    }
    materialThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    backend->BindDefaultTarget();
}

void EditorApp::PollHotReload() {
    const std::string base = projectDir_.empty() ? "." : projectDir_;

    // Scripts: only while a playtest runs (that is what executes scripts). A
    // changed *.lua under <projectDir>/scripts/ is applied as a playtest
    // restart (Stop + Start), which resets all script/entity/BT state - a safe,
    // deterministic reload for the editor. Shaders are compiled from strings
    // at init and are deliberately NOT hot-reloaded (YAGNI; see T4.8 notes).
    if (playtestActive_ && playtest_) {
        std::vector<std::string> files;
        ListLuaFiles(ScriptsDir(projectDir_), "scripts", files);
        bool scriptChanged = false;
        for (const std::string& rel : files) {
            const std::string full = base + "/" + rel;
            const uint64_t m = FileMTime(full);
            auto it = scriptMtimes_.find(full);
            if (it != scriptMtimes_.end() && it->second != m) {
                scriptChanged = true;
                break;
            }
            scriptMtimes_[full] = m;
        }
        if (scriptChanged) {
            ++hotReloadCount_;
            NEON_LOG_INFO(
                "Editor: hot reload: a script changed on disk -> restarting playtest "
                "(play state resets)");
            StopPlaytest();
            StartPlaytest();
        }
    }

    // Assets referenced by the editor scene: textures + file-backed meshes,
    // including the file-backed built-in mesh keys (helmet/tree resolve to
    // files via ResolveMesh). glTF is re-parsed by ResolveMesh on every call,
    // so a change only needs the mtime gate here; OBJ/textures drop through
    // the AssetManager cache.
    std::set<std::string> changedPaths;
    auto checkFile = [&](const std::string& path) {
        if (path.empty()) return;
        const uint64_t m = FileMTime(path);
        auto it = assetMtimes_.find(path);
        if (it != assetMtimes_.end() && it->second != m && m != 0) changedPaths.insert(path);
        assetMtimes_[path] = m;
    };
    for (const SceneEntity& e : entities_) {
        const std::string meshPath = MeshKeyAssetPath(e.meshKey);
        if (!meshPath.empty()) checkFile(meshPath);
        checkFile(e.albedoTex);
        checkFile(e.mrTex);
        checkFile(e.aoTex);
        checkFile(e.emissiveTex);
    }
    if (!changedPaths.empty()) {
        for (const std::string& p : changedPaths) {
            const std::string ext = ExtLower(p);
            if (ext == ".obj") assetMgr_.ReloadMeshOBJ(p);
            else if (ext != ".gltf") assetMgr_.ReloadTexture(p);
            NEON_LOG_INFO("Editor: hot reload: asset '%s' reloaded", p.c_str());
        }
        ++hotReloadCount_;
        // Re-resolve only the entities that reference a changed asset.
        for (SceneEntity& e : entities_) {
            const bool touches =
                changedPaths.count(MeshKeyAssetPath(e.meshKey)) ||
                changedPaths.count(e.albedoTex) || changedPaths.count(e.mrTex) ||
                changedPaths.count(e.aoTex) || changedPaths.count(e.emissiveTex);
            if (touches) {
                ResolveMesh(e);
                ApplyMaterialParams(e);
            }
        }
    }
}

core::Status EditorApp::ExportScene() {
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Editor: export aborted: %s", rootRes.Error().c_str());
        return core::Status::Err(rootRes.Error());
    }
    core::Json root = rootRes.Value();

    std::string base = projectDir_.empty() ? "." : projectDir_;
    std::string scenesDir = base + "/scenes";
    if (!EnsureDirs(scenesDir)) {
        NEON_LOG_ERROR("Editor: cannot create export directory '%s'", scenesDir.c_str());
        return core::Status::Err("editor: cannot create export directory '" + scenesDir + "'");
    }
    std::string path = scenesDir + "/exported_scene.json";
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out(path); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: exported scene (%zu entities) -> %s", entities_.size(),
                      path.c_str());
        return core::Status::Ok(true);
    }
    NEON_LOG_ERROR("Editor: cannot write '%s'", path.c_str());
    return core::Status::Err("editor: cannot write '" + path + "'");
}

void EditorApp::LoadEditorConfig() {
    projectDir_ = ".";
    std::ifstream in("neon_editor_config.json");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        core::Json root = core::Json::Parse(ss.str(), &err);
        if (root.IsObject()) {
            if (const core::Json* p = root.Get("projectDir")) projectDir_ = p->GetString();
        }
    }
    if (projectDir_.empty()) projectDir_ = ".";
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
}

void EditorApp::SaveEditorConfig() {
    if (projectDir_.empty()) projectDir_ = ".";
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json p;
    p.type_ = core::Json::Type::String;
    p.string_ = projectDir_;
    root.object_["projectDir"] = p;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("neon_editor_config.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: config saved (project dir '%s')", projectDir_.c_str());
    } else {
        NEON_LOG_WARN("Editor: cannot write editor config");
    }
}

bool EditorApp::ResolveMesh(SceneEntity& e) {
    const std::string& key = e.meshKey;
    if (key == "terrain") {
        e.mesh = gfx::MakeTerrainMesh(renderer_);
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "helmet") {
        assets::GltfAsset gltf =
            assetMgr_.LoadGLTF("assets/models/DamagedHelmet/DamagedHelmet.gltf");
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    } else if (key == "cube") {
        e.mesh = gfx::Mesh::CreateCube(renderer_, 1, 1, 1, "cube");
        e.material = gfx::Material::Lit({}, e.tint, 12.0f);
    } else if (key == "tree") {
        e.mesh = gfx::MakeTreeMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "house") {
        e.mesh = gfx::MakeHouseMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "npc" || key.compare(0, 4, "npc:") == 0) {
        // The entity tint selects the villager's tunic; head stays skin-tone.
        if (key.compare(0, 4, "npc:") == 0) {
            // "npc:r,g,b" encodes the tunic tint; decode it onto e.tint.
            int r = 128, g = 128, b = 128;
            std::sscanf(key.c_str() + 4, "%d,%d,%d", &r, &g, &b);
            e.tint = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        }
        e.mesh = gfx::MakeNPCMesh(renderer_, {e.tint.r, e.tint.g, e.tint.b, 1.0f});
        e.material = gfx::Material::Lit({}, gfx::Color::White, 12.0f);
    } else if (key == "hero") {
        e.mesh = gfx::MakeHeroMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 12.0f);
    } else if (key == "wolf") {
        e.mesh = gfx::MakeWolfMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "bush") {
        e.mesh = gfx::MakeBushMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "rock") {
        e.mesh = gfx::Mesh::CreateSphere(renderer_, 0.8f, 10, 7, "rock");
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "water") {
        e.mesh = gfx::Mesh::CreatePlane(renderer_, 20.0f, 20.0f, 8, 8, "water");
        e.material = gfx::Material::Lit({}, e.tint, 64.0f);
    } else if (key == "road") {
        e.mesh = gfx::Mesh::CreatePlane(renderer_, 1.0f, 1.0f, 1, 1, "road");
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key.rfind("obj:", 0) == 0) {
        e.mesh = assetMgr_.LoadMeshOBJ(key.substr(4));
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("gltf:", 0) == 0) {
        assets::GltfAsset gltf = assetMgr_.LoadGLTF(key.substr(5));
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
            // glTF materials carry their own PBR params (factors + texture
            // slots); sync them into the flattened fields so ApplyMaterialParams
            // applies the asset's values instead of the editor defaults.
            e.metallic = e.material.metallic;
            e.roughness = e.material.roughness;
            e.ao = e.material.aoStrength;
            e.emissiveIntensity = e.material.emissiveIntensity;
            e.tint = e.material.tint;
        }
    } else if (key.empty()) {
        if (!e.spriteTex.empty()) {
            // 2D sprite: image texture on an XY quad (facing the front-ortho
            // camera) rendered with an unlit material so colors are exactly
            // the texture's.
            // Sprite paths are stored project-relative ("assets/sprites/x.png"),
            // so resolve them against the project dir first (fall back to the
            // raw path for absolute paths and the repo-wide assets/ folder.
            // The default sandbox (projectDir_ == ".") can hold sprites dragged
            // in from any bundled project, so also probe every projects/*/.
            std::string texPath = e.spriteTex;
            const bool absolute = texPath.size() >= 2 && texPath[1] == ':' ||
                                  (!texPath.empty() &&
                                   (texPath[0] == '/' || texPath[0] == '\\'));
            if (!absolute) {
                auto exists = [](const std::string& f) {
                    std::ifstream probe(f, std::ios::binary);
                    return probe.is_open();
                };
                if (projectDir_ != "." && exists(projectDir_ + "/" + texPath)) {
                    texPath = projectDir_ + "/" + texPath;
                } else {
                    std::vector<AssetEntry> projDirs;
                    if (ListDirectory("projects", projDirs)) {
                        for (const AssetEntry& d : projDirs) {
                            if (!d.isDir) continue;
                            const std::string cand = d.path + "/" + texPath;
                            if (exists(cand)) {
                                texPath = cand;
                                break;
                            }
                        }
                    }
                }
            }
            gfx::Texture tex = assetMgr_.LoadTexture(texPath);
            if (!tex.Valid()) {
                NEON_LOG_ERROR("Editor: sprite texture '%s' failed to load", texPath.c_str());
                return false;
            }
            e.spriteMesh = gfx::Mesh::CreateQuad(renderer_, 1.0f, 1.0f, "sprite");
            e.spriteMaterial = gfx::Material::Unlit(tex.Handle(), e.tint);
            e.spriteMaterial.transparent = true; // PNG sprites keep their alpha
        }
        // Script-only / logical entities (e.g. a 2D game's entry entity that
        // carries no mesh) are valid without geometry.
        return true;
    }
    return e.mesh.Valid();
}

void EditorApp::ApplyMaterialParams(SceneEntity& e) {
    if (!e.spriteTex.empty()) {
        // Sprite tint follows the entity color (unlit material, so the color
        // tints the texture exactly like a 2D sprite's modulate color).
        e.spriteMaterial.tint = e.tint;
        return;
    }
    // Props that bake colors into vertex data keep a white material tint (for
    // "npc" the entity tint already selected the tunic at mesh-build time).
    e.material.tint = IsBakedColorKey(e.meshKey) ? gfx::Color::White : e.tint;
    e.material.metallic = e.metallic;
    e.material.roughness = e.roughness;
    e.material.aoStrength = e.ao;
    e.material.emissiveIntensity = e.emissiveIntensity;
    // Texture slots: load any non-empty path through the cached AssetManager.
    // Empty paths leave the existing handle untouched (e.g. a glTF material's
    // baked PBR textures survive until the user explicitly overrides/clears).
    if (!e.albedoTex.empty()) e.material.albedo = assetMgr_.LoadTexture(e.albedoTex).Handle();
    if (!e.mrTex.empty()) e.material.metallicRoughness = assetMgr_.LoadTexture(e.mrTex).Handle();
    if (!e.aoTex.empty()) e.material.occlusion = assetMgr_.LoadTexture(e.aoTex).Handle();
    if (!e.emissiveTex.empty()) e.material.emissive = assetMgr_.LoadTexture(e.emissiveTex).Handle();
}

void EditorApp::SaveScene() {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    if (!sceneExtends_.empty()) {
        core::Json ex;
        ex.type_ = core::Json::Type::String;
        ex.string_ = sceneExtends_;
        root.object_["extends"] = ex;
    }
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        obj.type_ = core::Json::Type::Object;
        core::Json name;
        name.type_ = core::Json::Type::String;
        name.string_ = e.name;
        obj.object_["name"] = name;
        auto str = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto num = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto vec3 = [&](const math::Vec3& v) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            j.array_ = {num(v.x), num(v.y), num(v.z)};
            return j;
        };
        obj.object_["mesh"] = str(e.meshKey);
        if (!e.spriteTex.empty()) obj.object_["spriteTex"] = str(e.spriteTex);
        if (e.spriteFlipX) obj.object_["spriteFlipX"] = num(1);
        if (e.spriteFlipY) obj.object_["spriteFlipY"] = num(1);
        if (!e.parent.empty()) obj.object_["parent"] = str(e.parent);
        obj.object_["pos"] = vec3(e.pos);
        obj.object_["scale"] = vec3(e.scale);
        core::Json tint;
        tint.type_ = core::Json::Type::Array;
        tint.array_ = {num(e.tint.r), num(e.tint.g), num(e.tint.b)};
        obj.object_["tint"] = tint;
        obj.object_["metallic"] = num(e.metallic);
        obj.object_["roughness"] = num(e.roughness);
        obj.object_["ao"] = num(e.ao);
        obj.object_["emissiveIntensity"] = num(e.emissiveIntensity);
        obj.object_["albedoTex"] = str(e.albedoTex);
        obj.object_["mrTex"] = str(e.mrTex);
        obj.object_["aoTex"] = str(e.aoTex);
        obj.object_["emissiveTex"] = str(e.emissiveTex);
        if (!e.scripts.empty()) {
            core::Json scriptsArr;
            scriptsArr.type_ = core::Json::Type::Array;
            for (const SceneScriptFields& f : e.scripts) {
                if (f.path.empty()) continue; // unconfigured script block
                core::Json it;
                it.type_ = core::Json::Type::Object;
                it.object_["backend"] = str(f.backend.empty() ? "lua" : f.backend);
                it.object_["path"] = str(f.path);
                if (f.vars.IsObject()) it.object_["vars"] = f.vars;
                scriptsArr.array_.push_back(std::move(it));
            }
            obj.object_["scripts"] = std::move(scriptsArr);
        }
        arr.array_.push_back(obj);
    }
    root.object_["entities"] = arr;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("editor_scene.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Scene saved (%zu entities)", entities_.size());
    }
}

// P1-1: writes a copy of the current scene as <dir>/<stem>_child.json with
// "extends" pointing at the current scene, then opens it. The child loads with
// the parent's entities underneath, so parent edits propagate and child
// same-name entities override.
void EditorApp::SaveSceneAsChild() {
    if (currentScenePath_.empty()) {
        NEON_LOG_WARN("Editor: 另存为子场景 needs a loaded scene file");
        return;
    }
    std::ifstream in(currentScenePath_, std::ios::binary);
    if (!in.is_open()) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string perr;
    core::Json root = core::Json::Parse(text, &perr);
    if (!root.IsObject()) {
        NEON_LOG_ERROR("Editor: cannot save child scene (parse error: %s)", perr.c_str());
        return;
    }
    const size_t slash = currentScenePath_.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "" : currentScenePath_.substr(0, slash + 1);
    const std::string base =
        slash == std::string::npos ? currentScenePath_ : currentScenePath_.substr(slash + 1);
    const size_t dot = base.rfind('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    core::Json ex;
    ex.type_ = core::Json::Type::String;
    ex.string_ = currentScenePath_;
    root.object_["extends"] = ex;
    const std::string childPath = dir + stem + "_child.json";
    if (std::ofstream out(childPath, std::ios::binary); out.is_open()) {
        out << core::JsonWriter::Write(root);
        NEON_LOG_INFO("Editor: child scene saved -> %s (extends %s)", childPath.c_str(),
                      currentScenePath_.c_str());
        LoadScene(childPath);
    }
}

// P1-1 scene inheritance: parent entities first; a child entity with the same
// name replaces the parent's entry (keeping its position), new names append.
// gameVars / level: the child wins when present.
static core::Json MergeSceneJson(const core::Json& parent, const core::Json& child) {
    core::Json out = parent;
    if (const core::Json* gv = child.Get("gameVars")) out.object_["gameVars"] = *gv;
    if (const core::Json* lv = child.Get("level")) out.object_["level"] = *lv;
    std::vector<core::Json> merged;
    if (const core::Json* pents = parent.Get("entities")) {
        if (pents->IsArray())
            for (const core::Json& e : pents->Items()) merged.push_back(e);
    }
    if (const core::Json* cents = child.Get("entities")) {
        if (cents->IsArray()) {
            for (const core::Json& c : cents->Items()) {
                const std::string cname =
                    c.Get("name") ? c.Get("name")->GetString("") : std::string();
                bool replaced = false;
                for (core::Json& e : merged) {
                    const std::string ename =
                        e.Get("name") ? e.Get("name")->GetString("") : std::string();
                    // Same-name entities only override when the child differs
                    // (a full-copy child inherits identical entities from the
                    // parent, so parent edits propagate to the child).
                    if (!cname.empty() && ename == cname &&
                        core::JsonWriter::Write(c) != core::JsonWriter::Write(e)) {
                        e = c;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) merged.push_back(c);
            }
        }
    }
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    arr.array_ = std::move(merged);
    out.object_["entities"] = std::move(arr);
    return out;
}

void EditorApp::LoadScene(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    core::Json root = core::Json::Parse(ss.str(), &err);
    // P1-1 scene inheritance: resolve "extends" chains by loading the parent
    // file(s) and overlaying same-named entities (child wins, new names
    // append). The parent path is kept so SaveScene writes it back.
    sceneExtends_.clear();
    if (const core::Json* ex = root.Get("extends")) {
        if (ex->IsString() && !ex->GetString().empty()) {
            const std::string parentPath = ex->GetString();
            sceneExtends_ = parentPath;
            std::ifstream pin(parentPath);
            if (pin.is_open()) {
                std::stringstream pss;
                pss << pin.rdbuf();
                core::Json parent = core::Json::Parse(pss.str(), &err);
                if (parent.IsObject() && parent.Get("entities")) {
                    // Recursively resolve the parent's own inheritance first.
                    std::ifstream prein(parentPath);
                    (void)prein;
                    if (const core::Json* pex = parent.Get("extends")) {
                        if (pex->IsString() && !pex->GetString().empty()) {
                            std::ifstream pin2(pex->GetString());
                            if (pin2.is_open()) {
                                std::stringstream pss2;
                                pss2 << pin2.rdbuf();
                                core::Json grand = core::Json::Parse(pss2.str(), &err);
                                if (grand.IsObject() && grand.Get("entities")) {
                                    parent = MergeSceneJson(grand, parent);
                                    parent.object_.erase("extends");
                                }
                            }
                        }
                    }
                    parent.object_.erase("extends");
                    root = MergeSceneJson(parent, root);
                }
            }
            root.object_.erase("extends");
        }
    }
    const core::Json* arr = root.Get("entities");
    if (!arr) return;
    // Keep the parsed scene root + path: 2D levels live inside the scene as
    // plant/zombie ENTITIES in the scene file, so scenes are the single
    // source of truth for both 3D and 2D.
    currentSceneRoot_ = root;
    currentScenePath_ = path;
    pvzPlants_.clear();
    pvzZombies_.clear();
    // Replace entity list, re-resolve meshes.
    std::vector<SceneEntity> loaded;
    bool has2DData = false; // any plant/zombie entity -> a 2D level scene
    // Support both the editor's flat format and the runtime's componentized
    // SceneFile format ("components": {transform/mesh/health/script}) so a
    // data-driven project scene (e.g. projects/neon_realm) opens directly.
    const bool componentized =
        arr->Size() > 0 && arr->At(0) != nullptr && arr->At(0)->Get("components") != nullptr;
    for (size_t i = 0; i < arr->Size(); ++i) {
        const core::Json* j = arr->At(i);
        if (!j) continue;
        SceneEntity e;
        e.name = j->Get("name")->GetString("entity");
        if (componentized) {
            if (const core::Json* pf = j->Get("prefab")) e.prefab = pf->GetString();
            // Effective components = prefab template merged with instance
            // overrides (instance fields win), mirroring the runtime.
            core::Json effective;
            effective.type_ = core::Json::Type::Object;
            if (!e.prefab.empty() && prefabLib_.Has(e.prefab)) {
                auto tpl = prefabLib_.Get(e.prefab);
                if (tpl.Ok()) {
                    // PrefabLibrary stores the component map directly.
                    const core::Json* tc = tpl.Value();
                    if (tc && tc->IsObject()) effective = *tc;
                }
            }
            if (const core::Json* inst = j->Get("components")) {
                if (inst->IsObject()) {
                    for (const auto& [k, v] : inst->Members()) effective.object_[k] = v;
                }
            }
            const core::Json* comps = &effective;
            if (const core::Json* t = comps->Get("transform")) {
                if (const core::Json* p = t->Get("pos"))
                    e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                             static_cast<float>(p->At(1)->GetNumber()),
                             static_cast<float>(p->At(2)->GetNumber())};
                if (const core::Json* s = t->Get("scale"))
                    e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                               static_cast<float>(s->At(1)->GetNumber()),
                               static_cast<float>(s->At(2)->GetNumber())};
                if (const core::Json* p = t->Get("parent")) e.parent = p->GetString();
            }
            if (const core::Json* m = comps->Get("mesh")) {
                e.meshKey = m->Get("meshKey") ? m->Get("meshKey")->GetString("cube") : "cube";
                if (const core::Json* mr = m->Get("materialRef"))
                    e.materialRef = mr->GetString();
                if (const core::Json* c = m->Get("colorHex")) e.tint = ColorFromHex(c->GetString());
                if (const core::Json* v = m->Get("metallic")) e.metallic = static_cast<float>(v->GetNumber());
                if (const core::Json* v = m->Get("roughness")) e.roughness = static_cast<float>(v->GetNumber());
                if (const core::Json* v = m->Get("ao")) e.ao = static_cast<float>(v->GetNumber());
                if (const core::Json* v = m->Get("emissiveIntensity")) e.emissiveIntensity = static_cast<float>(v->GetNumber());
                if (const core::Json* v = m->Get("albedoTex")) e.albedoTex = v->GetString();
                if (const core::Json* v = m->Get("mrTex")) e.mrTex = v->GetString();
                if (const core::Json* v = m->Get("aoTex")) e.aoTex = v->GetString();
                if (const core::Json* v = m->Get("emissiveTex")) e.emissiveTex = v->GetString();
            }
            if (const core::Json* sp = comps->Get("sprite")) {
                e.spriteTex = sp->Get("texture") ? sp->Get("texture")->GetString() : "";
                if (const core::Json* fx = sp->Get("flipX")) e.spriteFlipX = fx->GetBool();
                if (const core::Json* fy = sp->Get("flipY")) e.spriteFlipY = fy->GetBool();
                if (const core::Json* c = sp->Get("colorHex")) e.tint = ColorFromHex(c->GetString());
            }
            if (const core::Json* h = comps->Get("health")) {
                if (const core::Json* v = h->Get("hp")) e.hp = static_cast<float>(v->GetNumber());
                if (const core::Json* v = h->Get("maxHp")) e.maxHp = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* s = comps->Get("script")) {
                // Legacy single "script" component: one mounted script.
                if (s->IsObject()) {
                    SceneScriptFields f;
                    f.path = s->Get("path") ? s->Get("path")->GetString() : "";
                    f.backend = s->Get("backend") ? s->Get("backend")->GetString("lua") : "lua";
                    if (const core::Json* v = s->Get("vars")) f.vars = *v;
                    if (!f.path.empty()) e.scripts.push_back(std::move(f));
                }
            }
            if (const core::Json* list = comps->Get("scripts")) {
                if (const core::Json* items = list->Get("items")) {
                    if (items->IsArray()) {
                        for (const core::Json& it : items->Items()) {
                            SceneScriptFields f;
                            f.backend = it.Get("backend") ? it.Get("backend")->GetString("lua")
                                                          : "lua";
                            f.path = it.Get("path") ? it.Get("path")->GetString() : "";
                            if (const core::Json* v = it.Get("vars")) f.vars = *v;
                            if (!f.path.empty()) e.scripts.push_back(std::move(f));
                        }
                    }
                }
            }
            // Keep every non-flattened component as editable extra data
            // (schema-driven inspector; plant/zombie mirror the 2D canvas).
            for (const auto& [cname, cdata] : comps->Members()) {
                if (cname == "transform" || cname == "mesh" || cname == "health" ||
                    cname == "script" || cname == "sprite")
                    continue;
                e.extraComponents[cname] = cdata;
            }
            if (const core::Json* pl = comps->Get("plant")) {
                if (pl->IsObject()) {
                    const int row = pl->Get("row") ? pl->Get("row")->GetInt(-1) : -1;
                    const int col = pl->Get("col") ? pl->Get("col")->GetInt(-1) : -1;
                    const std::string name =
                        pl->Get("type") ? pl->Get("type")->GetString("sunflower") : "sunflower";
                    int type = -1;
                    for (int t = 0; t < 5; ++t)
                        if (name == kPvzPlantNames[t]) type = t;
                    if (row >= 0 && row < kPvzRows && col >= 0 && col < kPvzCols && type >= 0) {
                        pvzPlants_.push_back({row, col, type});
                        has2DData = true;
                    }
                }
            }
            if (const core::Json* zb = comps->Get("zombie")) {
                if (zb->IsObject()) {
                    const int row = zb->Get("row") ? zb->Get("row")->GetInt(-1) : -1;
                    const float delay =
                        zb->Get("delay")
                            ? static_cast<float>(zb->Get("delay")->GetNumber())
                            : 8.0f;
                    const std::string name =
                        zb->Get("type") ? zb->Get("type")->GetString("basic") : "basic";
                    int type = 0;
                    for (int t = 0; t < 3; ++t)
                        if (name == kPvzZombieNames[t]) type = t;
                    if (row >= 0 && row < kPvzRows) {
                        pvzZombies_.push_back({row, delay, type});
                        has2DData = true;
                    }
                }
            }
        } else {
            if (const core::Json* p = j->Get("parent")) e.parent = p->GetString();
            e.meshKey = j->Get("mesh")->GetString("cube");
            if (const core::Json* st = j->Get("spriteTex")) e.spriteTex = st->GetString();
            if (const core::Json* fx = j->Get("spriteFlipX")) e.spriteFlipX = fx->GetInt(0) != 0;
            if (const core::Json* fy = j->Get("spriteFlipY")) e.spriteFlipY = fy->GetInt(0) != 0;
            if (const core::Json* p = j->Get("pos")) {
                e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                         static_cast<float>(p->At(1)->GetNumber()),
                         static_cast<float>(p->At(2)->GetNumber())};
            }
            if (const core::Json* s = j->Get("scale")) {
                e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                           static_cast<float>(s->At(1)->GetNumber()),
                           static_cast<float>(s->At(2)->GetNumber())};
            }
            if (const core::Json* t = j->Get("tint")) {
                e.tint = {static_cast<float>(t->At(0)->GetNumber()),
                          static_cast<float>(t->At(1)->GetNumber()),
                          static_cast<float>(t->At(2)->GetNumber()), 1.0f};
            }
            if (const core::Json* m = j->Get("metallic")) e.metallic = static_cast<float>(m->GetNumber());
            if (const core::Json* r = j->Get("roughness")) e.roughness = static_cast<float>(r->GetNumber());
            if (const core::Json* a = j->Get("ao")) e.ao = static_cast<float>(a->GetNumber());
            if (const core::Json* ei = j->Get("emissiveIntensity")) e.emissiveIntensity = static_cast<float>(ei->GetNumber());
            if (const core::Json* at = j->Get("albedoTex")) e.albedoTex = at->GetString();
            if (const core::Json* mt = j->Get("mrTex")) e.mrTex = mt->GetString();
            if (const core::Json* aot = j->Get("aoTex")) e.aoTex = aot->GetString();
            if (const core::Json* et = j->Get("emissiveTex")) e.emissiveTex = et->GetString();
            // Flat editor-scene format: a "scripts" array (new) or the legacy
            // scriptPath/scriptBackend/scriptVars keys (old saves).
            if (const core::Json* list = j->Get("scripts")) {
                if (list->IsArray()) {
                    for (const core::Json& it : list->Items()) {
                        SceneScriptFields f;
                        f.backend =
                            it.Get("backend") ? it.Get("backend")->GetString("lua") : "lua";
                        f.path = it.Get("path") ? it.Get("path")->GetString() : "";
                        if (const core::Json* v = it.Get("vars")) f.vars = *v;
                        if (!f.path.empty()) e.scripts.push_back(std::move(f));
                    }
                }
            } else if (const core::Json* sp = j->Get("scriptPath")) {
                SceneScriptFields f;
                f.path = sp->GetString();
                if (const core::Json* sb = j->Get("scriptBackend"))
                    f.backend = sb->GetString();
                if (const core::Json* sv = j->Get("scriptVars")) f.vars = *sv;
                if (!f.path.empty()) e.scripts.push_back(std::move(f));
            }
        }
        if (!e.materialRef.empty()) {
            // Material-ball reference ("materials/x.mat.json"): expand it into
            // the flattened fields before resolving the mesh.
            LoadMaterialParamsInto(e, projectDir_ + "/" + e.materialRef);
        }
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            loaded.push_back(std::move(e));
        }
    }
    if (has2DData) {
        editMode_ = EditMode::Scene2D;
        viewCam_ = ViewCam::Front; // 2D canvas view is the front-ortho camera
        // 2D scenes live in the 1280x720 design space: frame that space so the
        // editor shows exactly what the game sees (same content as playtest).
        camTarget_ = {640.0f, 360.0f, 0.0f};
        orthoSize_ = 360.0f;
        cameraUserAdjusted_ = false;
        NEON_LOG_INFO("Scene 2D level loaded (%zu plants, %zu zombie spawns)",
                      pvzPlants_.size(), pvzZombies_.size());
    }
    if (!loaded.empty() || has2DData) {
        entities_ = std::move(loaded);
        SetSelection(-1);
        history_.Clear(); // undo history from the previous scene is invalid
        currentSceneName_ = BaseName(path);
        NEON_LOG_INFO("Scene loaded (%zu entities)", entities_.size());
    }
}

// Reads <dir>/game.json into `p` (title/mode/startScene) and lists the
// project's scenes/. Returns false when there is no game.json.
bool EditorApp::ReadProjectMeta(EditorProject& p) {
    std::ifstream in(p.dir + "/game.json", std::ios::binary);
    if (!in.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    core::Json root = core::Json::Parse(text, &err);
    const size_t slash = p.dir.find_last_of("/\\");
    p.name = slash == std::string::npos ? p.dir : p.dir.substr(slash + 1);
    if (root.IsObject()) {
        if (const core::Json* t = root.Get("title"))
            if (t->IsString() && !t->GetString().empty()) p.name = t->GetString();
        if (const core::Json* s = root.Get("startScene"))
            if (s->IsString()) p.startScene = s->GetString();
        if (const core::Json* ed = root.Get("editor"))
            if (const core::Json* m = ed->Get("mode"))
                if (m->IsString() && m->GetString() == "2d") p.mode = "2d";
    }
    std::vector<AssetEntry> files;
    if (ListDirectory(p.dir + "/scenes", files)) {
        for (const AssetEntry& f : files) {
            if (f.isDir) continue;
            const std::string& n = f.name;
            const bool isJson = n.size() > 5 &&
                                (n.compare(n.size() - 5, 5, ".json") == 0 ||
                                 n.compare(n.size() - 5, 5, ".JSON") == 0);
            if (isJson) p.scenes.push_back("scenes/" + n);
        }
    }
    std::sort(p.scenes.begin(), p.scenes.end());
    return true;
}

// Discovers every project under projects/ (a directory with a game.json) and
// keeps the active-project fields in sync with projectDir_.
void EditorApp::ScanProjects() {
    projects_.clear();
    std::vector<AssetEntry> dirs;
    if (ListDirectory("projects", dirs)) {
        for (const AssetEntry& d : dirs) {
            if (!d.isDir) continue;
            EditorProject p;
            p.dir = d.path;
            if (ReadProjectMeta(p)) projects_.push_back(std::move(p));
        }
    }
    std::sort(projects_.begin(), projects_.end(),
              [](const EditorProject& a, const EditorProject& b) {
                  std::string al = a.name, bl = b.name;
                  std::transform(al.begin(), al.end(), al.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  std::transform(bl.begin(), bl.end(), bl.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  return al < bl;
              });
    // Re-sync the active project fields (projectSel_/name/mode/scenes).
    projectSel_ = -1;
    for (size_t i = 0; i < projects_.size(); ++i) {
        if (projects_[i].dir != projectDir_) continue;
        projectSel_ = static_cast<int>(i);
        projectName_ = projects_[i].name;
        projectMode_ = projects_[i].mode;
        projectStartScene_ = projects_[i].startScene;
        projectScenes_ = projects_[i].scenes;
        return;
    }
    // projectDir_ is not under projects/: a custom path (or the default
    // sandbox). Read its game.json directly when present.
    projectName_.clear();
    projectMode_ = "3d";
    projectStartScene_.clear();
    projectScenes_.clear();
    EditorProject custom;
    custom.dir = projectDir_;
    if (ReadProjectMeta(custom)) {
        projectName_ = custom.name;
        projectMode_ = custom.mode;
        projectStartScene_ = custom.startScene;
        projectScenes_ = custom.scenes;
    }
}

// Loads every prefabs/*.json from the current project (Godot-style prefab
// templates referenced by scene entities).
void EditorApp::LoadPrefabLibrary() {
    prefabLib_ = scene::PrefabLibrary();
    projectPrefabs_.clear();
    std::vector<AssetEntry> files;
    if (!ListDirectory(projectDir_ + "/prefabs", files)) return;
    for (const AssetEntry& f : files) {
        if (f.isDir) continue;
        const std::string& n = f.name;
        const bool isJson =
            n.size() > 5 && (n.compare(n.size() - 5, 5, ".json") == 0 ||
                             n.compare(n.size() - 5, 5, ".JSON") == 0);
        if (!isJson) continue;
        std::ifstream in(f.path, std::ios::binary);
        if (!in.is_open()) continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string name = BaseName(f.path);
        const size_t dot = name.find_last_of('.');
        const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        projectPrefabs_.push_back(stem);
        core::Status st = prefabLib_.Add(stem, text);
        if (!st.Ok())
            NEON_LOG_WARN("Editor: prefab '%s' failed to parse: %s", f.path.c_str(),
                          st.Error().c_str());
    }
    std::sort(projectPrefabs_.begin(), projectPrefabs_.end());
    NEON_LOG_INFO("Editor: prefab library loaded (%zu prefabs)", prefabLib_.Size());
}

// Saves the selected entity's components as prefabs/<name>.json (a component
// template other entities can instantiate).
void EditorApp::SavePrefab(const std::string& name) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (name.empty()) {
        NEON_LOG_WARN("Editor: prefab name is empty");
        return;
    }
    if (e.meshKey.empty()) {
        NEON_LOG_WARN("Editor: entity has no mesh; cannot save as prefab");
        return;
    }
    auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale,
                                            ExportMeshKey(e.meshKey), e.metallic, e.roughness,
                                            e.tint, e.albedoTex, e.mrTex, e.aoTex,
                                            e.emissiveTex, e.ao, e.emissiveIntensity,
                                            "", "", core::Json{}, {},
                                            e.hp, e.maxHp, e.parent);
    if (!res.Ok()) {
        NEON_LOG_ERROR("Editor: cannot save prefab: %s", res.Error().c_str());
        return;
    }
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json comps;
    if (const core::Json* c = res.Value().Get("components")) {
        if (c->IsObject()) comps = *c;
    }
    comps.type_ = core::Json::Type::Object;
    for (const auto& [cname, cdata] : e.extraComponents) comps.object_[cname] = cdata;
    if (!e.scripts.empty()) {
        auto mkStr = [](const std::string& v) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = v;
            return j;
        };
        core::Json items;
        items.type_ = core::Json::Type::Array;
        for (const SceneScriptFields& f : e.scripts) {
            if (f.path.empty()) continue; // unconfigured script block
            core::Json it;
            it.type_ = core::Json::Type::Object;
            it.object_["backend"] = mkStr(f.backend.empty() ? "lua" : f.backend);
            it.object_["path"] = mkStr(f.path);
            if (f.vars.IsObject()) it.object_["vars"] = f.vars;
            items.array_.push_back(std::move(it));
        }
        core::Json scripts;
        scripts.type_ = core::Json::Type::Object;
        scripts.object_["items"] = std::move(items);
        comps.object_["scripts"] = std::move(scripts);
    }
    root.object_["components"] = std::move(comps);

    const std::string dir = projectDir_ + "/prefabs";
    EnsureDirs(dir + "/");
    const std::string path = dir + "/" + name + ".json";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write prefab '%s'", path.c_str());
        return;
    }
    {
        out << core::JsonWriter::Write(root);
        out.close(); // flush before the library reload below reads the file
    }
    LoadPrefabLibrary();
    NEON_LOG_INFO("Editor: prefab saved -> %s", path.c_str());
}

// Expands a material-ball asset (materials/*.mat.json) into an entity's
// flattened material fields. False when the asset is missing or invalid.
bool EditorApp::LoadMaterialParamsInto(SceneEntity& e, const std::string& path) {
    std::string text;
#if defined(_WIN32)
    // Open with the wide API: std::ifstream cannot read UTF-8 CJK filenames
    // (realm's material balls are Chinese-named), which silently broke the
    // grid-view preview for those assets.
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                         static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen > 0 ? wlen : 0), L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                            &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        text.resize(static_cast<size_t>(sz));
        if (std::fread(&text[0], 1, static_cast<size_t>(sz), f) !=
            static_cast<size_t>(sz)) {
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
#else
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
#endif
    std::string err;
    core::Json root = core::Json::Parse(text, &err);
    if (!root.IsObject()) return false;
    if (const core::Json* c = root.Get("colorHex"))
        e.tint = ColorFromHex(c->GetString("#FFFFFF"));
    if (const core::Json* v = root.Get("metallic"))
        e.metallic = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("roughness"))
        e.roughness = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("ao")) e.ao = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("emissiveIntensity"))
        e.emissiveIntensity = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("albedoTex")) e.albedoTex = v->GetString();
    if (const core::Json* v = root.Get("mrTex")) e.mrTex = v->GetString();
    if (const core::Json* v = root.Get("aoTex")) e.aoTex = v->GetString();
    if (const core::Json* v = root.Get("emissiveTex")) e.emissiveTex = v->GetString();
    return true;
}

// Saves the selected entity's material as a material-ball asset and links the
// entity to it (one undo step).
void EditorApp::SaveMaterialAsset(const std::string& name) {
    if (name.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(entities_.size())) {
        NEON_LOG_WARN("Editor: material asset name/selection invalid");
        return;
    }
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (e.meshKey.empty()) {
        NEON_LOG_WARN("Editor: entity has no mesh; cannot save a material ball");
        return;
    }
    auto str = [](const std::string& s) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = s;
        return j;
    };
    auto num = [](double v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = v;
        return j;
    };
    core::Json root;
    root.type_ = core::Json::Type::Object;
    root.object_["colorHex"] = str(ColorToHex(e.tint));
    root.object_["metallic"] = num(e.metallic);
    root.object_["roughness"] = num(e.roughness);
    root.object_["ao"] = num(e.ao);
    root.object_["emissiveIntensity"] = num(e.emissiveIntensity);
    root.object_["albedoTex"] = str(e.albedoTex);
    root.object_["mrTex"] = str(e.mrTex);
    root.object_["aoTex"] = str(e.aoTex);
    root.object_["emissiveTex"] = str(e.emissiveTex);

    const std::string dir = projectDir_ + "/materials";
    EnsureDirs(dir + "/");
    const std::string rel = "materials/" + name + ".mat.json";
    const std::string path = projectDir_ + "/" + rel;
    // Wide-char open so CJK material names write correctly.
    if (!WriteFileUtf8(path, core::JsonWriter::Write(root))) {
        NEON_LOG_ERROR("Editor: cannot write material asset '%s'", path.c_str());
        return;
    }

    const MaterialAssetValue oldVal{e.materialRef, ColorToHex(e.tint), e.metallic, e.roughness,
                                    e.ao, e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    const MaterialAssetValue newVal{rel, ColorToHex(e.tint), e.metallic, e.roughness, e.ao,
                                    e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    history_.Push(std::make_unique<EditPropertyCommand<MaterialAssetValue>>(
        &entities_, selected_, ApplyMaterialAssetProp, oldVal, newVal));
    if (assetDir_ == dir) RefreshAssetDir();
    NEON_LOG_INFO("Editor: material ball saved -> %s", path.c_str());
}

// Applies a material-ball asset to the selected entity (one undo step).
void EditorApp::ApplyMaterialAsset(const std::string& path) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    SceneEntity tmp = e;
    if (!LoadMaterialParamsInto(tmp, path)) {
        NEON_LOG_ERROR("Editor: cannot load material asset '%s'", path.c_str());
        return;
    }
    const MaterialAssetValue oldVal{e.materialRef, ColorToHex(e.tint), e.metallic, e.roughness,
                                    e.ao, e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    // Store the reference project-relative ("materials/x.mat.json") so scenes
    // round-trip regardless of where the asset panel is browsing.
    std::string rel = path;
    const std::string base = projectDir_ == "." ? "" : projectDir_ + "/";
    if (!base.empty() && rel.compare(0, base.size(), base) == 0)
        rel = rel.substr(base.size());
    const MaterialAssetValue newVal{rel, ColorToHex(tmp.tint), tmp.metallic, tmp.roughness,
                                    tmp.ao, tmp.emissiveIntensity, tmp.albedoTex, tmp.mrTex,
                                    tmp.aoTex, tmp.emissiveTex};
    history_.Push(std::make_unique<EditPropertyCommand<MaterialAssetValue>>(
        &entities_, selected_, ApplyMaterialAssetProp, oldVal, newVal));
    ApplyMaterialParams(entities_[static_cast<size_t>(selected_)]);
    NEON_LOG_INFO("Editor: material asset '%s' applied", path.c_str());
}

// Godot-style project switch: loads <dir>/game.json, enters the project's
// declared edit mode and loads its start scene (3D) or first level (2D).
void EditorApp::SwitchProject(const std::string& dir) {
    StopPlaytest();
    projectDir_ = dir.empty() ? "." : dir;
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
    ScanProjects();
    LoadPrefabLibrary();
    history_.Clear();
    SetSelection(-1);
    if (projectMode_ == "2d") {
        // 2D projects are scenes too: LoadScene reads scenes/<start>.json and
        // its plant/zombie entities switch the editor to the 2D canvas
        // automatically. No separate assets/levels/ data path.
        editMode_ = EditMode::Scene2D;
        viewCam_ = ViewCam::Front;
        // Frame the 1280x720 design space (2D content uses design coords).
        camTarget_ = {640.0f, 360.0f, 0.0f};
        orthoSize_ = 360.0f;
        cameraUserAdjusted_ = false;
    } else {
        editMode_ = EditMode::Scene3D;
        viewCam_ = ViewCam::Perspective;
    }
    if (!projectStartScene_.empty()) {
        LoadScene(projectDir_ + "/" + projectStartScene_);
    } else if (projectMode_ != "2d") {
        LoadScene("editor_scene.json"); // default sandbox scene
    }
    // The asset panel always points at the active context's assets/ dir and
    // creates it on demand (so 导入/新建 always have a home). The default
    // sandbox (no project open, projectDir_ == ".") is the repo root, whose
    // assets/ dir is that context's project assets.
    const std::string assetsDir = projectDir_ + "/assets";
    MakeDir(assetsDir);
    assetDir_ = assetsDir;
    RefreshAssetDir();
    SaveEditorConfig();
    NEON_LOG_INFO("Editor: switched project '%s' (mode=%s, %zu scenes)",
                  projectName_.c_str(), projectMode_.c_str(), projectScenes_.size());
}

// Loads the current project's start scene (3D) / level (2D): a "reload"
// entry point for the 项目 menu.
void EditorApp::LoadProjectScene() {
    SwitchProject(projectDir_);
}

// Loads a specific scene from the current project into the 3D scene tree.
void EditorApp::LoadProjectScene(const std::string& rel) {
    StopPlaytest();
    SetSelection(-1);
    history_.Clear();
    LoadScene(projectDir_ + "/" + rel);
    NEON_LOG_INFO("Editor: project scene loaded from '%s/%s'", projectDir_.c_str(), rel.c_str());
}

void EditorApp::OpenScriptEditor(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Editor: cannot open script '%s'", path.c_str());
        return;
    }
    if (!scriptCheckHost_) {
        scriptCheckHost_ = script::CreateLuaHost();
        if (scriptCheckHost_) scriptCheckHost_->Init();
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t cap = sizeof(scriptEditorBuf_) - 1;
    if (text.size() >= cap) {
        NEON_LOG_WARN("Editor: script '%s' too large for the editor buffer (%.1f KB cap)",
                      path.c_str(), static_cast<double>(cap) / 1024.0);
        text.resize(cap);
    }
    std::memcpy(scriptEditorBuf_, text.data(), text.size());
    scriptEditorBuf_[text.size()] = '\0';
    scriptEditorPath_ = path;
    scriptEditorRel_ = path;
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    if (path.compare(0, base.size(), base) == 0 && path.size() > base.size() &&
        (path[base.size()] == '/' || path[base.size()] == '\\'))
        scriptEditorRel_ = path.substr(base.size() + 1);
    scriptEditorCheck_ = ScriptCheckResult{};
    if (scriptCheckHost_) {
        if (scriptEditorRel_ != path) {
            scriptEditorCheck_ = CheckScriptFile(*scriptCheckHost_, base, scriptEditorRel_);
        } else {
            std::ifstream src(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(src)),
                             std::istreambuf_iterator<char>());
            scriptEditorCheck_.path = path;
            scriptEditorCheck_.ok = scriptCheckHost_->CheckSyntax(text);
            if (!scriptEditorCheck_.ok) {
                scriptEditorCheck_.message = scriptCheckHost_->LastError().message;
                scriptEditorCheck_.line = scriptCheckHost_->LastError().line;
            }
        }
    }
    scriptEditorDirty_ = false;
    showScriptEditor_ = true;
    NEON_LOG_INFO("Editor: script editor opened '%s'", path.c_str());
}

// Saves the built-in editor's content, re-checks syntax and refreshes the
// script panel.
void EditorApp::SaveScriptEditor() {
    if (scriptEditorPath_.empty()) return;
    std::ofstream out(scriptEditorPath_, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write script '%s'", scriptEditorPath_.c_str());
        return;
    }
    out << scriptEditorBuf_;
    scriptEditorDirty_ = false;
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    if (scriptCheckHost_) {
        if (scriptEditorRel_ != scriptEditorPath_) {
            scriptEditorCheck_ = CheckScriptFile(*scriptCheckHost_, base, scriptEditorRel_);
        } else {
            scriptEditorCheck_.path = scriptEditorPath_;
            scriptEditorCheck_.ok = scriptCheckHost_->CheckSyntax(scriptEditorBuf_);
            if (!scriptEditorCheck_.ok) {
                scriptEditorCheck_.message = scriptCheckHost_->LastError().message;
                scriptEditorCheck_.line = scriptCheckHost_->LastError().line;
            }
        }
    }
    RefreshScriptChecks();
    NEON_LOG_INFO("Editor: script saved '%s'", scriptEditorPath_.c_str());
}

// Opens the file in the system's default editor (VS Code etc.).

void EditorApp::OpenInExternalEditor(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const std::string cmd = std::string("xdg-open \"") + path + "\"";
    std::system(cmd.c_str());
#endif
}

void EditorApp::ClampSelection() {
    // Undo/redo can move entities under an unchanged selection index (e.g. a
    // reorder), so invalidate the script panel's index-keyed sync cache
    // unconditionally here, not only when the index changes.
    scriptSyncEntity_ = -1;
    if (entities_.empty()) {
        selected_ = -1;
    } else if (selected_ >= static_cast<int>(entities_.size())) {
        selected_ = static_cast<int>(entities_.size()) - 1;
    }
}

} // namespace neon::editor
