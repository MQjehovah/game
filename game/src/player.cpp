#include "player.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "font_data.hpp"
#include "neon/core/pack.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neon::player {
namespace {

// ---------------------------------------------------------------------------
// Filesystem helpers (temp dir + recursive cleanup). The unpacked pack lives
// in a fresh OS temp dir for the session and is removed on exit unless --keep.
// ---------------------------------------------------------------------------

bool ReadFileAll(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// Strips a leading UTF-8 BOM (EF BB BF), which Windows editors and PowerShell
// often prepend; JSON/Lua/bt parsers below expect plain UTF-8.
void StripBom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

std::string MakeTempDir() {
#if defined(_WIN32)
    char base[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, base);
    if (n == 0 || n >= MAX_PATH) return {};
    for (int i = 0; i < 128; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "neon_game_%lu_%lu",
                      static_cast<unsigned long>(GetCurrentProcessId()),
                      static_cast<unsigned long>(GetTickCount() + i));
        std::string dir = std::string(base) + name;
        if (CreateDirectoryA(dir.c_str(), nullptr)) return dir;
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    return {};
#else
    const char* base = std::getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    for (int i = 0; i < 128; ++i) {
        std::string dir = std::string(base) + "/neon_game_" +
                          std::to_string(static_cast<long long>(::getpid())) + "_" +
                          std::to_string(i);
        if (::mkdir(dir.c_str(), 0700) == 0) return dir;
        if (errno != EEXIST) break;
    }
    return {};
#endif
}

// Best-effort recursive delete (mirrors the test harness TempDir cleanup).
void RemoveTree(const std::string& dir) {
#if defined(_WIN32)
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                continue;
            std::string full = dir + "\\" + fd.cFileName;
            DWORD attrs = fd.dwFileAttributes;
            if (attrs & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesA(full.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            if (attrs & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(full.c_str());
            else
                DeleteFileA(full.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
#else
    DIR* d = ::opendir(dir.c_str());
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            std::string full = dir + "/" + e->d_name;
            struct stat st;
            if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                RemoveTree(full.c_str());
            else
                ::unlink(full.c_str());
        }
        ::closedir(d);
    }
    ::rmdir(dir.c_str());
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Boot: unpack the pack + load the manifest (run before the window exists so
// the manifest can drive window size/title).
// ---------------------------------------------------------------------------

core::Result<PackBoot> BootPack(const std::string& packPath) {
    if (packPath.empty()) return core::Result<PackBoot>::Err("player: no --pack file given");

    std::string bytes;
    if (!ReadFileAll(packPath, bytes)) {
        return core::Result<PackBoot>::Err("player: cannot read pack '" + packPath + "'");
    }
    std::vector<uint8_t> buf(bytes.begin(), bytes.end());
    core::PackReader reader(buf);
    if (!reader.Valid()) {
        return core::Result<PackBoot>::Err("player: pack '" + packPath + "' is invalid: " +
                                           reader.Error());
    }
    std::string unpackedDir = MakeTempDir();
    if (unpackedDir.empty()) {
        return core::Result<PackBoot>::Err("player: cannot create temp unpack directory");
    }
    core::Status st = core::Unpack(reader, unpackedDir);
    if (!st.Ok()) {
        RemoveTree(unpackedDir);
        return core::Result<PackBoot>::Err("player: unpack failed: " + st.Error());
    }

    std::string manifestText;
    if (!ReadFileAll(unpackedDir + "/game.json", manifestText)) {
        RemoveTree(unpackedDir);
        return core::Result<PackBoot>::Err("player: game.json missing in pack '" + packPath + "'");
    }
    StripBom(manifestText);
    auto res = scene::GameManifest::Load(manifestText);
    if (!res.Ok()) {
        RemoveTree(unpackedDir);
        return core::Result<PackBoot>::Err("player: bad manifest: " + res.Error());
    }

    NEON_LOG_INFO("player: unpacked %zu files to '%s'", reader.FileCount(),
                  unpackedDir.c_str());
    PackBoot boot;
    boot.unpackedDir = std::move(unpackedDir);
    boot.manifest = res.Value();
    return core::Result<PackBoot>::Ok(std::move(boot));
}

bool PlayerApp::OnCreate() {
    title_ = cfg_.manifest.title.empty() ? "Neon Game" : cfg_.manifest.title;
    if (!LoadSceneJson()) {
        CleanupUnpackedDir();
        return false;
    }

    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Player: renderer init failed");
        CleanupUnpackedDir();
        return false;
    }
    assetMgr_.Init(&renderer_);

    // Overlay font: embedded ASCII pixel font, upgraded with the system CJK
    // font when available so "Esc 退出" renders (fallback keeps ASCII labels).
    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
    const std::vector<std::string> cjkSamples = {"退出鼠标拖动旋转视角帧率游戏"};
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24, cjkSamples);
    theme_.font = cjkFont_.Valid() ? cjkFont_ : pixelFont_;

    scene::GameRuntimeConfig rcfg;
    rcfg.assets = &assetMgr_;
    rcfg.scriptBaseDir = cfg_.unpackedDir; // scripts/ + behaviors/ resolve here
    rcfg.assetBaseDir = cfg_.unpackedDir;  // obj:/gltf:/texture paths resolve here
    rcfg.input = Input();                  // data-driven scripts read live input
    core::Status st = runtime_.Start(sceneJson_, rcfg);
    if (!st.Ok()) {
        NEON_LOG_ERROR("Player: runtime start failed: %s", st.Error().c_str());
        CleanupUnpackedDir();
        return false;
    }
    started_ = true;
    NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                 "player: '%s' started (%zu entities, %zu scripts, %zu trees, %zu draws)",
                 title_.c_str(), runtime_.EntityCount(), runtime_.ScriptCount(),
                 runtime_.BehaviorTreeCount(), runtime_.DrawCount());
    return true;
}

void PlayerApp::CleanupUnpackedDir() {
    if (!cfg_.keep && !cfg_.unpackedDir.empty()) {
        RemoveTree(cfg_.unpackedDir);
        cfg_.unpackedDir.clear();
    }
}

bool PlayerApp::LoadSceneJson() {
    std::string scenePath = cfg_.manifest.startScene;
    if (!cfg_.sceneOverride.empty()) {
        scenePath = cfg_.sceneOverride;
        // A bare name (no slash, no extension) maps onto scenes/<name>.json.
        if (scenePath.find('/') == std::string::npos &&
            scenePath.find('\\') == std::string::npos &&
            scenePath.find('.') == std::string::npos) {
            scenePath = "scenes/" + scenePath + ".json";
        }
    }
    // Defense-in-depth: reject a startScene/--scene that could escape the
    // unpacked dir (a hostile pack's manifest or a hand-typed override).
    if (core::IsUnsafeRelPath(scenePath)) {
        NEON_LOG_ERROR("Player: unsafe scene path '%s'", scenePath.c_str());
        return false;
    }
    if (!ReadFileAll(cfg_.unpackedDir + "/" + scenePath, sceneJson_)) {
        NEON_LOG_ERROR("Player: cannot read scene '%s'", scenePath.c_str());
        return false;
    }
    StripBom(sceneJson_);
    NEON_LOG_INFO("Player: loading scene '%s'", scenePath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void PlayerApp::OnUpdate(float dt) {
    // Drain completed async texture decodes (GPU uploads + callbacks happen
    // here on the main thread). No-op when no async loads are in flight.
    assetMgr_.PumpAsync();
    if (!started_) return;
    UpdateCamera(dt);
    runtime_.Tick(dt);
    if (Input()->Pressed(platform::Key::Escape)) Window()->RequestClose();
}

// Orbit camera around a focus point. Scripts control the focus through the
// GameVar "cameraFocus" = {x,y,z} (data-driven framing); the default is the
// world origin. Mouse drag orbits, wheel zooms.
//
// NOTE: this reads MouseDelta/WheelDelta every frame and scripts see the SAME
// accumulated values via InputMouseX/Y (the input state only clears at
// EndFrame), so the orbit camera and data-driven scripts double-consume mouse
// input. See the PlayerApp class comment for the FPS-camera caveat.
void PlayerApp::UpdateCamera(float dt) {
    math::Vec3 focus = focus_;
    script::Value f = runtime_.GameVars().Get("cameraFocus");
    if (f.type == script::Value::Type::Table && f.table) {
        for (const auto& kv : f.table->fields) {
            if (kv.second.type != script::Value::Type::Number) continue;
            if (kv.first == "x") focus.x = static_cast<float>(kv.second.number);
            else if (kv.first == "y") focus.y = static_cast<float>(kv.second.number);
            else if (kv.first == "z") focus.z = static_cast<float>(kv.second.number);
        }
        focus_ = focus;
    }

    platform::IInput* input = Input();
    yaw_ += -input->MouseDelta().x * 0.004f;
    pitch_ += -input->MouseDelta().y * 0.004f;
    pitch_ = math::Clamp(pitch_, -1.3f, 1.3f);
    float wheel = input->WheelDelta();
    if (std::fabs(wheel) > 0.01f) camDist_ = math::Clamp(camDist_ - wheel * 1.5f, 2.0f, 80.0f);

    math::Vec3 offset{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
                      std::cos(yaw_) * std::cos(pitch_)};
    camera_.position = focus + offset * camDist_;
    camera_.target = focus;
    camera_.up = {0, 1, 0};
    (void)dt;
}

void PlayerApp::OnRender() {
    renderer_.BeginFrame({0.02f, 0.04f, 0.09f, 1.0f});
    renderer_.SetSky({0.10f, 0.18f, 0.36f, 1.0f}, {0.45f, 0.60f, 0.78f, 1.0f});
    renderer_.SetFog({0.45f, 0.60f, 0.78f, 1.0f}, 60.0f, 220.0f);
    renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.3f);
    renderer_.DrawSky();

    if (started_) {
        float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
        renderer_.SetCamera(camera_, aspect);
        runtime_.Draw(renderer_, camera_);
    }
    renderer_.EndScene();

    DrawOverlay();
    CaptureScreenshotIfDue();
    renderer_.EndFrame();
}

void PlayerApp::DrawOverlay() {
    const int w = renderer_.ScreenWidth();
    const int h = renderer_.ScreenHeight();
    ui::DrawLabel(renderer_, theme_, title_, {static_cast<float>(w) * 0.5f, 22}, 20,
                  theme_.accent, true, false);

    ui::DrawLabel(renderer_, theme_, "Esc 退出", {18, static_cast<float>(h) - 28}, 13,
                  theme_.text.WithAlpha(0.8f), false, false);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.0f FPS | 鼠标拖动旋转 滚轮缩放", TimeRef().Fps());
    math::Vec2 size = ui::MeasureText(theme_.font, buf, 12);
    ui::DrawLabel(renderer_, theme_, buf,
                  {static_cast<float>(w) - size.x - 14, static_cast<float>(h) - 28}, 12,
                  theme_.text.WithAlpha(0.55f), false, false);
}

void PlayerApp::CaptureScreenshotIfDue() {
    if (cfg_.screenshotPath.empty() || TimeRef().frameIndex < cfg_.screenshotFrame) return;
    std::vector<uint8_t> pixels;
    if (renderer_.CaptureFrame(pixels)) {
        int ok = stbi_write_png(cfg_.screenshotPath.c_str(), renderer_.ScreenWidth(),
                                renderer_.ScreenHeight(), 4, pixels.data(),
                                renderer_.ScreenWidth() * 4);
        NEON_LOG_INFO("Player: screenshot '%s' (%s)", cfg_.screenshotPath.c_str(),
                      ok ? "ok" : "failed");
    } else {
        NEON_LOG_WARN("Player: screenshot capture failed");
    }
    cfg_.screenshotPath.clear();
}

void PlayerApp::OnEvent(const platform::InputEvent& event) {
    if (event.type == platform::InputEvent::Type::KeyDown &&
        event.key == platform::Key::Escape) {
        Window()->RequestClose();
    }
}

void PlayerApp::OnShutdown() {
    if (started_) {
        if (cfg_.dumpVars) DumpGameVars();
        runtime_.Stop();
    }
    renderer_.Shutdown();
    CleanupUnpackedDir();
}

// Logs every GameVar so the smoke-test verification can grep a concrete result
// (e.g. a script-incremented "ticks" value > 0).
void PlayerApp::DumpGameVars() {
    runtime_.GameVars().ForEach([](const std::string& key, const script::Value& v) {
        char line[512];
        switch (v.type) {
            case script::Value::Type::Number:
                std::snprintf(line, sizeof(line), "GAME VAR %s = %g", key.c_str(), v.number);
                break;
            case script::Value::Type::String:
                std::snprintf(line, sizeof(line), "GAME VAR %s = \"%s\"", key.c_str(),
                              v.str.c_str());
                break;
            case script::Value::Type::Bool:
                std::snprintf(line, sizeof(line), "GAME VAR %s = %s", key.c_str(),
                              v.boolean ? "true" : "false");
                break;
            case script::Value::Type::Table:
                std::snprintf(line, sizeof(line), "GAME VAR %s = <table %zu fields>", key.c_str(),
                              v.table ? v.table->fields.size() : 0u);
                break;
            default:
                std::snprintf(line, sizeof(line), "GAME VAR %s = nil", key.c_str());
                break;
        }
        NEON_LOG_INFO("%s", line);
    });
}

} // namespace neon::player
