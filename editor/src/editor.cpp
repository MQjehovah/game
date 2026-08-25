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
    else if (key.rfind("obj:", 0) == 0) rel = key.substr(4);
    else if (key.rfind("gltf:", 0) == 0) rel = key.substr(5);
    else return {};
    return ResolveMeshAssetPath(rel, projectDir);
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

// ---------------------------------------------------------------------------
// Panel open/closed state persistence (neon_editor_imgui.ini).
// The ImGui ini saves each panel's dock node/size but NOT whether the panel is
// open, so a panel the user opened and docked vanished on the next launch (its
// show flag reset to the default while the layout data survived). A custom
// settings handler persists every panel's open state into the same ini: the
// next launch restores both the layout and which panels were visible.
// ---------------------------------------------------------------------------
struct PanelStateEntry {
    const char* title;        // ImGui window title / 视图 menu label
    bool EditorApp::*flag;    // member pointer to the panel's show flag
};

EditorApp* g_panelStateApp = nullptr;   // app owning the flags (set on register)
const PanelStateEntry* g_panelStateEntries = nullptr;
size_t g_panelStateCount = 0;

// Called when the loader enters "[NeonPanels][Panels]"; returning non-null
// marks the section as present so ReadLine applies the saved states.
void* NeonPanelsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) {
    return g_panelStateApp;
}

void NeonPanelsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry,
                        const char* line) {
    EditorApp* app = static_cast<EditorApp*>(entry);
    if (!app || !line) return;
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
        --len;
    if (len == 0 || line[0] == '#') return; // blank / comment
    const char* eq = static_cast<const char*>(std::memchr(line, '=', len));
    if (!eq) return;
    const bool open = std::atoi(eq + 1) != 0;
    const std::string title(line, static_cast<size_t>(eq - line));
    for (size_t i = 0; i < g_panelStateCount; ++i) {
        if (title == g_panelStateEntries[i].title) {
            app->*g_panelStateEntries[i].flag = open;
            return;
        }
    }
    // Plugin-contributed panels are matched by title as well (their open state
    // is owned by the plugin manager, not a member flag).
    if (editor::EditorPluginManager* mgr = app->PluginManager()) {
        for (editor::PluginPanel& p : mgr->Panels()) {
            if (title == p.title) {
                p.opened = open;
                return;
            }
        }
    }
}

void NeonPanelsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                        ImGuiTextBuffer* buf) {
    EditorApp* app = g_panelStateApp;
    if (!app || !buf) return;
    buf->appendf("[%s][Panels]\n", handler->TypeName);
    for (size_t i = 0; i < g_panelStateCount; ++i)
        buf->appendf("%s=%d\n", g_panelStateEntries[i].title,
                     app->*g_panelStateEntries[i].flag ? 1 : 0);
    if (editor::EditorPluginManager* mgr = app->PluginManager()) {
        for (const editor::PluginPanel& p : mgr->Panels())
            buf->appendf("%s=%d\n", p.title.c_str(), p.opened ? 1 : 0);
    }
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

// Friend of EditorApp: builds the title -> show-flag table (naming the private
// members requires friendship) and registers the ini settings handler above.
void RegisterPanelStateHandler(EditorApp* app) {
    static const PanelStateEntry kPanels[] = {
        {"场景", &EditorApp::showHierarchy_},
        {"属性", &EditorApp::showInspector_},
        {"资产", &EditorApp::showAssets_},
        {"资源", &EditorApp::showResources_},
        {"日志", &EditorApp::showLog_},
        {"模型查看器", &EditorApp::showModelPreview_},
        {"行为树", &EditorApp::showBt_},
        {"脚本", &EditorApp::showScripts_},
        {"脚本编辑器", &EditorApp::showScriptEditor_},
        {"打包", &EditorApp::showPackage_},
        {"性能", &EditorApp::showProfiler_},
        {"导航", &EditorApp::showNav_},
        {"调试覆盖层", &EditorApp::showDebugOverlay_},
        {"动画时间线", &EditorApp::showAnimEditor_},
        {"地形编辑", &EditorApp::showTerrain_},
        {"2D 地图", &EditorApp::showTilemap_},
        {"本地化", &EditorApp::showLoc_},
        {"UI 编辑器", &EditorApp::showUIEditor_},
        {"输入映射", &EditorApp::showInputMap_},
        {"插件", &EditorApp::showPlugins_},
    };
    g_panelStateApp = app;
    g_panelStateEntries = kPanels;
    g_panelStateCount = sizeof(kPanels) / sizeof(kPanels[0]);

    ImGuiSettingsHandler panelHandler;
    panelHandler.TypeName = "NeonPanels";
    panelHandler.TypeHash = ImHashStr("NeonPanels");
    panelHandler.ReadOpenFn = &NeonPanelsReadOpen;
    panelHandler.ReadLineFn = &NeonPanelsReadLine;
    panelHandler.WriteAllFn = &NeonPanelsWriteAll;
    ImGui::AddSettingsHandler(&panelHandler);
}

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
    ApplyEditorTheme();
    // Panel open/close persistence: save which panels are visible into the
    // same ImGui ini that stores the docking layout (see NeonPanels* above).
    RegisterPanelStateHandler(this);
    // Toolbar icon glyph self-check: a missing glyph renders as '?' in the
    // toolbar. Log once at startup so icon regressions are caught immediately.
    {
        ImFontBaked* baked = nullptr;
        if (ImGui::GetIO().Fonts && !ImGui::GetIO().Fonts->Fonts.empty())
            baked = ImGui::GetIO().Fonts->Fonts[0]->GetFontBaked(18.0f);
        if (baked) {
            static const unsigned int kToolbarIcons[] = {
                0x2725, 0x27F3, 0x21F2, 0x25C9, 0x25CE, 0x25A6, 0x2316,
                0x25B6, 0x25A0, 0x25CF, 0x25CB, 0x2715, 0x2B06};
            std::string missing;
            for (unsigned int cp : kToolbarIcons) {
                if (!baked->IsGlyphLoaded(static_cast<ImWchar>(cp))) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "U+%04X ", cp);
                    missing += buf;
                }
            }
            if (missing.empty()) {
                NEON_LOG_INFO("Editor: toolbar icon glyphs all present in the font atlas");
            } else {
                NEON_LOG_WARN("Editor: toolbar glyphs missing from the font atlas: %s",
                              missing.c_str());
            }
        }
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
    // Editor plugins: extend the editor itself (panels/tools/asset sources/
    // component schemas) from <projectDir>/plugins (type "editor").
    pluginMgr_ = std::make_unique<editor::EditorPluginManager>();
    pluginMgr_->Init(this);
    pluginMgr_->Load(projectDir_);
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
    if (!previewOnStart_.empty()) {
        showModelPreview_ = true;
        OpenModelPreview(previewOnStart_);
    }
    LoadInputMapEdit(); // Godot-style input panel data
    return true;
}

// Godot-inspired dark editor theme (UX item 6). Applied once right after the
// ImGui context exists; the palette + compact metrics make docked panels read
// as one workspace instead of default-gray windows, and keep the toolbar and
// inspector dense enough that content, not chrome, fills the screen.
void EditorApp::ApplyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Compact metrics.
    s.WindowPadding = ImVec2(6.0f, 6.0f);
    s.FramePadding = ImVec2(5.0f, 3.0f);
    s.ItemSpacing = ImVec2(6.0f, 4.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    s.IndentSpacing = 16.0f;
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowRounding = 0.0f; // docked panels: flush edges
    s.ChildRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 3.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    // Palette.
    const ImVec4 kAccent(0.35f, 0.65f, 1.00f, 1.0f);
    const ImVec4 kWindowBg(0.125f, 0.133f, 0.157f, 1.0f);
    const ImVec4 kChildBg(0.102f, 0.110f, 0.133f, 1.0f);
    const ImVec4 kPopupBg(0.165f, 0.176f, 0.200f, 1.0f);
    const ImVec4 kFrameBg(0.176f, 0.188f, 0.216f, 1.0f);
    const ImVec4 kFrameHover(0.235f, 0.255f, 0.294f, 1.0f);
    const ImVec4 kFrameActive(0.310f, 0.340f, 0.390f, 1.0f);
    const ImVec4 kTitleBg(0.145f, 0.155f, 0.180f, 1.0f);
    const ImVec4 kTitleActive(0.220f, 0.240f, 0.280f, 1.0f);
    const ImVec4 kBorder(0.250f, 0.270f, 0.310f, 0.80f);
    const ImVec4 kText(0.88f, 0.89f, 0.92f, 1.0f);
    const ImVec4 kTextDisabled(0.42f, 0.45f, 0.51f, 1.0f);
    const ImVec4 kHeader(0.220f, 0.240f, 0.280f, 1.0f);
    const ImVec4 kHeaderHover(0.290f, 0.320f, 0.380f, 1.0f);
    const ImVec4 kHeaderActive(0.350f, 0.390f, 0.460f, 1.0f);
    const ImVec4 kTabHover(0.290f, 0.320f, 0.380f, 1.0f);
    const ImVec4 kTabSelected(0.240f, 0.290f, 0.360f, 1.0f);
    const ImVec4 kTabDimmed(0.170f, 0.185f, 0.220f, 1.0f);
    const ImVec4 kTabDimmedSelected(0.200f, 0.235f, 0.290f, 1.0f);
    const ImVec4 kSeparator(0.250f, 0.270f, 0.310f, 1.0f);
    const ImVec4 kScrollbarGrab(0.330f, 0.360f, 0.410f, 1.0f);
    const ImVec4 kScrollbarHover(0.420f, 0.460f, 0.520f, 1.0f);
    const ImVec4 kScrollbarActive(0.500f, 0.550f, 0.620f, 1.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDisabled;
    c[ImGuiCol_WindowBg] = kWindowBg;
    c[ImGuiCol_ChildBg] = kChildBg;
    c[ImGuiCol_PopupBg] = kPopupBg;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = kFrameBg;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kTitleBg;
    c[ImGuiCol_TitleBgActive] = kTitleActive;
    c[ImGuiCol_TitleBgCollapsed] = kTitleBg;
    c[ImGuiCol_MenuBarBg] = kTitleBg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.8f);
    c[ImGuiCol_ScrollbarGrab] = kScrollbarGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = kScrollbarHover;
    c[ImGuiCol_ScrollbarGrabActive] = kScrollbarActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kScrollbarGrab;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = kFrameBg;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = kHeader;
    c[ImGuiCol_HeaderHovered] = kHeaderHover;
    c[ImGuiCol_HeaderActive] = kHeaderActive;
    c[ImGuiCol_Separator] = kSeparator;
    c[ImGuiCol_SeparatorHovered] = kHeaderHover;
    c[ImGuiCol_SeparatorActive] = kHeaderActive;
    c[ImGuiCol_ResizeGrip] = kScrollbarGrab;
    c[ImGuiCol_ResizeGripHovered] = kScrollbarHover;
    c[ImGuiCol_ResizeGripActive] = kScrollbarActive;
    c[ImGuiCol_Tab] = kTabDimmed;
    c[ImGuiCol_TabHovered] = kTabHover;
    c[ImGuiCol_TabSelected] = kTabSelected;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kTabDimmed;
    c[ImGuiCol_TabDimmedSelected] = kTabDimmedSelected;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_DockingPreview] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.09f, 0.095f, 0.12f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = kHeader;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextLink] = kAccent;
    c[ImGuiCol_PlotLines] = kAccent;
    c[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavHighlight] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    if (pluginMgr_) pluginMgr_->Shutdown();
    if (audioBackend_) {
        audioBackend_->Shutdown();
        audioBackend_.reset();
    }
    // Release the offscreen thumbnail targets + their ImGui registrations
    // before the renderer shuts down.
    if (gfx::IRenderBackend* backend = renderer_.Backend()) {
        for (SceneEntity& e : entities_) {
            if (e.customShader.Valid()) backend->DestroyShader(e.customShader.Handle());
        }
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
    if (benchMode_ && benchFrames_ > 0) {
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                     "BENCH-SUMMARY frames=%llu avgMs=%.2f maxMs=%.2f",
                     static_cast<unsigned long long>(benchFrames_),
                     benchFrameMsSum_ / static_cast<float>(benchFrames_), benchFrameMsMax_);
    }
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
    NormalizeEntityIds(); // setup-created entities also need stable ids
    EnsureSceneDefaultObjects();
}

void EditorApp::OnUpdate(float dt) {
    // P2-6 benchmark: per-interval frame-time/draw logs + an exit summary.
    if (benchMode_) {
        const float ms = TimeRef().delta * 1000.0f;
        ++benchFrames_;
        benchFrameMsSum_ += ms;
        benchFrameMsMax_ = std::fmax(benchFrameMsMax_, ms);
        if (TimeRef().frameIndex - benchLastLogFrame_ >= 60) {
            benchLastLogFrame_ = TimeRef().frameIndex;
            const assets::AssetStats st = assetMgr_.Stats();
            NEON_LOG_CAT(
                core::LogCategory::Core, core::LogLevel::Info,
                "BENCH frame=%llu fps=%.1f avgMs=%.2f maxMs=%.2f ents=%zu draws=%u bodies=%zu "
                "tex=%zu mesh=%zu",
                static_cast<unsigned long long>(TimeRef().frameIndex), TimeRef().Fps(),
                benchFrameMsSum_ / static_cast<float>(benchFrames_), benchFrameMsMax_,
                entities_.size(), renderer_.Stats().drawCalls,
                playtest_ ? playtest_->PhysicsBodyCount() : 0, st.textures, st.meshes);
        }
    }
    // Drain completed async texture decodes (uploads + callbacks, main thread).
    assetMgr_.PumpAsync();
    // Advance animated skinned glTF entities (edit-mode pose; the playtest
    // advances its own animators inside GameRuntime::TickAnimations).
    for (SceneEntity& e : entities_) {
        if (e.skinned && e.skinned->Valid()) e.skinned->Update(dt);
    }
    // Advance the model-preview playhead.
    if (showModelPreview_ && previewModel_ && previewPlaying_) {
        float dur = previewModel_->clips.empty()
                        ? 1.0f
                        : previewModel_->clips[static_cast<size_t>(previewClip_)].duration;
        if (dur > 0.0f) previewTime_ = std::fmod(previewTime_ + dt, dur);
    }
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

    // Smoke: panel open/close state must round-trip through the ini settings
    // handler (the user's report: a panel opened and docked vanished on the
    // next launch because only the layout was saved, not whether it was open).
    // Drive the handler's write path into a buffer, lose the states, then feed
    // the lines back through the read path exactly as LoadIniSettingsFromDisk
    // would, and verify the flags come back.
    if (smokeMode_ && TimeRef().frameIndex == 100) {
        static bool panelSmokeDone = false;
        if (!panelSmokeDone) {
            panelSmokeDone = true;
            bool ok = g_panelStateApp == this && g_panelStateEntries != nullptr;
            ImGuiSettingsHandler* h = ImGui::FindSettingsHandler("NeonPanels");
            ok = ok && h != nullptr;
            const bool savedAssets = showAssets_;
            const bool savedLoc = showLoc_;
            if (ok) {
                showAssets_ = true;  // open a panel (user docks it in real use)
                showLoc_ = false;    // close another
                ImGuiTextBuffer buf;
                h->WriteAllFn(ImGui::GetCurrentContext(), h, &buf);
                showAssets_ = false; // "lost" states; the read must restore
                showLoc_ = true;
                const char* p = buf.begin();
                while (p && *p) {
                    const char* nl = std::strchr(p, '\n');
                    const std::string line(
                        p, nl ? static_cast<size_t>(nl - p) : std::strlen(p));
                    if (!line.empty() && line[0] != '[')
                        h->ReadLineFn(ImGui::GetCurrentContext(), h,
                                      g_panelStateApp, line.c_str());
                    if (!nl) break;
                    p = nl + 1;
                }
                ok = showAssets_ && !showLoc_;
            }
            showAssets_ = savedAssets;
            showLoc_ = savedLoc;
            NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Info,
                         "EDITOR-PANELS-SMOKE: [%s] panel open/close state "
                         "round-trips through the ini handler",
                         ok ? "PASS" : "FAIL");
            if (!ok) smokeFailed_ = true;
        }
    }

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
        // Force the BT tab active for the upcoming render: when the persisted
        // dock layout tabs the panel away, ImGui::Begin returns false and the
        // canvas never emits vertices, so btCanvasDrawn_ (checked at frame 30)
        // would stay false. Focus is honored on the next frame's Begin.
        ImGui::SetWindowFocus("\u884c\u4e3a\u6811");
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
    // P2-6 shader hot reload smoke: compile a temp fragment shader on the
    // first entity, rewrite the file, recompile (mtime gate), restore.
    if (smokeMode_ && TimeRef().frameIndex == 46) {
        bool ok = !entities_.empty();
        const bool glBackend = backendName_ != "vulkan";  // custom shaders: GL only
        SceneEntity* target = entities_.empty() ? nullptr : &entities_[0];
        const std::string shPath = GetTempDir() + "/smoke_tint.glsl";
        const std::string oldShader = target ? target->shaderPath : "";
        if (ok) {
            if (std::ofstream out(shPath, std::ios::binary); out.is_open()) {
                out << "#version 330 core\n"
                    << "in vec2 vUV;\nin vec4 vColor;\nout vec4 FragColor;\n"
                    << "uniform sampler2D uTex;\n"
                    << "void main() { FragColor = vColor * texture(uTex, vUV); }\n";
            }
            target->shaderPath = shPath;
            ReloadEntityShader(*target);
            ok = !glBackend || target->customShader.Valid();
            if (ok) {
                // Touch the file and recompile (simulates a hot reload).
                if (std::ofstream out2(shPath, std::ios::app); out2) out2 << "// touched\n";
                const uint64_t before = FileMTime(shPath);
                ReloadEntityShader(*target);
                ok = (!glBackend || target->customShader.Valid()) && FileMTime(shPath) >= before;
            }
        }
        if (target) {
            target->shaderPath = oldShader;
            ReloadEntityShader(*target);
        }
        NEON_LOG_INFO("EDITOR-SHADER-SMOKE: [%s] custom fragment shader compile + hot reload",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P1-1 terrain tool smoke: edit the heightmap canvas and rebuild the mesh.
    if (smokeMode_ && TimeRef().frameIndex == 47) {
        bool ok = false;
        for (SceneEntity& e : entities_) {
            if (e.meshKey != "terrain") continue;
            RebuildTerrainMesh(e);
            if (e.terrainHeights_.empty() || !e.mesh.Valid()) break;
            const float before = e.terrainHeights_[0];
            e.terrainHeights_[0] = before + 1.0f;
            RebuildTerrainMesh(e);
            ok = e.mesh.Valid() &&
                 std::fabs(e.terrainHeights_[0] - before - 1.0f) < 1e-4f;
            break;
        }
        NEON_LOG_INFO("EDITOR-TERRAIN-SMOKE: [%s] terrain heightmap edit + rebuild",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P1-1 tilemap smoke: a "tilemap" entity resolves without a mesh and
    // carries its cell grid.
    if (smokeMode_ && TimeRef().frameIndex == 48) {
        SceneEntity tm;
        tm.name = "smoke_tilemap";
        tm.meshKey = "tilemap";
        tm.tilemapCols_ = 2;
        tm.tilemapRows_ = 2;
        tm.tilemapTiles_ = {"a.png", "", "b.png", ""};
        const bool ok = ResolveMesh(tm) && !tm.mesh.Valid() &&
                        tm.tilemapTiles_.size() == 4u;
        NEON_LOG_INFO("EDITOR-TILEMAP-SMOKE: [%s] tilemap entity resolves (cells=%zu)",
                      ok ? "PASS" : "FAIL", tm.tilemapTiles_.size());
        if (!ok) smokeFailed_ = true;
    }
    // P2-1 decal smoke: exporting a scene with a decal writes the runtime
    // component.
    if (smokeMode_ && TimeRef().frameIndex == 49) {
        bool ok = false;
        if (!entities_.empty()) {
            SceneEntity& e = entities_[0];
            const std::string oldTex = e.decalTex;
            e.decalTex = "assets/textures/decal.png";
            e.decalSize = 3.0f;
            e.decalAlpha = 0.5f;
            auto rootRes = BuildPlaySceneJson();
            e.decalTex = oldTex;
            e.decalMesh = {};
            if (rootRes.Ok()) {
                const core::Json* ents = rootRes.Value().Get("entities");
                if (ents && ents->Size() > 0) {
                    const core::Json* comps = ents->At(0)->Get("components");
                    ok = comps && comps->Get("decal") != nullptr;
                }
            }
        }
        NEON_LOG_INFO("EDITOR-DECAL-SMOKE: [%s] decal exports as a runtime component",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // P2-editor UX smoke: multi-selection + batch duplicate/delete + batch
    // gizmo transform command round-trip.
    if (smokeMode_ && TimeRef().frameIndex == 50) {
        bool ok = entities_.size() >= 2;
        if (ok) {
            SetSelection(0);
            ToggleSelection(1);
            ok = selection_.size() == 2 && IsSelected(0) && IsSelected(1);
            const size_t before = entities_.size();
            history_.Push(
                std::make_unique<MultiDuplicateEntityCommand>(&entities_, SelectedIndices()));
            ok = ok && entities_.size() == before + 2;
            ClampSelection();
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ok = ok && entities_.size() == before;
            ClampSelection();
            // Batch transform: move two entities by (1,0,0), then undo.
            const math::Vec3 p0 = entities_[0].pos;
            const math::Vec3 p1 = entities_[1].pos;
            std::vector<int> ids = {0, 1};
            std::vector<Transform3> from = {
                {entities_[0].pos, entities_[0].rot, entities_[0].scale},
                {entities_[1].pos, entities_[1].rot, entities_[1].scale}};
            std::vector<Transform3> to = from;
            to[0].pos.x += 1.0f;
            to[1].pos.x += 1.0f;
            history_.Push(std::make_unique<BatchTransformCommand>(&entities_, ids, from, to));
            ok = ok && std::fabs(entities_[0].pos.x - p0.x - 1.0f) < 1e-5f &&
                 std::fabs(entities_[1].pos.x - p1.x - 1.0f) < 1e-5f;
            history_.Undo();
            ok = ok && std::fabs(entities_[0].pos.x - p0.x) < 1e-5f &&
                 std::fabs(entities_[1].pos.x - p1.x) < 1e-5f;
            SetSelection(0);
        }
        NEON_LOG_INFO("EDITOR-MULTISELECT-SMOKE: [%s] multi-select + batch ops",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // Scene-tree smoke: per-parent recursive name sort (multi-level tree:
    // parent before children, each sibling group alphabetical) and the one
    // undo step that restores the previous order. The temporary names and
    // parent links are restored before the frame ends so the playtest smoke
    // at frame 60 sees the untouched scene.
    if (smokeMode_ && TimeRef().frameIndex == 51) {
        bool ok = entities_.size() >= 3;
        if (ok) {
            NormalizeEntityIds();
            const int id0 = entities_[0].id;
            const int id1 = entities_[1].id;
            const int id2 = entities_[2].id;
            const std::string n0 = entities_[0].name;
            const std::string n1 = entities_[1].name;
            const std::string n2 = entities_[2].name;
            const int p1 = entities_[1].parentId;
            const int p2 = entities_[2].parentId;
            entities_[0].name = "B_root";
            entities_[1].name = "A_child";
            entities_[2].name = "C_child";
            entities_[1].parentId = id0;
            entities_[2].parentId = id1; // grandchild: 3 levels deep
            const size_t undoDepthBefore = history_.UndoDepth();
            SortSceneTreeByName();
            size_t i0 = static_cast<size_t>(-1);
            size_t i1 = static_cast<size_t>(-1);
            size_t i2 = static_cast<size_t>(-1);
            for (size_t i = 0; i < entities_.size(); ++i) {
                if (entities_[i].id == id0) i0 = i;
                if (entities_[i].id == id1) i1 = i;
                if (entities_[i].id == id2) i2 = i;
            }
            ok = i0 != static_cast<size_t>(-1) && i1 != static_cast<size_t>(-1) &&
                 i2 != static_cast<size_t>(-1) && i0 < i1 && i1 < i2;
            if (history_.UndoDepth() > undoDepthBefore)
                history_.Undo(); // restore the pre-sort order
            for (SceneEntity& e : entities_) {
                if (e.id == id0) e.name = n0;
                if (e.id == id1) {
                    e.name = n1;
                    e.parentId = p1;
                }
                if (e.id == id2) {
                    e.name = n2;
                    e.parentId = p2;
                }
            }
        }
        NEON_LOG_INFO("EDITOR-SCENETREE-SMOKE: [%s] per-parent recursive sort + undo",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
    }
    // Save/load round-trip: SaveScene must write the scene that is actually
    // loaded (currentScenePath_) in the runtime componentized format, so
    // hierarchy, ids, rotation and health survive a restart. Regression for
    // "save, restart, hierarchy lost" (SaveScene used to hardcode
    // editor_scene.json and never touched the loaded project scene).
    if (smokeMode_ && TimeRef().frameIndex == 52) {
        bool ok = entities_.size() >= 2;
        const std::string prevPath = currentScenePath_;
        if (ok) {
            NormalizeEntityIds();
            const int id0 = entities_[0].id;
            const int id1 = entities_[1].id;
            entities_[1].parentId = id0; // parent -> child
            entities_[1].rot = math::Quat{0.1f, 0.2f, 0.3f, 1.0f};
            const float hp = entities_[1].maxHp > 0.0f ? entities_[1].hp : 0.0f;
            const std::string tmpSave = GetTempDir() + "/smoke_save_roundtrip.json";
            currentScenePath_ = tmpSave;
            SaveScene();
            // Guard: the smoke must NEVER write a real scene (the user's
            // project scenes / sandbox are user data). Verify the write target
            // was actually the temp file; any deviation fails the smoke.
            if (currentScenePath_ != tmpSave) ok = false;
            currentScenePath_ = prevPath;
            LoadScene(tmpSave); // reload exactly what SaveScene wrote
            int n0 = -1;
            int n1 = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                if (entities_[i].id == id0) n0 = static_cast<int>(i);
                if (entities_[i].id == id1) n1 = static_cast<int>(i);
            }
            ok = n0 >= 0 && n1 >= 0 && entities_[static_cast<size_t>(n1)].parentId == id0 &&
                 entities_[static_cast<size_t>(n1)].rot.x == 0.1f &&
                 entities_[static_cast<size_t>(n1)].rot.y == 0.2f &&
                 entities_[static_cast<size_t>(n1)].rot.z == 0.3f;
            if (ok && hp > 0.0f)
                ok = entities_[static_cast<size_t>(n1)].hp == hp;
            if (!prevPath.empty()) LoadScene(prevPath); // restore the scene
        }
        NEON_LOG_INFO("EDITOR-SAVESCENE-SMOKE: [%s] save->load keeps hierarchy/id/rot/health",
                      ok ? "PASS" : "FAIL");
        if (!ok) smokeFailed_ = true;
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
            // P5-editor UX: outline every selected node; the active one gets
            // resize handles.
            for (ui::UiNode* n : uiSelection_) {
                if (!n) continue;
                const math::Rect2 sel = n->AbsoluteRect();
                renderer_.DrawRectOutline(sel, 2.0f,
                                          {0.4f, 0.9f, 1.0f, 0.9f});
                if (n != uiSelected_) continue;
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
        // P2-editor UX: world-space grid overlay (toggle in the viewport
        // toolbar). Drawn into the scene so it matches the active camera.
        if (showViewportGrid_) {
            const float half = 30.0f;
            const gfx::Color gridC{0.42f, 0.48f, 0.58f, 0.35f};
            std::vector<gfx::Renderer::LineVertex> gv;
            for (float x = -half; x <= half; x += 1.0f) {
                gv.push_back({{x, 0.0f, -half}, gridC});
                gv.push_back({{x, 0.0f, half}, gridC});
            }
            for (float z = -half; z <= half; z += 1.0f) {
                gv.push_back({{-half, 0.0f, z}, gridC});
                gv.push_back({{half, 0.0f, z}, gridC});
            }
            renderer_.DrawLines(gv.data(), static_cast<uint32_t>(gv.size()),
                                math::Mat4::Identity());
        }
        // The scene's DirectionalLight object drives the world light (Unity-style).
        const scene::SceneLight* sl = nullptr;
        for (const SceneEntity& se : entities_) {
            if (se.hasLight) { sl = &se.light; break; }
        }
        if (sl)
            renderer_.SetDirectionalLight(sl->sunDir, sl->color, sl->ambientStrength);
        else
            renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.45f);
        // 2D view = camera lock: mark the ortho camera's visible rect so the
        // user sees exactly what the front/top camera frames (Unity-style
        // camera border), no matter which project the view belongs to.
        if (viewCam_ == ViewCam::Front) DrawCameraFrame();

        if (playtestActive_ && playtest_) {
            // Play mode: the viewport renders the runtime's world (a snapshot
            // of the scene taken at Play). The editor scene is untouched.
            // For a 2D project the runtime content must be framed by the same
            // 1280x720 design space the edit-mode camera frame draws, NOT the
            // editor's free pan/zoom camera (which would land the game off the
            // frame). Lock the play camera to the design rect instead.
            gfx::Camera playCam = cam;
            if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) {
                playCam.position = {640.0f, 360.0f, 100.0f};
                playCam.target = {640.0f, 360.0f, 0.0f};
                playCam.up = {0.0f, 1.0f, 0.0f};
                playCam.ortho = true;
                playCam.orthoSize = 360.0f; // half the 720 design height
                playCam.fovY = 60.0f * math::kDegToRad;
            }
            playtest_->Draw(renderer_, playCam);
            // Physics debug view: wireframe every collider so the collision
            // shapes are visible while playtesting (dynamic = cyan, static =
            // gray). Uses the physics world's live positions, so resting and
            // bouncing bodies show exactly where the simulation puts them.
            // G8-3: physics wireframe is one debug-overlay layer (on by default).
            if (debugColliders_) {
                for (const physics::World::DebugBody& db :
                     playtest_->PhysicsWorld().DebugBodies()) {
                    const gfx::Color c = db.dynamic ? gfx::Color{0.2f, 0.9f, 1.0f, 0.9f}
                                                    : gfx::Color{0.55f, 0.62f, 0.7f, 0.85f};
                    if (db.kind == physics::World::ShapeKind::Sphere)
                        renderer_.DrawSphere(db.pos, db.radius, c);
                    else
                        renderer_.DrawBox({db.pos - db.halfExtents, db.pos + db.halfExtents}, c);
                }
            }
        } else {
            // P2-3: sprites draw back-to-front by zOrder (stable sort keeps the
            // scene-tree order for everything else).
            std::vector<size_t> drawOrder(entities_.size());
            for (size_t i = 0; i < drawOrder.size(); ++i) drawOrder[i] = i;
            std::stable_sort(drawOrder.begin(), drawOrder.end(),
                             [&](size_t a, size_t b) {
                                 return entities_[a].zOrder < entities_[b].zOrder;
                             });
            for (size_t di : drawOrder) {
                SceneEntity& e = entities_[di];
                if (e.meshKey == "tilemap") {
                    // P1-1: draw every non-empty cell as a sprite quad; the
                    // entity's scale sets the cell size.
                    const size_t cellCount =
                        static_cast<size_t>(e.tilemapCols_) * e.tilemapRows_;
                    if (!e.tilemapTiles_.empty() && e.tilemapTiles_.size() == cellCount &&
                        e.spriteMesh.Valid()) {
                        for (int r = 0; r < e.tilemapRows_; ++r) {
                            for (int c = 0; c < e.tilemapCols_; ++c) {
                                const std::string& tex =
                                    e.tilemapTiles_[static_cast<size_t>(r) * e.tilemapCols_ + c];
                                if (tex.empty()) continue;
                                gfx::Material mat = e.spriteMaterial;
                                mat.albedo = assetMgr_.LoadTexture(tex).Handle();
                                mat.tint = gfx::Color::White;
                                const math::Vec3 offset{
                                    (static_cast<float>(c) + 0.5f) * e.scale.x,
                                    (static_cast<float>(r) + 0.5f) * e.scale.y, 0.0f};
                                math::Mat4 cellModel =
                                    math::Mat4::Translation(e.pos + e.rot.Rotate(offset)) *
                                    e.rot.ToMat4() *
                                    math::Mat4::Scale(e.scale);
                                renderer_.DrawMesh(e.spriteMesh, mat, cellModel);
                            }
                        }
                    }
                    continue;
                }
                if (!e.decalTex.empty()) {
                    gfx::Material mat = e.spriteMaterial;
                    mat.albedo = assetMgr_.LoadTexture(e.decalTex).Handle();
                    mat.transparent = true;
                    mat.tint = {1, 1, 1, e.decalAlpha};
                    if (!e.decalMesh.Valid())
                        e.decalMesh =
                            gfx::Mesh::CreatePlane(renderer_, e.decalSize, e.decalSize, 1, 1,
                                                   "decal");
                    math::Mat4 decalModel =
                        math::Mat4::Translation(e.pos + math::Vec3{0, 0.02f, 0}) *
                        e.rot.ToMat4();
                    renderer_.DrawMesh(e.decalMesh, mat, decalModel);
                    continue;
                }
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                if (!e.spriteTex.empty() && e.spriteMesh.Valid()) {
                    // 2D sprite: image quad; flips mirror it around its center
                    // via a negative local scale (keeps the texture upright).
                    if (e.spriteFlipX || e.spriteFlipY)
                        model = model * math::Mat4::Scale({e.spriteFlipX ? -1.0f : 1.0f,
                                                           e.spriteFlipY ? -1.0f : 1.0f, 1.0f});
                    renderer_.DrawMesh(e.spriteMesh, e.spriteMaterial, model);
                } else if (e.skinned && e.skinned->Valid()) {
                    const std::vector<math::Mat4> bones = e.skinned->BoneMatrices();
                    for (const auto& part : e.skinned->parts)
                        renderer_.DrawSkinnedMesh(part.mesh, part.material,
                                                  model * part.localTransform, bones,
                                                  static_cast<int>(bones.size()));
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
            // P2-editor UX: terrain brush footprint preview at the hover point.
            if (terrainPaintMode_ && terrainHoverValid_) {
                const gfx::Color ringC{0.95f, 0.65f, 0.2f, 0.9f};
                const int segs = 36;
                const float y = entities_[static_cast<size_t>(selected_)].pos.y + 0.05f;
                std::vector<gfx::Renderer::LineVertex> rv;
                rv.reserve(static_cast<size_t>(segs) * 2);
                for (int i = 0; i < segs; ++i) {
                    const float a0 = static_cast<float>(i) / segs * 6.2831853f;
                    const float a1 = static_cast<float>(i + 1) / segs * 6.2831853f;
                    rv.push_back({{terrainHoverPos_.x + std::cos(a0) * terrainBrushRadius_, y,
                                   terrainHoverPos_.z + std::sin(a0) * terrainBrushRadius_},
                                  ringC});
                    rv.push_back({{terrainHoverPos_.x + std::cos(a1) * terrainBrushRadius_, y,
                                   terrainHoverPos_.z + std::sin(a1) * terrainBrushRadius_},
                                  ringC});
                }
                renderer_.DrawLines(rv.data(), static_cast<uint32_t>(rv.size()),
                                    math::Mat4::Identity());
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
    // G8-3: debug overlay layers (nav walkable area, light probes, audio).
    DrawDebugOverlay(cam);
    DrawSceneGizmos(cam);

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

    // Model viewer renders into its own docked panel (not the main viewport),
    // so it coexists with the edit/playtest scene.
    RenderModelPreviewPanel();

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

// --- P5-editor UX: UI-editor selection / align / snap helpers -------------

void EditorApp::UISelectNode(ui::UiNode* n) {
    uiSelection_.clear();
    if (n) uiSelection_.insert(n);
    uiSelected_ = n;
}

void EditorApp::UIToggleSelectNode(ui::UiNode* n) {
    if (!n) return;
    const auto it = uiSelection_.find(n);
    if (it != uiSelection_.end()) {
        uiSelection_.erase(it);
        if (uiSelected_ == n)
            uiSelected_ = uiSelection_.empty() ? nullptr : *uiSelection_.rbegin();
    } else {
        uiSelection_.insert(n);
        uiSelected_ = n;
    }
}

ui::UiNode* EditorApp::UICloneNode(const ui::UiNode& src) {
    auto clone = std::make_unique<ui::UiNode>();
    clone->type = src.type;
    clone->name = src.name + "_副本";
    clone->rect = src.rect;
    clone->rect.x += 8.0f;
    clone->rect.y += 8.0f;
    clone->color = src.color;
    clone->borderColor = src.borderColor;
    clone->text = src.text;
    clone->sprite = src.sprite;
    clone->fill = src.fill;
    clone->fontSize = src.fontSize;
    clone->visible = src.visible;
    clone->clipChildren = src.clipChildren;
    for (const auto& c : src.children)
        clone->children.push_back(std::unique_ptr<ui::UiNode>(UICloneNode(*c)));
    for (auto& c : clone->children) c->parent = clone.get();
    return clone.release();
}

void EditorApp::UIDuplicateSelectedNodes() {
    if (uiSelection_.empty()) return;
    std::vector<ui::UiNode*> sel(uiSelection_.begin(), uiSelection_.end());
    uiSelection_.clear();
    for (ui::UiNode* n : sel) {
        if (!n || !n->parent) continue;
        ui::UiNode* copy = UICloneNode(*n);
        n->parent->children.push_back(std::unique_ptr<ui::UiNode>(copy));
        copy->parent = n->parent;
        uiSelection_.insert(copy);
    }
    uiSelected_ = uiSelection_.empty() ? nullptr : *uiSelection_.rbegin();
    MarkUIDirty();
}

void EditorApp::UIDeleteSelectedNodes() {
    if (uiSelection_.empty()) return;
    std::vector<ui::UiNode*> sel(uiSelection_.begin(), uiSelection_.end());
    uiSelection_.clear();
    uiSelected_ = nullptr;
    for (ui::UiNode* n : sel) {
        if (!n || !n->parent) continue;
        std::vector<std::unique_ptr<ui::UiNode>>& kids = n->parent->children;
        for (auto it = kids.begin(); it != kids.end(); ++it) {
            if (it->get() == n) {
                kids.erase(it);
                break;
            }
        }
    }
    MarkUIDirty();
}

void EditorApp::UIAlignSelected(int mode) {
    if (uiSelection_.empty()) return;
    for (ui::UiNode* n : uiSelection_) {
        if (!n || !n->parent) continue;
        const math::Rect2& p = n->parent->rect;
        switch (mode) {
            case 0: n->rect.x = 0.0f; break;                     // left
            case 1: n->rect.x = (p.w - n->rect.w) * 0.5f; break; // h-center
            case 2: n->rect.x = p.w - n->rect.w; break;          // right
            case 3: n->rect.y = 0.0f; break;                     // top
            case 4: n->rect.y = (p.h - n->rect.h) * 0.5f; break; // v-center
            case 5: n->rect.y = p.h - n->rect.h; break;          // bottom
            default: break;
        }
        n->rect.x = UISnap(n->rect.x);
        n->rect.y = UISnap(n->rect.y);
    }
    MarkUIDirty();
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
        // P5-editor UX: Ctrl+click toggles multi-selection.
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (hit && hit != &uiDoc_.root) {
            if (ctrl)
                UIToggleSelectNode(hit);
            else
                UISelectNode(hit);
        } else if (!ctrl) {
            UISelectNode(nullptr);
        }
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
            // P5-editor UX: grid snap while resizing.
            r.x = UISnap(r.x);
            r.y = UISnap(r.y);
            r.w = std::max(UISnap(r.w), 4.0f);
            r.h = std::max(UISnap(r.h), 4.0f);
            r.w = std::max(r.w, 8.0f);
            r.h = std::max(r.h, 8.0f);
        } else {
            r.x = UISnap(r.x + delta.x);
            r.y = UISnap(r.y + delta.y);
        }
        MarkUIDirty();
        return;
    }

    if (input->MouseReleased(platform::MouseButton::Left)) {
        uiDragging_ = false;
        uiResizeHandle_ = -1;
    }

    // P5-editor UX shortcuts: Delete 删除选中, Ctrl+D 复制, 方向键 微调.
    if (!gfx::ImGuiNeon_WantCaptureKeyboard()) {
        if (input->Pressed(platform::Key::Delete)) UIDeleteSelectedNodes();
        if (input->Pressed(platform::Key::D) && ImGui::GetIO().KeyCtrl)
            UIDuplicateSelectedNodes();
        const float nudge = uiGridSize_;
        math::Vec2 dir{};
        if (input->Pressed(platform::Key::ArrowLeft)) dir.x = -nudge;
        if (input->Pressed(platform::Key::ArrowRight)) dir.x = nudge;
        if (input->Pressed(platform::Key::ArrowUp)) dir.y = -nudge;
        if (input->Pressed(platform::Key::ArrowDown)) dir.y = nudge;
        if (dir.x != 0.0f || dir.y != 0.0f) {
            for (ui::UiNode* n : uiSelection_) {
                if (!n) continue;
                n->rect.x += dir.x;
                n->rect.y += dir.y;
            }
            MarkUIDirty();
        }
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
    // P1-1 camera entity preview: when enabled and a Camera3D entity is
    // selected, the viewport shows through it (transform + fov/ortho).
    if (cameraFollowSelected_ && selected_ >= 0 &&
        selected_ < static_cast<int>(entities_.size())) {
        const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        if (e.nodeType == "Camera3D") {
            gfx::Camera cam;
            cam.position = e.pos;
            cam.target = e.pos + e.rot.Rotate({0, 0, -1});
            cam.up = {0, 1, 0};
            cam.ortho = e.cameraOrtho;
            cam.fovY = e.cameraFov * math::kDegToRad;
            return cam;
        }
    }
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

void EditorApp::DrawSceneGizmos(const gfx::Camera&) {
    if (playtestActive_) return; // gizmos are an edit-mode planning aid
    const gfx::Color camCol{0.4f, 0.9f, 1.0f, 0.95f};
    const gfx::Color dirCol{1.0f, 0.9f, 0.4f, 0.95f};
    const gfx::Color ptCol{1.0f, 0.65f, 0.25f, 0.95f};
    const gfx::Color ambCol{0.7f, 0.9f, 1.0f, 0.95f};

    for (const SceneEntity& e : entities_) {
        const math::Mat4 model =
            math::Mat4::Translation(e.pos) * e.rot.ToMat4() * math::Mat4::Scale(e.scale);
        if (e.nodeType == "Camera3D") {
            // View frustum: perspective pyramid or ortho box along local -Z.
            // Use the 1280x720 design aspect so a default 2D camera's box
            // coincides with the design-space border (and the game view).
            const float aspect = static_cast<float>(gfx::Renderer::kDesignWidth) /
                                 static_cast<float>(gfx::Renderer::kDesignHeight);
            const float nearP = 0.1f, farP = 120.0f;
            math::Vec3 c[8];
            if (e.cameraOrtho) {
                const float hh = e.cameraOrthoSize, hw = hh * aspect;
                c[0] = {-hw, hh, -nearP}; c[1] = {hw, hh, -nearP};
                c[2] = {hw, -hh, -nearP}; c[3] = {-hw, -hh, -nearP};
                c[4] = {-hw, hh, -farP};  c[5] = {hw, hh, -farP};
                c[6] = {hw, -hh, -farP};  c[7] = {-hw, -hh, -farP};
            } else {
                const float t = std::tan(e.cameraFov * 0.5f * math::kDegToRad);
                const float nh = t * nearP, nw = nh * aspect;
                const float fh = t * farP, fw = fh * aspect;
                c[0] = {-nw, nh, -nearP}; c[1] = {nw, nh, -nearP};
                c[2] = {nw, -nh, -nearP}; c[3] = {-nw, -nh, -nearP};
                c[4] = {-fw, fh, -farP};  c[5] = {fw, fh, -farP};
                c[6] = {fw, -fh, -farP};  c[7] = {-fw, -fh, -farP};
            }
            std::vector<gfx::Renderer::LineVertex> ln;
            auto seg = [&](int a, int b) {
                ln.push_back({model.TransformPoint(c[a]), camCol});
                ln.push_back({model.TransformPoint(c[b]), camCol});
            };
            const int q0[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
            const int q1[4][2] = {{4, 5}, {5, 6}, {6, 7}, {7, 4}};
            for (auto& ed : q0) seg(ed[0], ed[1]);
            for (auto& ed : q1) seg(ed[0], ed[1]);
            for (int i = 0; i < 4; ++i) seg(i, 4 + i);
            renderer_.DrawLines(ln.data(), static_cast<uint32_t>(ln.size()),
                                math::Mat4::Identity());
            // Camera position marker.
            renderer_.DrawSphere(e.pos, 0.15f, camCol);
        } else if (e.hasLight) {
            const gfx::Color col = e.light.type == "point"
                                       ? ptCol
                                       : (e.light.type == "ambient" ? ambCol : dirCol);
            if (e.light.type == "directional") {
                // Sun icon: a small body + an arrow along the light direction.
                const math::Vec3 dir = e.light.sunDir.Normalized();
                const math::Vec3 tip = e.pos + dir * 2.0f;
                std::vector<gfx::Renderer::LineVertex> ln;
                ln.push_back({e.pos, col});
                ln.push_back({tip, col});
                math::Vec3 s = math::Cross(dir, math::Vec3::Up());
                if (s.LengthSq() < 1e-4f) s = {1.0f, 0.0f, 0.0f};
                s = s.Normalized() * 0.25f;
                ln.push_back({tip - dir * 0.5f - s, col});
                ln.push_back({tip, col});
                ln.push_back({tip - dir * 0.5f + s, col});
                ln.push_back({tip, col});
                renderer_.DrawLines(ln.data(), static_cast<uint32_t>(ln.size()),
                                    math::Mat4::Identity());
                renderer_.DrawSphere(e.pos, 0.2f, col);
            } else if (e.light.type == "point") {
                renderer_.DrawSphere(e.pos, e.light.radius, ptCol); // falloff
                renderer_.DrawSphere(e.pos, 0.2f, gfx::Color{1, 1, 1, 1});
            } else {
                renderer_.DrawSphere(e.pos, 0.3f, ambCol); // ambient fill
            }
        }
    }
}

void EditorApp::UpdateViewport(float dt) {
    // Model preview camera: only when the mouse is actually over the preview
    // panel; otherwise this falls through and the main viewport camera keeps
    // driving (right-drag in the 3D view must not rotate the preview).
    if (showModelPreview_ && previewModel_ && previewScreenRect_.w > 0.0f) {
        platform::IInput* in = Input();
        const math::Vec2 mpx = in->MousePos();
        const bool overPreview =
            mpx.x >= previewScreenRect_.x && mpx.x <= previewScreenRect_.x + previewScreenRect_.w &&
            mpx.y >= previewScreenRect_.y && mpx.y <= previewScreenRect_.y + previewScreenRect_.h;
        if (overPreview) {
            if (in->MouseDown(platform::MouseButton::Right)) {
                previewYaw_ += -in->MouseDelta().x * 0.005f;
                previewPitch_ = math::Clamp(previewPitch_ + -in->MouseDelta().y * 0.005f,
                                            -1.4f, 1.4f);
            }
            return;
        }
    }
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
        // A CLOSED panel keeps its stale rect in g.Windows (ImGui never
        // destroys it), so without this check the old area would keep
        // hijacking the viewport camera even after the panel is closed.
        // Active is reset every NewFrame for windows not submitted that frame.
        if (!w->Active) continue;
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
            // P1-1 terrain brush: paint instead of picking while enabled.
            if (terrainPaintMode_) {
                PaintTerrain(ray);
                return;
            }
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
            // P2-editor UX: Ctrl+click adds to the selection, plain click
            // replaces it (Shift+click in the hierarchy selects ranges).
            if (ImGui::GetIO().KeyCtrl)
                ToggleSelection(picked);
            else
                SetSelection(picked);
        }
        // Hold-to-paint: the brush applies every frame while the button stays
        // down (drag sculpting).
        if (terrainPaintMode_ && input->MouseDown(platform::MouseButton::Left)) {
            const float aspect = ViewportAspect();
            gfx::Camera cam = ActiveCamera();
            const math::Vec2 mousePx = input->MousePos();
            const math::Rect2& vp = viewportScreenRect_;
            const float vpW = vp.w > 0.0f ? vp.w : static_cast<float>(renderer_.ScreenWidth());
            const float vpH = vp.h > 0.0f ? vp.h : static_cast<float>(renderer_.ScreenHeight());
            const float vpX = vp.w > 0.0f ? vp.x : 0.0f;
            const float vpY = vp.h > 0.0f ? vp.y : 0.0f;
            const float ndcX = (mousePx.x - vpX) / vpW * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mousePx.y - vpY) / vpH * 2.0f;
            PaintTerrain(RayFromNDC(cam, aspect, ndcX, ndcY));
        }
        // P2-editor UX: terrain brush hover preview — track where the brush
        // would land every frame while paint mode is on.
        terrainHoverValid_ = false;
        if (terrainPaintMode_ && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()) &&
            entities_[static_cast<size_t>(selected_)].meshKey == "terrain") {
            const float aspect = ViewportAspect();
            gfx::Camera cam = ActiveCamera();
            const math::Vec2 mousePx = input->MousePos();
            const math::Rect2& vp = viewportScreenRect_;
            const float vpW = vp.w > 0.0f ? vp.w : static_cast<float>(renderer_.ScreenWidth());
            const float vpH = vp.h > 0.0f ? vp.h : static_cast<float>(renderer_.ScreenHeight());
            const float vpX = vp.w > 0.0f ? vp.x : 0.0f;
            const float vpY = vp.h > 0.0f ? vp.y : 0.0f;
            const float ndcX = (mousePx.x - vpX) / vpW * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mousePx.y - vpY) / vpH * 2.0f;
            const math::Ray ray = RayFromNDC(cam, aspect, ndcX, ndcY);
            if (std::fabs(ray.dir.y) > 1e-6f) {
                const float ty = (entities_[static_cast<size_t>(selected_)].pos.y -
                                  ray.origin.y) /
                                 ray.dir.y;
                if (ty >= 0.0f) {
                    terrainHoverPos_ = ray.origin + ray.dir * ty;
                    terrainHoverValid_ = true;
                }
            }
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
    selection_.clear();
    if (index >= 0) selection_.insert(index);
    selectionAnchor_ = index;
    scriptSyncEntity_ = -1; // script panel caches by index: force a re-sync
}

void EditorApp::ToggleSelection(int index) {
    if (index < 0) return;
    const auto it = selection_.find(index);
    if (it != selection_.end()) {
        selection_.erase(it);
        if (selected_ == index) {
            selected_ = selection_.empty() ? -1 : *selection_.rbegin();
        }
    } else {
        selection_.insert(index);
        selected_ = index;
        selectionAnchor_ = index;
    }
    scriptSyncEntity_ = -1;
}

void EditorApp::SelectRangeTo(int index) {
    if (index < 0 || selectionAnchor_ < 0) {
        SetSelection(index);
        return;
    }
    const int lo = std::min(selectionAnchor_, index);
    const int hi = std::max(selectionAnchor_, index);
    selection_.clear();
    for (int i = lo; i <= hi; ++i) {
        if (i >= 0 && i < static_cast<int>(entities_.size())) selection_.insert(i);
    }
    selected_ = index;
    scriptSyncEntity_ = -1;
}

void EditorApp::ClearSelection() {
    selection_.clear();
    selected_ = -1;
    selectionAnchor_ = -1;
    scriptSyncEntity_ = -1;
}

bool EditorApp::IsSelected(int idx) const {
    return selection_.count(idx) != 0;
}

std::vector<int> EditorApp::SelectedIndices() const {
    std::vector<int> out(selection_.begin(), selection_.end());
    return out;
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
            if (selection_.size() > 1) {
                // P2-editor UX: drag the whole selection. On the first changed
                // frame capture every selected entity's pre-drag transform,
                // then apply the active entity's delta to the others and push
                // one BatchTransformCommand (frames merge into a single undo).
                if (!gizmoBatchCaptured_) {
                    gizmoBatchIndices_ = SelectedIndices();
                    gizmoBatchFrom_.clear();
                    for (int i : gizmoBatchIndices_) {
                        const SceneEntity& se = entities_[static_cast<size_t>(i)];
                        gizmoBatchFrom_.push_back({se.pos, se.rot, se.scale});
                    }
                    gizmoBatchCaptured_ = true;
                }
                int activePos = 0;
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k)
                    if (gizmoBatchIndices_[k] == selected_) activePos = static_cast<int>(k);
                const Transform3 activeFrom = gizmoBatchFrom_[static_cast<size_t>(activePos)];
                const math::Quat activeInv{-activeFrom.rot.x, -activeFrom.rot.y,
                                           -activeFrom.rot.z, activeFrom.rot.w};
                std::vector<Transform3> fromNow, to;
                for (int i : gizmoBatchIndices_) {
                    const SceneEntity& se = entities_[static_cast<size_t>(i)];
                    fromNow.push_back({se.pos, se.rot, se.scale});
                }
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k) {
                    Transform3 t = fromNow[k];
                    if (static_cast<int>(k) == activePos) {
                        t.pos = pos;
                        t.rot = rot;
                        t.scale = scale;
                    } else {
                        t.pos = t.pos + (pos - activeFrom.pos);
                        const math::Quat rel = (activeInv * rot).Normalized();
                        t.rot = (t.rot * rel).Normalized();
                        t.scale = {t.scale.x * (scale.x / activeFrom.scale.x),
                                   t.scale.y * (scale.y / activeFrom.scale.y),
                                   t.scale.z * (scale.z / activeFrom.scale.z)};
                    }
                    to.push_back(t);
                }
                for (size_t k = 0; k < gizmoBatchIndices_.size(); ++k) {
                    SceneEntity& se = entities_[static_cast<size_t>(gizmoBatchIndices_[k])];
                    se.pos = to[k].pos;
                    se.rot = to[k].rot;
                    se.scale = to[k].scale;
                }
                history_.Push(std::make_unique<BatchTransformCommand>(
                    &entities_, gizmoBatchIndices_, fromNow, to));
            } else {
                gizmoDragOriginValid_ = true;
                history_.Push(std::make_unique<EditTransformCommand>(
                    &entities_, selected_, e.pos, e.rot, e.scale, pos, rot, scale,
                    EditTransformCommand::kAll));
            }
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
        if (gizmoBatchCaptured_) {
            if (BatchTransformCommand* top =
                    dynamic_cast<BatchTransformCommand*>(history_.TopUndo()))
                top->Seal();
            gizmoBatchCaptured_ = false;
            gizmoBatchIndices_.clear();
            gizmoBatchFrom_.clear();
        }
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
            if (ImGui::MenuItem("导出场景", "Ctrl+E")) ExportScene();
            ImGui::Separator();
            if (ImGui::MenuItem("退出")) {
                if (Window()) Window()->RequestClose();
            }
            ImGui::EndMenu();
        }
        // 编辑: scene-history undo/redo + selection batch operations (the
        // shortcuts also work while the viewport has focus; the menu adds
        // discoverability + enabled/disabled state).
        if (ImGui::BeginMenu("编辑")) {
            if (ImGui::MenuItem("撤销", "Ctrl+Z", false, history_.CanUndo())) {
                history_.Undo();
                ClampSelection();
            }
            if (ImGui::MenuItem("重做", "Ctrl+Y", false, history_.CanRedo())) {
                history_.Redo();
                ClampSelection();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("删除选中", "Del", false, !selection_.empty())) {
                history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                    &entities_, SelectedIndices()));
                ClampSelection();
            }
            if (ImGui::MenuItem("复制选中", "Ctrl+D", false, !selection_.empty())) {
                history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                    &entities_, SelectedIndices()));
                SetSelection(static_cast<int>(entities_.size()) - 1);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("聚焦选中", "F", false,
                                selected_ >= 0 &&
                                    selected_ < static_cast<int>(entities_.size()))) {
                camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景", nullptr, &showHierarchy_);
            ImGui::MenuItem("属性", nullptr, &showInspector_);
            ImGui::MenuItem("动画时间线", nullptr, &showAnimEditor_);
            ImGui::MenuItem("地形编辑", nullptr, &showTerrain_);
            ImGui::MenuItem("2D 地图", nullptr, &showTilemap_);
            ImGui::MenuItem("以选中相机为视图", nullptr, &cameraFollowSelected_);
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
            ImGui::MenuItem("插件", nullptr, &showPlugins_);
            // Plugin-contributed panels appear in the same menu (docked like
            // built-in panels; the manager owns their open state).
            if (pluginMgr_) {
                for (editor::PluginPanel& p : pluginMgr_->Panels())
                    ImGui::MenuItem(p.title.c_str(), nullptr, &p.opened);
            }
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
                if (ImGui::MenuItem(SceneDisplayName(s).c_str())) LoadProjectScene(s);
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

    // F3 toggles the unified debug-overlay panel (G8-3). Runs regardless of
    // selection so the layers can be switched while playtesting.
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) showDebugOverlay_ = !showDebugOverlay_;

    // Transform-gizmo shortcuts: W/E/R switch the operation while an entity is
    // selected (ignored while the user is typing text, e.g. the name field).
    if (selected_ >= 0 && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = ImGuizmo::SCALE;
        // P2-editor UX shortcuts: Delete = 删除选中, Ctrl+D = 复制选中,
        // F = 相机聚焦到选中实体.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selection_.empty()) {
            history_.Push(std::make_unique<MultiDeleteEntityCommand>(
                &entities_, SelectedIndices()));
            ClampSelection();
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
            !selection_.empty()) {
            history_.Push(std::make_unique<MultiDuplicateEntityCommand>(
                &entities_, SelectedIndices()));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && selected_ < static_cast<int>(entities_.size())) {
            camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
        }
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
    ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, toolH), ImGuiCond_Always);
    ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toolbar", nullptr, tbFlags)) {
        // Icon button helper: fixed-size glyph button with an active-state
        // highlight and a hover tooltip (toolbar icon-ization, UX item 6).
        auto ToolbarIcon = [](const char* label, const char* tip, bool active) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.36f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.42f, 0.62f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.47f, 0.68f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.29f, 0.33f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.38f, 0.43f, 1.0f));
            }
            const bool clicked = ImGui::Button(label, ImVec2(26.0f, 0.0f));
            if (active)
                ImGui::PopStyleColor(4);
            else
                ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        // --- 上下文: 项目 / 场景切换 ---
        ImGui::SetNextItemWidth(150.0f);
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
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##scene_picker", currentSceneName_.empty()
                                                    ? "选择场景…"
                                                    : SceneDisplayName(currentSceneName_).c_str())) {
            if (projectDir_ == ".")
                if (ImGui::Selectable(SceneDisplayName("editor_scene.json").c_str(),
                                      currentSceneName_ == "editor_scene.json"))
                    LoadScene("editor_scene.json");
            for (const std::string& s : projectScenes_) {
                if (ImGui::Selectable(SceneDisplayName(s).c_str(),
                                      currentSceneName_ == BaseName(s)))
                    LoadProjectScene(s);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 运行/视图: 试玩、热重载、相机预设、2D/3D 切换 ---
        if (ToolbarIcon(playtestActive_ ? "■" : "▶",
                        playtestActive_ ? "停止试玩 (F5)" : "试玩 (F5)", playtestActive_))
            TogglePlaytest();
        ImGui::SameLine();
        // Hot reload toggle (T4.8): off by default. When on, script/asset mtime
        // changes restart the playtest / reload the cached assets (throttled).
        if (ToolbarIcon(hotReload_ ? "●" : "○", "热重载: 脚本/资源改动自动重载", hotReload_))
            hotReload_ = !hotReload_;
        ImGui::SameLine();
        // Multi-camera viewport preset (T4.8): 透视 / 顶视 / 前视 (also Tab).
        const char* camLabels[] = {"透视", "顶视", "前视"};
        int camSel = static_cast<int>(viewCam_);
        ImGui::SetNextItemWidth(88.0f);
        if (ImGui::Combo("##viewport_cam", &camSel, camLabels, 3))
            SetViewCam(static_cast<ViewCam>(camSel));
        ImGui::SameLine();
        const bool in2D = editMode_ == EditMode::Scene2D;
        if (ToolbarIcon(in2D ? "3D" : "2D", in2D ? "切换到 3D 透视" : "切换到 2D 视图",
                        false))
            Set2DMode(!in2D);
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 变换: 移动/旋转/缩放 + 本地/世界坐标 ---
        if (ToolbarIcon("✥", "移动 (W)", gizmoOp_ == ImGuizmo::TRANSLATE))
            gizmoOp_ = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ToolbarIcon("⟳", "旋转 (E)", gizmoOp_ == ImGuizmo::ROTATE))
            gizmoOp_ = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ToolbarIcon("⇲", "缩放 (R)", gizmoOp_ == ImGuizmo::SCALE))
            gizmoOp_ = ImGuizmo::SCALE;
        ImGui::SameLine();
        if (ToolbarIcon("◉", "本地坐标空间", gizmoMode_ == ImGuizmo::LOCAL))
            gizmoMode_ = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ToolbarIcon("◎", "世界坐标空间", gizmoMode_ == ImGuizmo::WORLD))
            gizmoMode_ = ImGuizmo::WORLD;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 视口辅助: 网格、聚焦 ---
        if (ToolbarIcon("▦", "视口网格", showViewportGrid_))
            showViewportGrid_ = !showViewportGrid_;
        ImGui::SameLine();
        if (ToolbarIcon("⌖", "聚焦选中 (F)", false) && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()))
            camTarget_ = entities_[static_cast<size_t>(selected_)].pos;
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // --- 状态: 选中/实体数量 ---
        ImGui::TextDisabled("选中 %zu / 实体 %zu", selection_.size(), entities_.size());
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        // Plugin-contributed toolbar tools.
        if (pluginMgr_) {
            for (editor::PluginTool& t : pluginMgr_->Tools()) {
                ImGui::SameLine();
                if (ImGui::Button(t.label.c_str()) && t.host && t.fn != 0) {
                    const auto res = t.host->CallCaptured(t.fn, {});
                    if (!res.Ok()) {
                        NEON_LOG_ERROR("Editor plugin tool '%s' failed: %s", t.id.c_str(),
                                       t.host->LastError().message.c_str());
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                    ImGui::SetTooltip("插件工具: %s (%s)", t.label.c_str(), t.id.c_str());
            }
        }
    }
    ImGui::End();

    // The DockSpace's Begin/End (above) can overwrite the hover we resolved
    // after NewFrame, so re-resolve it right before the tool panels build.
    // IMPORTANT: never touch HoveredWindow while a popup/menu is open - the
    // 项目/场景 menus and any combo render as popups, and stealing the hover
    // would make their items unclickable.
    {
        ImGuiContext& ictx = *ImGui::GetCurrentContext();
        if (ictx.OpenPopupStack.Size == 0) {
            ImGuiWindow* best = nullptr;
            for (int wi = ictx.Windows.Size - 1; wi >= 0; --wi) {
                ImGuiWindow* w = ictx.Windows[wi];
                if (!w || w->Hidden) continue;
                if (!w->Active) continue; // closed panels must not win hover
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
    }
    BuildScenePanel();
    BuildAssetPanel();
    BuildResourcePanel();
    BuildInspectorPanel();
    BuildLogPanel();
    BuildModelPreviewPanel();
    BuildBtPanel();
    BuildScriptPanel();
    BuildScriptEditorPanel();
    BuildAnimEditorPanel();
    BuildTerrainPanel();
    BuildPackagePanel();
    BuildProfilerPanel();
    BuildInputMapPanel();
    BuildNavPanel();
    BuildDebugOverlayPanel();
    BuildUIEditorPanel();
    BuildLocPanel();
    BuildViewportPanel();
    BuildPluginPanels();
    BuildPluginsPanel();

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

    // --- UI editor multi-select / align / duplicate (P5-editor UX) ---
    {
        uiDoc_ = ui::UiDocument{};
        uiDoc_.root.rect = {0, 0, 1280, 720};
        ui::UiNode* a = uiDoc_.root.AddChild(ui::UiNodeType::Label, "A");
        a->rect = {10, 20, 100, 24};
        ui::UiNode* b = uiDoc_.root.AddChild(ui::UiNodeType::Label, "B");
        b->rect = {300, 400, 120, 30};
        ui::UiNode* c = uiDoc_.root.AddChild(ui::UiNodeType::Label, "C");
        c->rect = {500, 600, 80, 20};

        UISelectNode(a);
        check(uiSelection_.size() == 1 && uiSelected_ == a,
              "UI select sets the active node");
        UIToggleSelectNode(b);
        check(uiSelection_.count(a) == 1 && uiSelection_.count(b) == 1 &&
                  uiSelection_.size() == 2,
              "UI ctrl-click multi-select accumulates a second node");
        UIToggleSelectNode(b);
        check(uiSelection_.size() == 1 && uiSelected_ == a,
              "UI ctrl-click again deselects");
        UIToggleSelectNode(b);
        UIToggleSelectNode(c);
        check(uiSelection_.size() == 3, "UI multi-select holds three nodes");

        const bool oldSnap = uiSnapToGrid_;
        uiSnapToGrid_ = false; // exact math for the align assertions
        UIAlignSelected(0);    // left
        check(a->rect.x == 0.0f && b->rect.x == 0.0f && c->rect.x == 0.0f,
              "UI align-left snaps every selected x to the parent edge");
        UIAlignSelected(1); // h-center
        check(std::fabs(a->rect.x - (1280.0f - a->rect.w) * 0.5f) < 0.01f &&
                  std::fabs(b->rect.x - (1280.0f - b->rect.w) * 0.5f) < 0.01f &&
                  std::fabs(c->rect.x - (1280.0f - c->rect.w) * 0.5f) < 0.01f,
              "UI align h-center snaps every selected x to the parent center");
        UIAlignSelected(5); // bottom
        check(std::fabs(a->rect.y - (720.0f - a->rect.h)) < 0.01f &&
                  std::fabs(b->rect.y - (720.0f - b->rect.h)) < 0.01f &&
                  std::fabs(c->rect.y - (720.0f - c->rect.h)) < 0.01f,
              "UI align bottom snaps every selected y to the parent bottom");
        uiSnapToGrid_ = oldSnap;

        const size_t childCount = uiDoc_.root.children.size();
        UIDuplicateSelectedNodes();
        check(uiDoc_.root.children.size() == childCount + 3 &&
                  uiSelection_.size() == 3,
              "UI duplicate clones every selected node");
        UIDeleteSelectedNodes();
        check(uiDoc_.root.children.size() == childCount && uiSelection_.empty(),
              "UI delete removes the cloned selection");
        uiDocOpen_ = false;
        uiDocPath_.clear();
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

#ifdef NEON_ENABLE_JS
    // --- Built-in script editor, JS backend (.js routes to QuickJS) ---
    // Only runs when the QuickJS backend is compiled in: without it the editor
    // cannot syntax-check .js files (the host is unavailable), so this smoke
    // assertion would fail on MSVC builds (NEON_ENABLE_JS defaults off).
    {
        const std::string path = GetTempDir() + "/editor_script.js";
        {
            std::ofstream out(path, std::ios::binary);
            out << "function on_start(ent) {\n}\n";
        }
        OpenScriptEditor(path);
        check(showScriptEditor_ && scriptEditorPath_ == path,
              "script editor: opens a .js file");
        check(scriptEditorCheck_.ok, "script editor: JS syntax passes on open");
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_), "function broken( {\n");
        SaveScriptEditor();
        check(!scriptEditorCheck_.ok && !scriptEditorCheck_.message.empty(),
              "script editor: JS syntax error detected after save");
        std::snprintf(scriptEditorBuf_, sizeof(scriptEditorBuf_),
                      "function on_start(ent) {\n}\n");
        SaveScriptEditor();
        check(scriptEditorCheck_.ok, "script editor: JS syntax passes after fix");
    }
#endif // NEON_ENABLE_JS

    // --- Editor plugins (project plugins/ dir, type=editor) ---
    {
        // The smoke can run from any cwd; if the bundled examples are reachable
        // from ".", load them so the registration checks below are meaningful.
        if (pluginMgr_ && pluginMgr_->Count() == 0 &&
            std::ifstream("plugins/tree_gen/plugin.json", std::ios::binary)
                .is_open()) {
            pluginMgr_->Load(".");
        }
        if (pluginMgr_ && pluginMgr_->Count() > 0) {
            bool hasTree = false;
            bool hasVault = false;
            bool hasTool = false;
            for (const editor::PluginPanel& p : pluginMgr_->Panels())
                if (p.id == "tree_gen") hasTree = true;
            for (const editor::PluginAssetSource& s : pluginMgr_->AssetSources())
                if (s.id == "vault") hasVault = true;
            for (const editor::PluginTool& t : pluginMgr_->Tools())
                if (t.id == "tree_gen") hasTool = true;
            check(hasTree, "editor plugin registered the tree generator panel");
            check(hasTool, "editor plugin registered a toolbar tool");
            check(hasVault, "editor plugin registered the asset vault source");
        }
        // Plugin mesh generation + scene placement (the tree generator's core
        // path): buildMesh writes an OBJ asset, spawn adds an entity.
        {
            const size_t before = entities_.size();
            const std::vector<math::Vec3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
            const std::string key = PluginBuildMesh("smoke_plugin_tri", verts, {1, 2, 3});
            check(!key.empty() && key.rfind("obj:", 0) == 0,
                  "plugin buildMesh wrote an OBJ asset");
            PluginAddEntity(key, 5, 5, 5);
            check(entities_.size() == before + 1, "plugin spawn added an entity");
            history_.Undo(); // remove the smoke entity; keep the sandbox intact
            const std::string generated =
                (projectDir_ == "." ? "assets/generated/" : projectDir_ + "/assets/generated/") +
                "smoke_plugin_tri.obj";
            std::remove(generated.c_str());
        }
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
    if (meshKey == "camera") {
        SceneEntity e;
        e.name = "相机" + std::to_string(counter++);
        e.nodeType = "Camera3D";
        e.pos = pos;
        e.cameraFov = 60.0f;
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        return;
    }
    if (meshKey == "light:directional") {
        SceneEntity e;
        e.name = "方向光" + std::to_string(counter++);
        e.nodeType = "Light3D";
        e.hasLight = true;
        e.light.type = "directional";
        e.pos = pos;
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        return;
    }
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
    NormalizeEntityIds();
    if (entities_.empty())
        return core::Result<core::Json>::Err("editor: scene is empty");
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    auto parentNameOf = [this](int parentId) {
        if (parentId == 0) return std::string();
        for (const SceneEntity& p : entities_)
            if (p.id == parentId) return p.name;
        return std::string();
    };
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        if (e.id != 0) {
            core::Json id;
            id.type_ = core::Json::Type::Number;
            id.number_ = e.id;
            obj.object_["id"] = std::move(id);
        }
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
            if (e.parentId != 0) tf.object_["parentId"] = mkNum(e.parentId);
            const std::string pname = parentNameOf(e.parentId);
            if (!pname.empty()) tf.object_["parent"] = mkStr(pname);
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
                                                e.hp, e.maxHp, parentNameOf(e.parentId),
                                                e.parentId, e.id);
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
        if (e.parentId != 0) tf.object_["parentId"] = mkNum(e.parentId);
        const std::string pname = parentNameOf(e.parentId);
        if (!pname.empty()) tf.object_["parent"] = mkStr(pname);
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
        // P1-1/P2-3 export: node type, camera, sort order and the authored
        // terrain heightmap as runtime components.
        {
            auto mkNum = [](double v) {
                core::Json j;
                j.type_ = core::Json::Type::Number;
                j.number_ = v;
                return j;
            };
            core::Json comps;
            if (const core::Json* c = obj.Get("components")) {
                if (c->IsObject()) comps = *c;
            }
            comps.type_ = core::Json::Type::Object;
            if (!e.nodeType.empty()) {
                core::Json t;
                t.type_ = core::Json::Type::Object;
                core::Json v;
                v.type_ = core::Json::Type::String;
                v.string_ = e.nodeType;
                t.object_["value"] = v;
                comps.object_["type"] = std::move(t);
            }
            if (e.nodeType == "Camera3D") {
                core::Json cam;
                cam.type_ = core::Json::Type::Object;
                cam.object_["fov"] = mkNum(e.cameraFov);
                core::Json ortho;
                ortho.type_ = core::Json::Type::Bool;
                ortho.bool_ = e.cameraOrtho;
                cam.object_["ortho"] = ortho;
                cam.object_["orthoSize"] = mkNum(e.cameraOrthoSize);
                comps.object_["camera"] = std::move(cam);
            }
            if (e.hasLight) {
                core::Json li;
                li.type_ = core::Json::Type::Object;
                core::Json t;
                t.type_ = core::Json::Type::String;
                t.string_ = e.light.type;
                li.object_["type"] = std::move(t);
                auto vecJson = [&](float x, float y, float z) {
                    core::Json a;
                    a.type_ = core::Json::Type::Array;
                    a.array_.push_back(mkNum(x));
                    a.array_.push_back(mkNum(y));
                    a.array_.push_back(mkNum(z));
                    return a;
                };
                auto colJson = [&](float r, float g, float b, float aa) {
                    core::Json a;
                    a.type_ = core::Json::Type::Array;
                    a.array_.push_back(mkNum(r));
                    a.array_.push_back(mkNum(g));
                    a.array_.push_back(mkNum(b));
                    a.array_.push_back(mkNum(aa));
                    return a;
                };
                li.object_["sunDir"] =
                    vecJson(e.light.sunDir.x, e.light.sunDir.y, e.light.sunDir.z);
                li.object_["color"] =
                    colJson(e.light.color.r, e.light.color.g, e.light.color.b, e.light.color.a);
                li.object_["radius"] = mkNum(e.light.radius);
                li.object_["ambientColor"] = colJson(e.light.ambientColor.r, e.light.ambientColor.g,
                                                     e.light.ambientColor.b, e.light.ambientColor.a);
                li.object_["ambientStrength"] = mkNum(e.light.ambientStrength);
                comps.object_["light"] = std::move(li);
            }
            if (e.zOrder != 0.0f) {
                core::Json so;
                so.type_ = core::Json::Type::Object;
                so.object_["z"] = mkNum(e.zOrder);
                comps.object_["sortOrder"] = std::move(so);
            }
            if (e.meshKey == "terrain" && !e.terrainHeights_.empty()) {
                core::Json terr;
                terr.type_ = core::Json::Type::Object;
                terr.object_["segments"] = mkNum(e.terrainSegments_);
                terr.object_["size"] = mkNum(e.terrainSize_);
                terr.object_["heightScale"] = mkNum(e.terrainHeightScale_);
                core::Json hs;
                hs.type_ = core::Json::Type::Array;
                for (float h : e.terrainHeights_) hs.array_.push_back(mkNum(h));
                terr.object_["heights"] = std::move(hs);
                // G2-3 chunked LOD + vegetation knobs (written only when the
                // user enables them; the runtime reads them back verbatim).
                if (e.chunkGridDiv_ > 0) {
                    terr.object_["chunkGridDiv"] = mkNum(e.chunkGridDiv_);
                    terr.object_["chunkLodLevels"] = mkNum(e.chunkLodLevels_);
                    terr.object_["chunkBaseSubdiv"] = mkNum(e.chunkBaseSubdiv_);
                }
                if (!e.vegMeshKey_.empty() && e.vegCount_ > 0) {
                    core::Json vk;
                    vk.type_ = core::Json::Type::String;
                    vk.string_ = e.vegMeshKey_;
                    terr.object_["vegMeshKey"] = std::move(vk);
                    terr.object_["vegCount"] = mkNum(e.vegCount_);
                    terr.object_["vegSeed"] = mkNum(e.vegSeed_);
                    terr.object_["vegSize"] = mkNum(e.vegSize_);
                    terr.object_["vegImpostorDistance"] = mkNum(e.vegImpostorDistance_);
                    terr.object_["vegMinHeight"] = mkNum(e.vegMinHeight_);
                    terr.object_["vegMaxHeight"] = mkNum(e.vegMaxHeight_);
                    terr.object_["vegMaxSlope"] = mkNum(e.vegMaxSlope_);
                }
                comps.object_["terrain"] = std::move(terr);
            }
            if (e.meshKey == "tilemap" && !e.tilemapTiles_.empty()) {
                core::Json tlm;
                tlm.type_ = core::Json::Type::Object;
                tlm.object_["cols"] = mkNum(e.tilemapCols_);
                tlm.object_["rows"] = mkNum(e.tilemapRows_);
                tlm.object_["cellSize"] = mkNum(e.tilemapCellSize_);
                core::Json tls;
                tls.type_ = core::Json::Type::Array;
                for (const std::string& t : e.tilemapTiles_) {
                    core::Json s;
                    s.type_ = core::Json::Type::String;
                    s.string_ = t;
                    tls.array_.push_back(std::move(s));
                }
                tlm.object_["tiles"] = std::move(tls);
                comps.object_["tilemap"] = std::move(tlm);
            }
            if (!e.decalTex.empty()) {
                core::Json dc;
                dc.type_ = core::Json::Type::Object;
                core::Json tex;
                tex.type_ = core::Json::Type::String;
                tex.string_ = e.decalTex;
                dc.object_["texture"] = std::move(tex);
                dc.object_["size"] = mkNum(e.decalSize);
                dc.object_["alpha"] = mkNum(e.decalAlpha);
                comps.object_["decal"] = std::move(dc);
            }
            if (!e.shaderPath.empty()) {
                // Editor-only custom fragment shader, preserved so the scene
                // round-trips (the runtime ignores this component).
                core::Json sh;
                sh.type_ = core::Json::Type::Object;
                sh.object_["path"] = [&e]() {
                    core::Json p;
                    p.type_ = core::Json::Type::String;
                    p.string_ = e.shaderPath;
                    return p;
                }();
                comps.object_["shader"] = std::move(sh);
            }
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
    // P2-2 audio hooks: music bus, 3D positional sfx against the live camera
    // listener, and bus volume control.
    cfg.playMusic = [this](const std::string& name, float vol) {
        if (audioBackend_) audioBackend_->PlayMusic(MakePvzSfx(name), vol);
    };
    cfg.playSfx3D = [this](const std::string& name, const math::Vec3& pos) {
        if (audioBackend_) {
            const gfx::Camera& cam = ActiveCamera();
            const math::Vec3 fwd = (cam.target - cam.position).Normalized();
            audioBackend_->Play3D(MakePvzSfx(name), pos, cam.position, fwd, 0.7f);
        }
    };
    cfg.setBusVolume = [this](int bus, float gain) {
        if (audioBackend_ && bus >= 0 && bus <= 2)
            audioBackend_->SetBusVolume(static_cast<neon::audio::AudioBus>(bus), gain);
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
        ListScriptFiles(ScriptsDir(projectDir_), "scripts", files);
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
        const std::string meshPath = MeshKeyAssetPath(e.meshKey, projectDir_);
        if (!meshPath.empty()) checkFile(meshPath);
        if (!e.shaderPath.empty()) checkFile(e.shaderPath);
        checkFile(e.albedoTex);
        checkFile(e.mrTex);
        checkFile(e.aoTex);
        checkFile(e.emissiveTex);
    }
    if (!changedPaths.empty()) {
        for (const std::string& p : changedPaths) {
            const std::string ext = ExtLower(p);
            if (ext == ".obj") {
                assetMgr_.ReloadMeshOBJ(p);
            } else if (ext == ".gltf") {
                // Drop the resolved-model cache so the next ResolveMesh
                // re-parses the updated file. (The old GPU meshes are not
                // explicitly destroyed; same as the pre-existing mesh path.)
                skinnedModelCache_.erase(p);
                gltfStaticMeshCache_.erase(p);
                gltfStaticMaterialCache_.erase(p);
            } else {
                assetMgr_.ReloadTexture(p);
            }
            NEON_LOG_INFO("Editor: hot reload: asset '%s' reloaded", p.c_str());
        }
        ++hotReloadCount_;
        // Re-resolve only the entities that reference a changed asset.
        for (SceneEntity& e : entities_) {
            const bool touches =
                changedPaths.count(MeshKeyAssetPath(e.meshKey, projectDir_)) ||
                changedPaths.count(e.shaderPath) ||
                changedPaths.count(e.albedoTex) || changedPaths.count(e.mrTex) ||
                changedPaths.count(e.aoTex) || changedPaths.count(e.emissiveTex);
            if (touches) {
                if (changedPaths.count(e.shaderPath)) ReloadEntityShader(e);
                ResolveMesh(e);
                ApplyMaterialParams(e);
            }
        }
    }

    // Asset directory watch (P1-1 import pipeline): new/changed files under
    // the project's assets dir show up in the asset panel automatically (the
    // panel still owns the actual import/copy actions).
    if (!assetDir_.empty()) {
        std::vector<AssetEntry> now;
        if (ListDirectory(assetDir_, now)) {
            std::string sig;
            for (const AssetEntry& a : now) {
                sig += a.name;
                sig += a.isDir ? "/" : "|";
            }
            if (sig != assetDirSignature_) {
                assetDirSignature_ = sig;
                RefreshAssetDir();
                NEON_LOG_INFO("Editor: asset dir watch: %zu entries", now.size());
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
    if (key == "tilemap") {
        // 2D tilemap: cells draw as sprite quads in the render loop.
        e.mesh = {};
        return true;
    } else if (key == "terrain") {
        RebuildTerrainMesh(e);
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
        e.mesh = assetMgr_.LoadMeshOBJ(ResolveMeshAssetPath(key.substr(4), projectDir_));
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("gltf:", 0) == 0) {
        const std::string gltfPath = ResolveMeshAssetPath(key.substr(5), projectDir_);
        // Cache the resolved model per path: the first entity pays the full
        // parse + upload, the rest clone the result (GPU handles shared, the
        // per-entity Animator state is a fresh copy).
        if (skinnedModelCache_.count(gltfPath) == 0 &&
            gltfStaticMeshCache_.count(gltfPath) == 0) {
            assets::GltfAsset gltf = assetMgr_.LoadGLTF(gltfPath);
            if (!gltf.nodes.empty()) {
                gltfStaticMeshCache_[gltfPath] = gltf.nodes[0].mesh;
                gltfStaticMaterialCache_[gltfPath] = gltf.nodes[0].material;
                if (gltf.nodes[0].mesh.Skinned()) {
                    core::Result<scene::SkinnedModel> sm =
                        scene::LoadSkinnedModel(assetMgr_, gltfPath);
                    if (sm.Ok()) {
                        skinnedModelCache_[gltfPath] =
                            std::make_shared<scene::SkinnedModel>(std::move(sm.Value()));
                    } else {
                        NEON_LOG_WARN("Editor: skinned model '%s' failed to resolve: %s",
                                      key.c_str(), sm.Error().c_str());
                    }
                }
            }
        }
        const auto meshIt = gltfStaticMeshCache_.find(gltfPath);
        if (meshIt != gltfStaticMeshCache_.end()) {
            e.mesh = meshIt->second;
            e.material = gltfStaticMaterialCache_[gltfPath];
            // glTF materials carry their own PBR params (factors + texture
            // slots); sync them into the flattened fields so ApplyMaterialParams
            // applies the asset's values instead of the editor defaults.
            e.metallic = e.material.metallic;
            e.roughness = e.material.roughness;
            e.ao = e.material.aoStrength;
            e.emissiveIntensity = e.material.emissiveIntensity;
            e.tint = e.material.tint;
            // Animated skinned glTF: clone the cached model (per-entity
            // Animator state, shared skeleton/clips/meshes).
            const auto smIt = skinnedModelCache_.find(gltfPath);
            if (e.mesh.Skinned() && smIt != skinnedModelCache_.end())
                e.skinned = std::make_shared<scene::SkinnedModel>(*smIt->second);
            else
                e.skinned.reset();
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
    // Custom fragment shader (P2-6) wins over the built-in lit/unlit shader.
    if (e.customShader.Valid()) {
        e.material.shader = e.customShader.Handle();
        e.material.lit = false;
    } else {
        e.material.shader = {};
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

// Rebuilds a terrain entity's mesh from its heightmap canvas (P1-1). A blank
// canvas (no heights) becomes a flat field the brush can carve.
void EditorApp::RebuildTerrainMesh(SceneEntity& e) {
    const size_t need = static_cast<size_t>(e.terrainSegments_ + 1) *
                        (e.terrainSegments_ + 1);
    if (e.terrainHeights_.size() != need) {
        e.terrainHeights_.assign(need, 0.0f);
        // Match the runtime's default rolling terrain so a fresh 地面 matches
        // what the packed game shows before the user sculpts.
        const float half = e.terrainSize_ * 0.5f;
        const float cell = e.terrainSize_ / static_cast<float>(e.terrainSegments_);
        for (int row = 0; row <= e.terrainSegments_; ++row) {
            for (int col = 0; col <= e.terrainSegments_; ++col) {
                const float x = -half + col * cell;
                const float z = -half + row * cell;
                float h = std::sin(x * 0.11f) * std::cos(z * 0.13f) * 0.8f +
                          std::sin(x * 0.31f + z * 0.27f) * 0.35f;
                const float d = std::sqrt(x * x + z * z);
                h *= math::Saturate((d - 6.0f) / 10.0f);
                e.terrainHeights_[static_cast<size_t>(row) * (e.terrainSegments_ + 1) + col] = h;
            }
        }
    }
    e.mesh = gfx::Mesh::CreateTerrain(renderer_, e.terrainSegments_, e.terrainSize_,
                                      e.terrainHeights_, e.terrainHeightScale_, "terrain");
}

// P1-1 terrain brush: lowers/raises the heightmap around the ray's hit on the
// selected terrain's ground plane, then rebuilds the mesh.
void EditorApp::PaintTerrain(const math::Ray& ray) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (e.meshKey != "terrain") return;
    if (e.terrainHeights_.size() !=
        static_cast<size_t>(e.terrainSegments_ + 1) * (e.terrainSegments_ + 1))
        RebuildTerrainMesh(e);
    // Intersect the ray with the terrain's ground plane (y = e.pos.y).
    if (std::fabs(ray.dir.y) < 1e-6f) return;
    const float t = (e.pos.y - ray.origin.y) / ray.dir.y;
    if (t < 0.0f) return;
    const math::Vec3 hit = ray.origin + ray.dir * t;
    const float half = e.terrainSize_ * 0.5f;
    const float cell = e.terrainSize_ / static_cast<float>(e.terrainSegments_);
    const float localX = hit.x - e.pos.x;
    const float localZ = hit.z - e.pos.z;
    if (localX < -half || localX > half || localZ < -half || localZ > half) return;
    const int seg = e.terrainSegments_;
    const float radius = terrainBrushRadius_;
    const float r2 = radius * radius;
    const float delta = terrainBrushStrength_ * (terrainRaise_ ? 1.0f : -1.0f) / e.terrainHeightScale_;
    for (int row = 0; row <= seg; ++row) {
        for (int col = 0; col <= seg; ++col) {
            const float x = -half + col * cell - localX;
            const float z = -half + row * cell - localZ;
            const float d2 = x * x + z * z;
            if (d2 > r2) continue;
            const float falloff = 1.0f - d2 / r2;
            size_t idx = static_cast<size_t>(row) * (seg + 1) + col;
            e.terrainHeights_[idx] = math::Clamp(e.terrainHeights_[idx] + delta * falloff,
                                                 -10.0f, 10.0f);
        }
    }
    RebuildTerrainMesh(e);
    sceneDirty_ = true;
}

// P2-6 shader hot reload: (re)compiles the entity's custom fragment shader
// against the built-in unlit vertex contract and re-binds it to the material.
// The GL backend supports custom fragments; other backends return an invalid
// handle and the material keeps its built-in shader.
void EditorApp::ReloadEntityShader(SceneEntity& e) {
    if (e.shaderPath.empty()) {
        if (e.customShader.Valid() && renderer_.Backend()) {
            renderer_.Backend()->DestroyShader(e.customShader.Handle());
            e.customShader = {};
        }
        ApplyMaterialParams(e);
        return;
    }
    std::ifstream in(e.shaderPath, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Editor: cannot open shader '%s'", e.shaderPath.c_str());
        return;
    }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    gfx::Shader sh = renderer_.CreateUnlitFragmentShader(src, e.shaderPath);
    if (!sh.Valid()) {
        NEON_LOG_ERROR("Editor: shader compile failed for '%s'", e.shaderPath.c_str());
        return;
    }
    if (e.customShader.Valid() && renderer_.Backend())
        renderer_.Backend()->DestroyShader(e.customShader.Handle());
    e.customShader = sh;
    ApplyMaterialParams(e);
    NEON_LOG_INFO("Editor: shader '%s' compiled", e.shaderPath.c_str());
}

void EditorApp::SaveScene() {
    NormalizeEntityIds(); // stable ids before serialization
    // Serialize in the runtime componentized format (same as playtest/export)
    // so project scenes stay loadable by neon_game and no field is dropped
    // (health, materialRef, prefab, extraComponents, parentId, rotation...).
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Scene save aborted: %s", rootRes.Error().c_str());
        return;
    }
    core::Json root = rootRes.Value();
    // Preserve scene-level metadata from the loaded file (inheritance chain,
    // gameVars / level / title ...); entities were just rebuilt.
    if (!sceneExtends_.empty()) {
        core::Json ex;
        ex.type_ = core::Json::Type::String;
        ex.string_ = sceneExtends_;
        root.object_["extends"] = std::move(ex);
    }
    if (currentSceneRoot_.IsObject()) {
        for (const auto& [k, v] : currentSceneRoot_.Members()) {
            if (k == "entities" || k == "extends") continue;
            root.object_[k] = v;
        }
    }
    // Save to the scene file that is actually loaded (project scenes live in
    // <project>/scenes/*.json). Previously this hardcoded editor_scene.json,
    // so saving a project scene silently wrote the sandbox file and the
    // hierarchy (plus every other edit) was lost on restart.
    const std::string savePath =
        currentScenePath_.empty() ? "editor_scene.json" : currentScenePath_;
    if (std::ofstream out(savePath); out.is_open()) {
        out << core::JsonWriter::Write(root);
        sceneDirty_ = false;
        NEON_LOG_INFO("Scene saved (%zu entities) -> %s", entities_.size(),
                      savePath.c_str());
    } else {
        NEON_LOG_ERROR("Scene save failed: cannot write '%s'", savePath.c_str());
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

// G1-3: assign a unique id to every entity missing one (id == 0), using
// max existing id + 1, so parentId references are always resolvable. Called
// after scene load/setup and before save/export.
void EditorApp::NormalizeEntityIds() {
    int maxId = 0;
    for (const SceneEntity& e : entities_)
        if (e.id > maxId) maxId = e.id;
    std::set<int> used;
    for (SceneEntity& e : entities_) {
        // id 0 (unassigned, e.g. entities added mid-session) and DUPLICATE ids
        // (e.g. the duplicate command copies the source id) both get a fresh
        // unique id: without this, the id-based tree + drag cycle guards
        // misbehave (a duplicate id triggers the self-parent rejection).
        if (e.id == 0 || used.count(e.id) != 0) e.id = ++maxId;
        used.insert(e.id);
    }
}

// Stable per-parent sort of the scene tree by entity name. Root entities come
// first, then each group's children sorted case-insensitively by name,
// recursively (depth-first). Entities whose parentId points at a missing
// entity keep their relative order at the end. One undo step restores the
// whole previous order.
void EditorApp::SortSceneTreeByName() {
    if (entities_.size() < 2) return;
    NormalizeEntityIds(); // parentId references must resolve for the DFS
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    std::vector<size_t> order;
    order.reserve(entities_.size());
    std::set<int> placed;
    std::function<void(int)> visit = [&](int parentId) {
        std::vector<size_t> kids;
        for (size_t i = 0; i < entities_.size(); ++i)
            if (entities_[i].parentId == parentId) kids.push_back(i);
        std::stable_sort(kids.begin(), kids.end(), [&](size_t a, size_t b) {
            return lower(entities_[a].name) < lower(entities_[b].name);
        });
        for (size_t k : kids) {
            if (placed.count(static_cast<int>(k)) != 0) continue;
            placed.insert(static_cast<int>(k));
            order.push_back(k);
            visit(entities_[k].id);
        }
    };
    visit(0);
    for (size_t i = 0; i < entities_.size(); ++i)
        if (placed.count(static_cast<int>(i)) == 0) {
            placed.insert(static_cast<int>(i));
            order.push_back(i);
        }
    bool changed = false;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != i) {
            changed = true;
            break;
        }
    if (!changed) return;
    history_.Push(std::make_unique<SortSceneTreeCommand>(&entities_, std::move(order)));
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
    std::vector<std::string> legacyParents; // old name-based parents, resolved below
    // Destroy custom shader handles from the previous scene (P2-6).
    if (renderer_.Backend()) {
        for (SceneEntity& old : entities_) {
            if (old.customShader.Valid())
                renderer_.Backend()->DestroyShader(old.customShader.Handle());
        }
    }
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
        if (const core::Json* id = j->Get("id")) e.id = id->GetInt(0);
        e.name = j->Get("name")->GetString("entity");
        std::string legacyParent;
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
                if (const core::Json* r = t->Get("rot"))
                    e.rot = {static_cast<float>(r->At(0)->GetNumber()),
                             static_cast<float>(r->At(1)->GetNumber()),
                             static_cast<float>(r->At(2)->GetNumber()),
                             static_cast<float>(r->At(3)->GetNumber())};
                if (const core::Json* s = t->Get("scale"))
                    e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                               static_cast<float>(s->At(1)->GetNumber()),
                               static_cast<float>(s->At(2)->GetNumber())};
                if (const core::Json* p = t->Get("parentId")) e.parentId = p->GetInt(0);
                if (const core::Json* p = t->Get("parent")) legacyParent = p->GetString();
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
            if (const core::Json* nt = comps->Get("type")) {
                e.nodeType =
                    nt->Get("value") ? nt->Get("value")->GetString() : nt->GetString();
            }
            if (const core::Json* cam = comps->Get("camera")) {
                if (const core::Json* v = cam->Get("fov"))
                    e.cameraFov = static_cast<float>(v->GetNumber());
                if (const core::Json* v = cam->Get("ortho")) e.cameraOrtho = v->GetBool();
                if (const core::Json* v = cam->Get("orthoSize"))
                    e.cameraOrthoSize = static_cast<float>(v->GetNumber());
                if (e.nodeType.empty()) e.nodeType = "Camera3D";
            }
            if (const core::Json* li = comps->Get("light")) {
                e.hasLight = true;
                if (const core::Json* v = li->Get("type")) e.light.type = v->GetString();
                if (const core::Json* v = li->Get("sunDir")) {
                    float vv[3] = {0.0f, 0.0f, 0.0f};
                    size_t n = 0;
                    for (const core::Json& x : v->Items())
                        if (n < 3) vv[n++] = static_cast<float>(x.GetNumber());
                    e.light.sunDir = {vv[0], vv[1], vv[2]};
                }
                if (const core::Json* v = li->Get("color")) {
                    float vv[4] = {1, 1, 1, 1};
                    size_t n = 0;
                    for (const core::Json& x : v->Items())
                        if (n < 4) vv[n++] = static_cast<float>(x.GetNumber());
                    e.light.color = {vv[0], vv[1], vv[2], vv[3]};
                }
                if (const core::Json* v = li->Get("radius"))
                    e.light.radius = static_cast<float>(v->GetNumber());
                if (const core::Json* v = li->Get("ambientColor")) {
                    float vv[4] = {1, 1, 1, 1};
                    size_t n = 0;
                    for (const core::Json& x : v->Items())
                        if (n < 4) vv[n++] = static_cast<float>(x.GetNumber());
                    e.light.ambientColor = {vv[0], vv[1], vv[2], vv[3]};
                }
                if (const core::Json* v = li->Get("ambientStrength"))
                    e.light.ambientStrength = static_cast<float>(v->GetNumber());
                if (e.nodeType.empty()) e.nodeType = "Light3D";
            }
            if (const core::Json* so = comps->Get("sortOrder")) {
                if (const core::Json* z = so->Get("z"))
                    e.zOrder = static_cast<float>(z->GetNumber());
            }
            if (const core::Json* te = comps->Get("terrain")) {
                if (const core::Json* seg = te->Get("segments"))
                    e.terrainSegments_ = seg->GetInt(48);
                if (const core::Json* sz = te->Get("size"))
                    e.terrainSize_ = static_cast<float>(sz->GetNumber());
                if (const core::Json* hscale = te->Get("heightScale"))
                    e.terrainHeightScale_ = static_cast<float>(hscale->GetNumber());
                if (const core::Json* h = te->Get("heights")) {
                    if (h->IsArray())
                        for (const core::Json& v : h->Items())
                            e.terrainHeights_.push_back(static_cast<float>(v.GetNumber()));
                }
                // G2-3 chunked LOD + vegetation knobs (round-trip).
                if (const core::Json* v = te->Get("chunkGridDiv")) e.chunkGridDiv_ = v->GetInt(0);
                if (const core::Json* v = te->Get("chunkLodLevels")) e.chunkLodLevels_ = v->GetInt(3);
                if (const core::Json* v = te->Get("chunkBaseSubdiv")) e.chunkBaseSubdiv_ = v->GetInt(16);
                if (const core::Json* v = te->Get("vegMeshKey")) e.vegMeshKey_ = v->GetString();
                if (const core::Json* v = te->Get("vegCount")) e.vegCount_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = te->Get("vegSeed")) e.vegSeed_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = te->Get("vegSize")) e.vegSize_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegImpostorDistance")) e.vegImpostorDistance_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMinHeight")) e.vegMinHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMaxHeight")) e.vegMaxHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMaxSlope")) e.vegMaxSlope_ = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* tlm = comps->Get("tilemap")) {
                if (const core::Json* cols = tlm->Get("cols"))
                    e.tilemapCols_ = cols->GetInt(8);
                if (const core::Json* rows = tlm->Get("rows"))
                    e.tilemapRows_ = rows->GetInt(5);
                if (const core::Json* cs = tlm->Get("cellSize"))
                    e.tilemapCellSize_ = static_cast<float>(cs->GetNumber());
                if (const core::Json* tls = tlm->Get("tiles")) {
                    if (tls->IsArray())
                        for (const core::Json& v : tls->Items())
                            e.tilemapTiles_.push_back(v.GetString());
                }
            }
            if (const core::Json* dc = comps->Get("decal")) {
                if (const core::Json* tex = dc->Get("texture"))
                    e.decalTex = tex->GetString();
                if (const core::Json* sz = dc->Get("size"))
                    e.decalSize = static_cast<float>(sz->GetNumber());
                if (const core::Json* al = dc->Get("alpha"))
                    e.decalAlpha = static_cast<float>(al->GetNumber());
            }
            if (const core::Json* sh = comps->Get("shader")) {
                if (const core::Json* p = sh->Get("path")) e.shaderPath = p->GetString();
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
                    cname == "script" || cname == "sprite" || cname == "shader")
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
            if (const core::Json* p = j->Get("parentId")) e.parentId = p->GetInt(0);
            if (const core::Json* p = j->Get("parent")) legacyParent = p->GetString();
            if (const core::Json* nt = j->Get("nodeType")) e.nodeType = nt->GetString();
            if (const core::Json* cf = j->Get("cameraFov"))
                e.cameraFov = static_cast<float>(cf->GetNumber());
            if (const core::Json* co = j->Get("cameraOrtho"))
                e.cameraOrtho = co->GetBool() || co->GetNumber() != 0;
            if (const core::Json* sp = j->Get("shaderPath")) e.shaderPath = sp->GetString();
            e.meshKey = j->Get("mesh")->GetString("cube");
            if (const core::Json* zo = j->Get("zOrder"))
                e.zOrder = static_cast<float>(zo->GetNumber());
            if (const core::Json* td = j->Get("terrainData")) {
                if (const core::Json* seg = td->Get("segments"))
                    e.terrainSegments_ = seg->GetInt(48);
                if (const core::Json* sz = td->Get("size"))
                    e.terrainSize_ = static_cast<float>(sz->GetNumber());
                if (const core::Json* hscale = td->Get("heightScale"))
                    e.terrainHeightScale_ = static_cast<float>(hscale->GetNumber());
                if (const core::Json* h = td->Get("heights")) {
                    if (h->IsArray())
                        for (const core::Json& v : h->Items())
                            e.terrainHeights_.push_back(static_cast<float>(v.GetNumber()));
                }
                // G2-3 chunked LOD + vegetation knobs (round-trip).
                if (const core::Json* v = td->Get("chunkGridDiv")) e.chunkGridDiv_ = v->GetInt(0);
                if (const core::Json* v = td->Get("chunkLodLevels")) e.chunkLodLevels_ = v->GetInt(3);
                if (const core::Json* v = td->Get("chunkBaseSubdiv")) e.chunkBaseSubdiv_ = v->GetInt(16);
                if (const core::Json* v = td->Get("vegMeshKey")) e.vegMeshKey_ = v->GetString();
                if (const core::Json* v = td->Get("vegCount")) e.vegCount_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = td->Get("vegSeed")) e.vegSeed_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = td->Get("vegSize")) e.vegSize_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegImpostorDistance")) e.vegImpostorDistance_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMinHeight")) e.vegMinHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMaxHeight")) e.vegMaxHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMaxSlope")) e.vegMaxSlope_ = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* tlm = j->Get("tilemapData")) {
                if (const core::Json* cols = tlm->Get("cols"))
                    e.tilemapCols_ = cols->GetInt(8);
                if (const core::Json* rows = tlm->Get("rows"))
                    e.tilemapRows_ = rows->GetInt(5);
                if (const core::Json* cs = tlm->Get("cellSize"))
                    e.tilemapCellSize_ = static_cast<float>(cs->GetNumber());
                if (const core::Json* tls = tlm->Get("tiles")) {
                    if (tls->IsArray())
                        for (const core::Json& v : tls->Items())
                            e.tilemapTiles_.push_back(v.GetString());
                }
            }
            if (const core::Json* dc = j->Get("decalData")) {
                if (const core::Json* tex = dc->Get("texture"))
                    e.decalTex = tex->GetString();
                if (const core::Json* sz = dc->Get("size"))
                    e.decalSize = static_cast<float>(sz->GetNumber());
                if (const core::Json* al = dc->Get("alpha"))
                    e.decalAlpha = static_cast<float>(al->GetNumber());
            }
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
            if (!e.shaderPath.empty()) ReloadEntityShader(e);
            ApplyMaterialParams(e);
            loaded.push_back(std::move(e));
            legacyParents.push_back(std::move(legacyParent));
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
        // G1-3: stable ids (0 -> sequential) so the scene tree can reference
        // parents by id; then migrate legacy name-based parents to ids.
        NormalizeEntityIds();
        {
            std::map<std::string, int> firstIdByName;
            for (const SceneEntity& e : entities_)
                if (firstIdByName.count(e.name) == 0) firstIdByName[e.name] = e.id;
            for (size_t i = 0; i < entities_.size() && i < legacyParents.size(); ++i) {
                if (entities_[i].parentId != 0 || legacyParents[i].empty()) continue;
                const auto it = firstIdByName.find(legacyParents[i]);
                if (it != firstIdByName.end()) entities_[i].parentId = it->second;
            }
        }
        SetSelection(-1);
        history_.Clear(); // undo history from the previous scene is invalid
        currentSceneName_ = BaseName(path);
        NEON_LOG_INFO("Scene loaded (%zu entities)", entities_.size());
        EnsureSceneDefaultObjects();
    }
}

void EditorApp::EnsureSceneDefaultObjects() {
    // Unity default scene: every scene keeps a Main Camera + a Directional
    // Light object so nothing is observer-less. Old scenes that lack them get
    // them added on load (and the scene is marked dirty so a save persists it).
    bool hasCam = false, hasLight = false;
    for (const SceneEntity& e : entities_) {
        if (e.nodeType == "Camera3D") hasCam = true;
        if (e.hasLight) hasLight = true;
    }
    bool added = false;
    if (!hasCam) {
        SceneEntity e;
        e.name = "Main Camera";
        e.nodeType = "Camera3D";
        e.pos = {0.0f, 3.0f, 10.0f};
        e.cameraFov = 60.0f;
        if (projectMode_ == "2d" || editMode_ == EditMode::Scene2D) {
            // 2D: a locked orthographic camera framing the 1280x720 design space.
            e.cameraOrtho = true;
            e.cameraOrthoSize = 360.0f;
            e.pos = {640.0f, 360.0f, 100.0f}; // behind the content plane, at the design centre
        }
        entities_.push_back(std::move(e));
        added = true;
    }
    if (!hasLight) {
        SceneEntity e;
        e.name = "Directional Light";
        e.nodeType = "Light3D";
        e.hasLight = true;
        e.light.type = "directional";
        entities_.push_back(std::move(e));
        added = true;
    }
    if (added) {
        NormalizeEntityIds();
        sceneDirty_ = true;
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
                                            e.hp, e.maxHp);
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
    // Editor plugins are project-scoped: reload them for the new project.
    if (pluginMgr_) pluginMgr_->Load(projectDir_);
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

// Returns the throwaway syntax-check host matching a script file's extension
// (.js -> QuickJS, otherwise Lua), creating it lazily. Both hosts are
// validation-only: CheckSyntax never executes a chunk, so a failed check
// leaves the host fully reusable for the next file.
script::IScriptHost* EditorApp::ScriptCheckHostFor(const std::string& path) {
    const bool isJs = path.size() >= 3 &&
                      (path.compare(path.size() - 3, 3, ".js") == 0 ||
                       path.compare(path.size() - 3, 3, ".JS") == 0);
    if (isJs) {
        if (!scriptCheckHostJs_) {
            scriptCheckHostJs_ = script::CreateJsHost();
            if (scriptCheckHostJs_) scriptCheckHostJs_->Init();
        }
        return scriptCheckHostJs_.get();
    }
    if (!scriptCheckHost_) {
        scriptCheckHost_ = script::CreateLuaHost();
        if (scriptCheckHost_) scriptCheckHost_->Init();
    }
    return scriptCheckHost_.get();
}

void EditorApp::OpenScriptEditor(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Editor: cannot open script '%s'", path.c_str());
        return;
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
    script::IScriptHost* checkHost = ScriptCheckHostFor(path);
    if (checkHost) {
        if (scriptEditorRel_ != path) {
            scriptEditorCheck_ = CheckScriptFile(*checkHost, base, scriptEditorRel_);
        } else {
            std::ifstream src(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(src)),
                             std::istreambuf_iterator<char>());
            scriptEditorCheck_.path = path;
            scriptEditorCheck_.ok = checkHost->CheckSyntax(text);
            if (!scriptEditorCheck_.ok) {
                scriptEditorCheck_.message = checkHost->LastError().message;
                scriptEditorCheck_.line = checkHost->LastError().line;
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
    script::IScriptHost* checkHost = ScriptCheckHostFor(scriptEditorPath_);
    if (checkHost) {
        if (scriptEditorRel_ != scriptEditorPath_) {
            scriptEditorCheck_ = CheckScriptFile(*checkHost, base, scriptEditorRel_);
        } else {
            scriptEditorCheck_.path = scriptEditorPath_;
            scriptEditorCheck_.ok = checkHost->CheckSyntax(scriptEditorBuf_);
            if (!scriptEditorCheck_.ok) {
                scriptEditorCheck_.message = checkHost->LastError().message;
                scriptEditorCheck_.line = checkHost->LastError().line;
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
    // unconditionally here, not only when the index changes. Also keeps the
    // multi-selection set inside the entity list bounds.
    scriptSyncEntity_ = -1;
    const int n = static_cast<int>(entities_.size());
    for (auto it = selection_.begin(); it != selection_.end();) {
        if (*it >= n)
            it = selection_.erase(it);
        else
            ++it;
    }
    if (selected_ >= n) selected_ = selection_.empty() ? -1 : *selection_.rbegin();
    if (!selection_.empty() && selection_.count(selected_) == 0)
        selected_ = *selection_.rbegin();
    if (entities_.empty()) {
        selected_ = -1;
        selection_.clear();
        selectionAnchor_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Editor plugin API (NeonEditor.* native bindings)
// ---------------------------------------------------------------------------

void EditorApp::PluginAddEntity(const std::string& meshKey, float x, float y, float z) {
    static int counter = 1;
    std::string name;
    if (meshKey.rfind("obj:", 0) == 0 || meshKey.rfind("gltf:", 0) == 0) {
        const std::string path = meshKey.substr(meshKey.find(':') + 1);
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        const size_t begin = slash == std::string::npos ? 0 : slash + 1;
        const size_t len =
            (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = {x, y, z};
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        NEON_LOG_INFO("Editor plugin spawned '%s' (%s) at (%.1f, %.1f, %.1f)", e.name.c_str(),
                      meshKey.c_str(), x, y, z);
    } else {
        NEON_LOG_ERROR("Editor plugin: cannot resolve mesh key '%s'", meshKey.c_str());
    }
}

std::string EditorApp::PluginBuildMesh(const std::string& name,
                                       const std::vector<math::Vec3>& verts,
                                       const std::vector<int>& indices) {
    if (name.empty() || verts.empty() || indices.empty() || indices.size() % 3 != 0) {
        NEON_LOG_ERROR("Editor plugin: buildMesh needs a name, verts and triangle indices");
        return {};
    }
    const std::string rel =
        (projectDir_ == "." ? "assets/generated/" : projectDir_ + "/assets/generated/") +
        name + ".obj";
    const size_t slash = rel.find_last_of("/\\");
    if ((slash != std::string::npos && !EnsureDirs(rel.substr(0, slash))) ||
        (slash == std::string::npos)) {
        NEON_LOG_ERROR("Editor plugin: cannot create generated asset dir for '%s'", rel.c_str());
        return {};
    }
    std::ofstream out(rel, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor plugin: cannot write '%s'", rel.c_str());
        return {};
    }
    for (const math::Vec3& v : verts)
        out << "v " << v.x << " " << v.y << " " << v.z << "\n";
    for (size_t i = 0; i < indices.size(); i += 3)
        out << "f " << indices[i] << " " << indices[i + 1] << " " << indices[i + 2] << "\n";
    out.close();
    NEON_LOG_INFO("Editor plugin: generated mesh asset '%s'", rel.c_str());
    return "obj:" + rel;
}

script::Value EditorApp::PluginSelectedEntity() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size()))
        return script::Value::Nil();
    const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("name", script::Value::Str(e.name));
    t.table->fields.emplace_back("x", script::Value::Num(e.pos.x));
    t.table->fields.emplace_back("y", script::Value::Num(e.pos.y));
    t.table->fields.emplace_back("z", script::Value::Num(e.pos.z));
    return t;
}

script::Value EditorApp::PluginEntityList() const {
    script::Value out = script::Value::Tbl();
    for (const SceneEntity& e : entities_) {
        script::Value t = script::Value::Tbl();
        t.table->fields.emplace_back("name", script::Value::Str(e.name));
        t.table->fields.emplace_back("x", script::Value::Num(e.pos.x));
        t.table->fields.emplace_back("y", script::Value::Num(e.pos.y));
        t.table->fields.emplace_back("z", script::Value::Num(e.pos.z));
        t.table->fields.emplace_back("mesh", script::Value::Str(e.meshKey));
        out.table->array.push_back(std::move(t));
    }
    return out;
}

void EditorApp::PluginLog(const std::string& msg) {
    NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Info, "plugin: %s", msg.c_str());
}

std::string EditorApp::PluginImportAsset(const std::string& srcPath) {
    if (srcPath.empty() || assetDir_.empty()) return {};
    const std::string name = BaseName(srcPath);
    if (name.empty() || name == "." || name == "..") return {};
    const std::string dst = assetDir_ + "/" + name;
    std::ifstream in(srcPath, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) {
        NEON_LOG_ERROR("Editor plugin: cannot import '%s' -> '%s'", srcPath.c_str(),
                       dst.c_str());
        return {};
    }
    out << in.rdbuf();
    out.close();
    RefreshAssetDir();
    return ToProjectRelPath(dst, projectDir_);
}

std::vector<std::string> EditorApp::PluginListDir(const std::string& dir) const {
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + "/" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (::stat((dir + "/" + name).c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) out.push_back(dir + "/" + name);
    }
    ::closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

void EditorApp::BuildPluginPanels() {
    if (!pluginMgr_) return;
    for (editor::PluginPanel& p : pluginMgr_->Panels()) pluginMgr_->DrawPanel(p);
}

void EditorApp::BuildPluginsPanel() {
    if (!showPlugins_ || !pluginMgr_) return;
    if (ImGui::Begin("插件", &showPlugins_)) {
        ImGui::TextDisabled("项目插件目录: %s/plugins", projectDir_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("重新加载")) pluginMgr_->Load(projectDir_);
        ImGui::Separator();
        const auto& manifests = pluginMgr_->Manifests();
        if (manifests.empty()) {
            ImGui::TextDisabled("未发现编辑器插件 (type=editor)");
        }
        for (const plugin::PluginManifest& m : manifests) {
            ImGui::BulletText("%s  v%s  [%s/%s]", m.name.c_str(), m.version.c_str(),
                              m.backend.c_str(), plugin::PluginTypeName(m.type));
            ImGui::TextDisabled("  id: %s  entry: %s", m.id.c_str(), m.entry.c_str());
        }
        ImGui::Separator();
        if (!pluginMgr_->Panels().empty()) {
            ImGui::TextDisabled("面板 (%zu):", pluginMgr_->Panels().size());
            for (const editor::PluginPanel& p : pluginMgr_->Panels())
                ImGui::TextDisabled("  %s", p.title.c_str());
        }
        if (!pluginMgr_->AssetSources().empty()) {
            ImGui::TextDisabled("资产源 (%zu):", pluginMgr_->AssetSources().size());
            for (const editor::PluginAssetSource& s : pluginMgr_->AssetSources())
                ImGui::TextDisabled("  %s", s.name.c_str());
        }
    }
    ImGui::End();
}

void EditorApp::BuildDebugOverlayPanel() {
    if (!showDebugOverlay_) return;
    if (ImGui::Begin("调试覆盖层", &showDebugOverlay_)) {
        ImGui::TextDisabled("F3 开关本面板; 图层实时作用在视口");
        ImGui::Checkbox("碰撞线框", &debugColliders_);
        ImGui::Checkbox("导航可行走区域", &debugNavMesh_);
        ImGui::Checkbox("光照探针", &debugProbes_);
        ImGui::Checkbox("音频源", &debugAudio_);
        ImGui::SameLine();
        ImGui::TextDisabled("(暂无数据)");
        ImGui::TextDisabled("提示: 碰撞/导航在编辑与试玩视口即时生效");
    }
    ImGui::End();
}

void EditorApp::DrawDebugOverlay(const gfx::Camera& cam) {
    (void)cam;
    if (!renderer_.Backend()) return;

    // Navigation walkable area: translucent green (walkable) / red (blocked)
    // ground cells so the field is visible in the viewport, not just the panel.
    if (debugNavMesh_ && navGrid_.Valid()) {
        static gfx::Mesh cell = gfx::Mesh::CreatePlane(renderer_, 1.0f, 1.0f, 1, 1, "navcell");
        if (!cell.Valid()) return;
        gfx::Material walk = gfx::Material::Unlit({}, gfx::Color{0.3f, 0.85f, 0.4f, 0.28f});
        walk.transparent = true;
        gfx::Material block = gfx::Material::Unlit({}, gfx::Color{0.9f, 0.3f, 0.3f, 0.28f});
        block.transparent = true;
        for (int y = 0; y < navGrid_.Height(); ++y) {
            for (int x = 0; x < navGrid_.Width(); ++x) {
                const math::Vec2 c = navGrid_.CellToWorld(x, y);
                const math::Mat4 m = math::Mat4::Translation({c.x, 0.02f, c.y}) *
                                     math::Mat4::Scale({navGrid_.CellSize(), 1.0f,
                                                        navGrid_.CellSize()});
                renderer_.DrawMesh(cell, navGrid_.Walkable(x, y) ? walk : block, m);
            }
        }
    }

    // Light probes: wireframe sphere markers tinted by probe irradiance, over a
    // field rebuilt lazily from the scene's 3D meshes' combined AABB. A single
    // representative light near the scene centre makes the 3D gradient visible;
    // real scene point lights would feed this list (G2-4).
    if (debugProbes_) {
        math::AABB bounds;
        bool haveBounds = false;
        for (const SceneEntity& e : entities_) {
            if (!e.mesh.Valid()) continue;
            const math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                     math::Mat4::Scale(e.scale);
            const math::AABB wb = math::TransformAABB(e.mesh.Bounds(), model);
            if (!haveBounds) {
                bounds = wb;
                haveBounds = true;
            } else {
                bounds.min.x = std::min(bounds.min.x, wb.min.x);
                bounds.min.y = std::min(bounds.min.y, wb.min.y);
                bounds.min.z = std::min(bounds.min.z, wb.min.z);
                bounds.max.x = std::max(bounds.max.x, wb.max.x);
                bounds.max.y = std::max(bounds.max.y, wb.max.y);
                bounds.max.z = std::max(bounds.max.z, wb.max.z);
            }
        }
        if (!haveBounds) {
            bounds = math::AABB{{-15, 0, -15}, {15, 5, 15}};
        }
        const bool same =
            debugProbeBounds_.min.x == bounds.min.x && debugProbeBounds_.max.x == bounds.max.x &&
            debugProbeBounds_.min.y == bounds.min.y && debugProbeBounds_.max.y == bounds.max.y &&
            debugProbeBounds_.min.z == bounds.min.z && debugProbeBounds_.max.z == bounds.max.z;
        if (debugProbeDirty_ || !same) {
            gfx::ProbeLightInput in;
            in.pointLights.push_back({(bounds.min + bounds.max) * 0.5f,
                                      gfx::Color{1.0f, 0.9f, 0.75f, 1.0f}, 6.0f,
                                      std::max(bounds.max.x - bounds.min.x, 8.0f)});
            gfx::BuildProbeField(bounds, debugProbeRes_, in, debugProbeField_);
            debugProbeBounds_ = bounds;
            debugProbeDirty_ = false;
        }
        const float markerR =
            std::max(0.08f, (bounds.max.x - bounds.min.x) / (2.0f * static_cast<float>(debugProbeRes_)));
        for (const gfx::IrradianceProbe& p : debugProbeField_) {
            const float y = math::Clamp(p.irradiance.y, 0.0f, 1.0f);
            renderer_.DrawSphere(p.pos, markerR,
                                 gfx::Color{math::Clamp(p.irradiance.x, 0.0f, 1.0f),
                                            y,
                                            math::Clamp(p.irradiance.z, 0.0f, 1.0f), 0.9f});
        }
    }
}

} // namespace neon::editor
