#include "player.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "font_data.hpp"
#include "neon/assets/asset_variants.hpp"
#include "neon/core/pack.hpp"
#include "neon/io/vfs.hpp"

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

bool FileExists(const std::string& path) {
#if defined(_WIN32)
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
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

// Parent directory of a path ("a/b/c.json" -> "a/b", "scene.json" -> ".").
// Mirrors neon_server's DirName so a loose --scene resolves its scripts/ the
// same way the server does.
std::string DirName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

// Mirrors neon_server's ScriptBaseForLooseScene: walk up from the scene file
// to the nearest ancestor with game.json (the project root) so an
// assets/scenes/x.json scene resolves "assets/..." scripts from the project.
std::string ScriptBaseForLooseScene(const std::string& scenePath) {
    const std::string fallback = DirName(scenePath);
    std::string dir = fallback;
    for (int i = 0; i < 8; ++i) {
        if (std::ifstream(dir + "/game.json", std::ios::binary).is_open()) return dir;
        const size_t slash = dir.find_last_of("/\\");
        if (slash == std::string::npos || slash == 0) break;
        dir = dir.substr(0, slash);
    }
    return fallback;
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

// Recursive copy of `from` into `to` (directories merged, files overwritten;
// Mod overlay semantics: later copies win).
void CopyTree(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    CreateDirectoryA(to.c_str(), nullptr);
    const std::string pattern = from + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        const std::string src = from + "\\" + fd.cFileName;
        const std::string dst = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            CopyTree(src, dst);
        else
            CopyFileA(src.c_str(), dst.c_str(), FALSE); // overwrite: mod wins
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    ::mkdir(to.c_str(), 0777);
    DIR* d = ::opendir(from.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        const std::string src = from + "/" + e->d_name;
        const std::string dst = to + "/" + e->d_name;
        struct stat st;
        if (::lstat(src.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            CopyTree(src, dst);
        else {
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(dst, std::ios::binary);
            out << in.rdbuf();
        }
    }
    ::closedir(d);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Boot: unpack the pack + load the manifest (run before the window exists so
// the manifest can drive window size/title).
// ---------------------------------------------------------------------------

core::Result<PackBoot> BootPack(const std::string& packPath,
                                const std::vector<std::string>& modDirs) {
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

    // G7-1 剩余: read the pack straight through the VFS — game.json, scenes,
    // scripts, assets and Mods all resolve via the mount stack, so nothing is
    // unpacked to disk (boot is faster and leaves no temp dir).
    auto vfs = std::make_shared<neon::io::MountStack>();
    vfs->Mount(std::make_shared<neon::io::PackFileSystem>(std::move(buf)));
    for (const std::string& modDir : modDirs) {
        if (!modDir.empty() && FileExists(modDir)) {
            vfs->Mount(std::make_shared<neon::io::DiskFileSystem>(modDir));
            NEON_LOG_INFO("player: mounted mod '%s' over the base pack", modDir.c_str());
        } else {
            NEON_LOG_WARN("player: mod dir '%s' not found; skipped", modDir.c_str());
        }
    }

    const core::Result<std::vector<uint8_t>> manifestBytes = vfs->ReadFile("game.json");
    if (!manifestBytes.Ok()) {
        return core::Result<PackBoot>::Err("player: game.json missing in pack '" + packPath + "'");
    }
    std::string manifestText(manifestBytes.Value().begin(), manifestBytes.Value().end());
    StripBom(manifestText);
    auto res = scene::GameManifest::Load(manifestText);
    if (!res.Ok()) {
        return core::Result<PackBoot>::Err("player: bad manifest: " + res.Error());
    }

    NEON_LOG_INFO("player: pack '%s' served via VFS (%zu files, no unpack)",
                  packPath.c_str(), reader.FileCount());
    PackBoot boot;
    boot.unpackedDir.clear(); // everything is served through the VFS
    boot.manifest = res.Value();
    boot.vfs = std::move(vfs);
    return core::Result<PackBoot>::Ok(std::move(boot));
}

bool PlayerApp::OnCreate() {
    title_ = cfg_.manifest.title.empty() ? "Neon Game" : cfg_.manifest.title;
    renderer_.SetBackendName(cfg_.backend);
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
// G7-1: asset reads go through the pack + Mod mount stack when present.
if (cfg_.vfs) assetMgr_.SetFileSystem(cfg_.vfs.get());
// G5-4-3: prefer offline-baked BC1 textures (.neon/imported in the pack, or on
// disk for loose scenes) so uploads skip the runtime decode+compress.
assetMgr_.SetTextureBakeDir(".neon/imported");
    // Overlay font: embedded ASCII pixel font, upgraded with the system CJK
    // font when available so "Esc 退出" renders (fallback keeps ASCII labels).
    pixelFont_ = renderer_.CreateFontFromMemory(neon::embedded::kEmbeddedFontData,
                                                neon::embedded::kEmbeddedFontSize, 24);
    // System CJK font with DYNAMIC glyphs: any text (HUD, dialogue, scripts)
    // renders without maintaining a character list.
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24);
    theme_.font = cjkFont_.Valid() ? cjkFont_ : pixelFont_;

    // Microkernel (P-E): wire the replaceable physics + script services through
    // the Kernel. GameRuntime fetches them from the registry (non-owning); any
    // absent service falls back to the runtime's self-contained creation.
#ifdef NEON_ENABLE_JOLT
    kernel_.Add(std::make_unique<modules::PhysicsModule>(
        std::make_unique<physics::JoltWorld>()));  // packaged game = Jolt
#endif
    if (auto lua = script::CreateLuaHost())
        kernel_.Add(std::make_unique<modules::ScriptModule>(std::move(lua)));
    kernel_.Init();

    scene::GameRuntimeConfig rcfg;
    rcfg.assets = &assetMgr_;
    rcfg.services = &kernel_.Services();
    rcfg.scriptBaseDir = cfg_.unpackedDir; // assets/scripts + assets/behaviors resolve here
    rcfg.pluginBaseDir = cfg_.unpackedDir.empty() ? std::string(".") : cfg_.unpackedDir; // G5-1
    if (!cfg_.looseScenePath.empty()) rcfg.scriptBaseDir = cfg_.scriptsDir;
    // With a VFS installed (pack + Mods), asset paths stay RELATIVE so the
    // mount stack resolves them (pack-relative keys); without one they resolve
    // against the unpacked dir as before.
    rcfg.assetBaseDir = cfg_.vfs ? std::string() : cfg_.unpackedDir;
    // G7-1: script reads (assets/ scripts+behaviors+prefabs+locales, input.json) go
    // through the pack + Mod mount stack directly, overriding the unpacked-dir
    // copy — Mods replace base-pack scripts without editing the unpacked tree.
    rcfg.fileSystem = cfg_.vfs ? cfg_.vfs.get() : nullptr;
    // G6-1: platform/LOD asset variants from the project's variants.json.
    rcfg.variantTable = cfg_.variant.empty() ? nullptr : &variantTable_;
    if (!cfg_.variant.empty()) {
        std::string variantsText;
        if (cfg_.vfs) {
            const core::Result<std::vector<uint8_t>> b = cfg_.vfs->ReadFile("variants.json");
            if (b.Ok()) variantsText.assign(b.Value().begin(), b.Value().end());
        } else if (!cfg_.unpackedDir.empty()) {
            std::ifstream vIn(cfg_.unpackedDir + "/variants.json", std::ios::binary);
            if (vIn.is_open())
                variantsText.assign(std::istreambuf_iterator<char>(vIn),
                                    std::istreambuf_iterator<char>());
        }
        if (variantsText.empty()) {
            NEON_LOG_WARN("Player: --variant '%s' but no variants.json found; using base assets",
                          cfg_.variant.c_str());
            rcfg.variantTable = nullptr;
        } else {
            std::string varErr;
            if (!assets::AssetVariantTable::LoadVariant(variantsText, cfg_.variant, variantTable_,
                                                        &varErr)) {
                NEON_LOG_WARN("Player: --variant '%s' rejected: %s; using base assets",
                              cfg_.variant.c_str(), varErr.c_str());
                variantTable_ = assets::AssetVariantTable{};
                rcfg.variantTable = nullptr;
            } else {
                NEON_LOG_INFO("Player: asset variant '%s' active (%zu overrides)",
                              cfg_.variant.c_str(), variantTable_.Size());
            }
        }
    }
    // The runtime reads input through ClientInput (a transparent bridge to the
    // real input; SetForceMove synthesizes W for the --ticks smoke run). The
    // same bridge feeds the MsgInput builder, so prediction and the
    // authoritative server see the identical input.
    clientInput_.SetBase(Input());
    rcfg.input = &clientInput_;
    rcfg.font2d = cjkFont_.Valid() ? cjkFont_ : pixelFont_;
    rcfg.rngSeed = cfg_.rngSeed;
    core::Status st = runtime_.Start(sceneJson_, rcfg);
    if (!st.Ok()) {
        NEON_LOG_ERROR("Player: runtime start failed: %s", st.Error().c_str());
        CleanupUnpackedDir();
        return false;
    }
    // P2-4: wire the Lua Rpc() binding to the networked channel and register
    // the client-side informational handlers.
    runtime_.ScriptContext().rpcCall = [this](const std::string& name,
                                              const std::string& argsJson) {
        SendRpc(name, argsJson);
    };
    started_ = true;
    NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Info,
                 "player: '%s' started (%zu entities, %zu scripts, %zu trees, %zu draws)",
                 title_.c_str(), runtime_.EntityCount(), runtime_.ScriptCount(),
                 runtime_.BehaviorTreeCount(), runtime_.DrawCount());

    // T6.4 --connect: join the server (UdpSocket + ReliableChannel + MsgJoin),
    // then per frame send MsgInput, receive MsgSnapshot into ClientSync and
    // reconcile the controlled entity.
    if (!cfg_.connectHost.empty() && cfg_.connectPort != 0) {
        networked_ = true;
        if (SmokeActive()) clientInput_.SetForceMove(true);
        if (!StartNetwork()) {
            CleanupUnpackedDir();
            return false;
        }
        ResolveControlledEntity(); // the script spawns the player in on_start
    }
    return true;
}

void PlayerApp::CleanupUnpackedDir() {
    if (!cfg_.keep && !cfg_.unpackedDir.empty()) {
        RemoveTree(cfg_.unpackedDir);
        cfg_.unpackedDir.clear();
    }
}

bool PlayerApp::LoadSceneJson() {
    // T6.4 --connect with a loose scene FILE (no pack): load it directly and
    // resolve scripts from --scripts or the scene file's directory, exactly
    // like neon_server does. The client runs the SAME scene as the server; the
    // server is authoritative.
    if (!cfg_.looseScenePath.empty()) {
        if (!ReadFileAll(cfg_.looseScenePath, sceneJson_)) {
            NEON_LOG_ERROR("Player: cannot read scene '%s'", cfg_.looseScenePath.c_str());
            return false;
        }
        StripBom(sceneJson_);
        if (cfg_.scriptsDir.empty()) cfg_.scriptsDir = ScriptBaseForLooseScene(cfg_.looseScenePath);
        NEON_LOG_INFO("Player: loading loose scene '%s' (scripts from '%s')",
                      cfg_.looseScenePath.c_str(), cfg_.scriptsDir.c_str());
        return true;
    }

    std::string scenePath = cfg_.manifest.startScene;
    if (!cfg_.sceneOverride.empty()) {
        scenePath = cfg_.sceneOverride;
        // A bare name (no slash, no extension) maps onto assets/scenes/<name>.json.
        if (scenePath.find('/') == std::string::npos &&
            scenePath.find('\\') == std::string::npos &&
            scenePath.find('.') == std::string::npos) {
            scenePath = "assets/scenes/" + scenePath + ".json";
        }
    }
    // Defense-in-depth: reject a startScene/--scene that could escape the
    // pack (a hostile pack's manifest or a hand-typed override).
    if (core::IsUnsafeRelPath(scenePath)) {
        NEON_LOG_ERROR("Player: unsafe scene path '%s'", scenePath.c_str());
        return false;
    }
    // G7-1 剩余: with a VFS installed (pack + Mods, no unpack), read the scene
    // through it; loose mode falls back to the unpacked dir on disk.
    if (cfg_.vfs) {
        const core::Result<std::vector<uint8_t>> sceneBytes = cfg_.vfs->ReadFile(scenePath);
        if (!sceneBytes.Ok()) {
            NEON_LOG_ERROR("Player: cannot read scene '%s' from pack", scenePath.c_str());
            return false;
        }
        sceneJson_.assign(sceneBytes.Value().begin(), sceneBytes.Value().end());
    } else if (!ReadFileAll(cfg_.unpackedDir + "/" + scenePath, sceneJson_)) {
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
    if (networked_) {
        // T6.4 networked frame: ingest snapshots, send this frame's input,
        // step LOCAL prediction with the same input, then reconcile the
        // controlled entity against the latest server snapshot.
        PumpNetwork();
        SendInputPacket();
        runtime_.Tick(dt);
        ReconcileControlled();
    } else {
        runtime_.Tick(dt);
    }
    if (Input()->Pressed(platform::Key::Escape)) Window()->RequestClose();
}

// Orbit camera around a focus point. Scripts control the focus through the
// GameVar "cameraFocus" = {x,y,z} (data-driven framing); the default is the
// world origin. Mouse drag orbits, wheel zooms.
//
// MOUSE OWNERSHIP: by default the orbit camera is the SOLE consumer of the
// frame's MouseDelta/WheelDelta — after applying them it consumes the input,
// so scripts reading InputMouseX/Y see 0 (no double-consume). A scene that
// needs exclusive mouse control (e.g. an FPS look script) sets the GameVar
// "cameraMouseLock" to a truthy value: the camera then yields entirely and
// scripts read the raw delta. The mode flips one frame after the script sets
// the var (the camera runs before scripts in OnUpdate).
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
    const script::Value lock = runtime_.GameVars().Get("cameraMouseLock");
    const bool mouseLocked = (lock.type == script::Value::Type::Number &&
                              lock.number != 0.0) ||
                             (lock.type == script::Value::Type::Bool && lock.boolean);
    if (!mouseLocked) {
        yaw_ += -input->MouseDelta().x * 0.004f;
        pitch_ += -input->MouseDelta().y * 0.004f;
        pitch_ = math::Clamp(pitch_, -1.3f, 1.3f);
        float wheel = input->WheelDelta();
        if (std::fabs(wheel) > 0.01f)
            camDist_ = math::Clamp(camDist_ - wheel * 1.5f, 2.0f, 80.0f);
        // The camera owns the mouse by default: consume so scripts see zero.
        input->ConsumeMouseDelta();
        input->ConsumeWheel();
    }

    math::Vec3 offset{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
                      std::cos(yaw_) * std::cos(pitch_)};
    camera_.position = focus + offset * camDist_;
    camera_.target = focus;
    camera_.up = {0, 1, 0};
    // Data-driven scripts use the orbit yaw for camera-relative movement.
    runtime_.GameVars().Set("cameraYaw", script::Value::Num(yaw_));
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
        if (networked_)
            DrawNetworkWorld();
        else
            runtime_.Draw(renderer_, camera_);
    }
    renderer_.EndScene();

    // G5-4-4: the on_render HUD canvas flushes AFTER the composite (raw colors,
    // on top); the data-driven UI draws on top of it.
    if (started_) runtime_.FlushCanvas(renderer_);
    // Data-driven UI (UIShow menus/HUD) draws on top of the composited frame
    // so its colors are exactly what the author picked (not tone-mapped with
    // the 3D scene). The 2D canvas / scene content stays in the HDR target.
    if (started_) runtime_.DrawUI(renderer_);
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

    char buf[160];
    if (networked_) {
        const char* state = connectedLost_            ? "LOST"
                            : !loggedIn_              ? "LOGGING IN"
                            : !welcomed_              ? "LOGGED IN"
                            : snapshotsReceived_ > 0  ? "LINKED"
                            : "CONNECTED";
        std::snprintf(buf, sizeof(buf),
                      "%s | %.0f FPS | tick %.0f | snaps %u | server %s:%u",
                      state, TimeRef().Fps(), sync_.CurrentServerTick(),
                      snapshotsReceived_, cfg_.connectHost.c_str(), cfg_.connectPort);
        const float wsnap = static_cast<float>(w) - 500.0f;
        ui::DrawLabel(renderer_, theme_, buf,
                      {wsnap < 18.0f ? 18.0f : wsnap, static_cast<float>(h) - 48}, 12,
                      theme_.text.WithAlpha(0.55f), false, false);
        std::snprintf(buf, sizeof(buf), "snapshots received: %u | 预测本地: %s | 网络插值: 方框占位",
                      snapshotsReceived_, controlledMoved_ ? "移动" : "静止");
        ui::DrawLabel(renderer_, theme_, buf, {18, static_cast<float>(h) - 48}, 12,
                      theme_.text.WithAlpha(0.55f), false, false);
        return;
    }
    std::snprintf(buf, sizeof(buf), "%.0f FPS | 鼠标拖动旋转 滚轮缩放", TimeRef().Fps());
    math::Vec2 size = ui::MeasureText(theme_.font, buf, 12);
    ui::DrawLabel(renderer_, theme_, buf,
                  {static_cast<float>(w) - size.x - 14, static_cast<float>(h) - 28}, 12,
                  theme_.text.WithAlpha(0.55f), false, false);
}

// ---------------------------------------------------------------------------
// T6.4 networked client: join, send inputs, receive snapshots, predict +
// reconcile. The wire format (EncodeBody + MsgJoin/Input/Despawn) matches
// server::EncodeBody exactly, so the player and the server speak the same
// protocol.
// ---------------------------------------------------------------------------

uint64_t PlayerApp::EntityKey(const ecs::Entity& e) const {
    // Matches GameServer::EntityKey: stable across id reuse via the generation.
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

bool PlayerApp::SmokeActive() const {
    return cfg_.smokeFrames > 0 || cfg_.connectTicks > 0;
}

bool PlayerApp::StartNetwork() {
    core::Result<net::UdpSocket> sock = net::UdpSocket::Create();
    if (!sock.Ok()) {
        NEON_LOG_ERROR("client: socket create failed: %s", sock.Error().c_str());
        return false;
    }
    clientSock_ = std::move(sock.Value());
    if (!clientSock_.BindLoopback(0).Ok()) {
        NEON_LOG_ERROR("client: cannot bind local UDP socket");
        clientSock_.Close();
        return false;
    }
    if (!clientSock_.SetPeer(net::NetAddress{cfg_.connectHost, cfg_.connectPort}).Ok()) {
        NEON_LOG_ERROR("client: cannot set peer %s:%u", cfg_.connectHost.c_str(),
                       cfg_.connectPort);
        clientSock_.Close();
        return false;
    }
    clientChan_.SetOutbound([this](const std::vector<uint8_t>& bytes) {
        if (clientSock_.Valid()) clientSock_.Send(bytes.data(), bytes.size());
    });
    clientChan_.SetDeliver([this](const net::DecodedMessage& m) { OnClientMessage(m); });
    clientChan_.SetTimeout([this]() {
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Warn,
                     "client: reliable channel to %s:%u timed out",
                     cfg_.connectHost.c_str(), cfg_.connectPort);
        connectedLost_ = true;
    });

    // T6.6 v0 anonymous login: the FIRST network step. The server accepts any
    // non-empty name, replies MsgLoginOk + MsgCharList, and only then do we
    // send the T6.4 game join (see SendJoin / the LoginOk handler).
    net::MsgLogin login{cfg_.playerName, net::kProtocolVersion};
    core::Status st =
        clientChan_.Send(static_cast<uint8_t>(net::MsgType::Login), client::EncodeBody(login));
    if (!st.Ok()) {
        NEON_LOG_ERROR("client: login send failed: %s", st.Error().c_str());
        clientSock_.Close();
        return false;
    }
    NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                 "client: connecting to %s:%u (login name '%s')",
                 cfg_.connectHost.c_str(), cfg_.connectPort, cfg_.playerName.c_str());
    return true;
}

// The T6.4 game join (MsgJoin -> MsgWelcome -> snapshots). Sent once, only
// after the account step completed (MsgLoginOk), so the join/transport flow is
// gated behind the login.
void PlayerApp::SendJoin() {
    if (joinSent_) return;
    joinSent_ = true;
    net::MsgJoin join{cfg_.playerName, net::kProtocolVersion};
    core::Status st =
        clientChan_.Send(static_cast<uint8_t>(net::MsgType::Join), client::EncodeBody(join));
    if (!st.Ok()) {
        NEON_LOG_ERROR("client: join send failed: %s", st.Error().c_str());
    }
}

void PlayerApp::OnClientMessage(const net::DecodedMessage& msg) {
    switch (static_cast<net::MsgType>(msg.header.msgId)) {
        case net::MsgType::LoginOk: {
            const net::MsgLoginOk& ok = std::get<net::MsgLoginOk>(msg.payload);
            loggedIn_ = true;
            NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                         "client: logged in as account id=%llu (server tick %u)",
                         static_cast<unsigned long long>(ok.accountId), ok.tick);
            SendJoin(); // logged in: now join the game (welcome follows)
            break;
        }
        case net::MsgType::CharList: {
            const net::MsgCharList& list = std::get<net::MsgCharList>(msg.payload);
            std::string names;
            for (size_t i = 0; i < list.characters.size(); ++i) {
                if (i != 0) names += ", ";
                names += list.characters[i].name;
            }
            NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                         "client: characters: %u (%s)", list.count, names.c_str());
            break;
        }
        case net::MsgType::Welcome: {
            const net::MsgWelcome& w = std::get<net::MsgWelcome>(msg.payload);
            welcomed_ = true;
            NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                         "client: welcomed as client id=%llu (server tick %u)",
                         static_cast<unsigned long long>(w.clientId), w.tick);
            break;
        }
        case net::MsgType::Snapshot: {
            const net::MsgSnapshot& s = std::get<net::MsgSnapshot>(msg.payload);
            sync_.OnSnapshot(s);
            ++snapshotsReceived_;
            break;
        }
        case net::MsgType::Despawn:
            sync_.OnDespawn(std::get<net::MsgDespawn>(msg.payload).entityId);
            break;
        case net::MsgType::Rpc:
            HandleRpc(std::get<net::MsgRpc>(msg.payload));
            break;
        case net::MsgType::Pong:
        case net::MsgType::Join:
        case net::MsgType::Input:
        case net::MsgType::Spawn:
        case net::MsgType::Ping:
        case net::MsgType::Ack:
            break; // client-authoritative messages are ignored
    }
}

void PlayerApp::HandleRpc(const net::MsgRpc& rpc) {
    std::optional<std::pair<std::string, std::string>> reply;
    if (clientRpc_.Dispatch(0, rpc.name, rpc.argsJson, &reply)) {
        if (reply) SendRpc(reply->first, reply->second);
        return;
    }
    // Built-in informational RPCs the server sends are logged.
    if (rpc.name == "room.chat") {
        std::string from, message;
        std::string perr;
        core::Json args = core::Json::Parse(rpc.argsJson, &perr);
        if (const core::Json* f = args.Get("from"))
            from = std::to_string(static_cast<uint64_t>(f->GetNumber()));
        if (const core::Json* m = args.Get("message")) message = m->GetString();
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                     "client: room.chat from=%s: %s", from.c_str(), message.c_str());
    } else if (rpc.name == "room.joined" || rpc.name == "room.left" ||
               rpc.name == "room.list" || rpc.name == "world.hash") {
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                     "client: rpc '%s' %s", rpc.name.c_str(), rpc.argsJson.c_str());
    } else {
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Debug,
                     "client: unhandled rpc '%s'", rpc.name.c_str());
    }
}

void PlayerApp::SendRpc(const std::string& name, const std::string& argsJson) {
    if (!welcomed_) return;
    net::MsgRpc rpc{name, argsJson};
    clientChan_.Send(static_cast<uint8_t>(net::MsgType::Rpc),
                     client::EncodeBody(rpc));
}

void PlayerApp::PumpNetwork() {
    if (connectedLost_) return;
    uint8_t buf[4096];
    for (;;) {
        core::Result<net::RecvPacket> r = clientSock_.RecvFrom(buf, sizeof(buf));
        if (!r.Ok() || r.Value().size == 0) break;
        clientChan_.OnDatagram(buf, r.Value().size);
    }
    // Monotonic clock in ms (accumulated fixed ticks * 1000); the reliable
    // channel only needs a monotonic clock for retransmit/timeout/ack pacing.
    const uint64_t nowMs = static_cast<uint64_t>(TimeRef().elapsed * 1000.0);
    clientChan_.Tick(nowMs);
    // A10: 1 Hz heartbeat so the server measures a real RTT and automatic lag
    // compensation actually rewinds (without pings AutoLagCompTicks stays 0).
    if (nowMs - lastPingMs_ >= 1000u) {
        lastPingMs_ = nowMs;
        net::MsgPing ping{nowMs};
        clientChan_.Send(static_cast<uint8_t>(net::MsgType::Ping), net::EncodeBody(ping));
    }
    if (connectedLost_ && SmokeActive()) {
        NEON_LOG_WARN("client: connection lost; smoke run will report failure");
    }
}

void PlayerApp::SendInputPacket() {
    if (connectedLost_ || clientChan_.TimedOut()) return;
    // Derive MsgInput from the same input the local prediction reads
    // (bitmask + axes match the server's NetInput mapping).
    uint8_t buttons = 0;
    if (clientInput_.IsDown(platform::Key::Space)) buttons |= client::kButtonJump;
    if (clientInput_.IsDown(platform::Key::Shift)) buttons |= client::kButtonSprint;
    if (clientInput_.IsDown(platform::Key::Control)) buttons |= client::kButtonControl;
    if (clientInput_.IsDown(platform::Key::F)) buttons |= client::kButtonInteract;
    const float moveX = (clientInput_.IsDown(platform::Key::D) ? 1.0f : 0.0f) -
                        (clientInput_.IsDown(platform::Key::A) ? 1.0f : 0.0f);
    const float moveY = (clientInput_.IsDown(platform::Key::W) ? 1.0f : 0.0f) -
                        (clientInput_.IsDown(platform::Key::S) ? 1.0f : 0.0f);
    net::MsgInput in{inputSeq_++, buttons, moveX, moveY};
    core::Status st =
        clientChan_.Send(static_cast<uint8_t>(net::MsgType::Input), client::EncodeBody(in));
    if (!st.Ok() && ++sendDropLogs_ <= 3) {
        // The send window can be full (e.g. the server is slow to ack); the
        // input is simply not sent this frame. Throttled so it never spams.
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Debug,
                     "client: input %u deferred (%s)", inputSeq_ - 1, st.Error().c_str());
    }
}

void PlayerApp::ResolveControlledEntity() {
    if (controlledKey_ != 0) return;
    ecs::World& world = runtime_.World();
    auto view = world.ViewAll<script::CTransformBind>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
        controlledEntity_ = e;
        controlledKey_ = EntityKey(e);
        const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
        if (t) controlledStartPos_ = t->pos;
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                     "client: controlled entity id=%u gen=%u key=%llu start=(%.2f,%.2f,%.2f)",
                     e.id, e.generation, static_cast<unsigned long long>(controlledKey_),
                     controlledStartPos_.x, controlledStartPos_.y, controlledStartPos_.z);
        break;
    }
}

void PlayerApp::ReconcileControlled() {
    if (!welcomed_ || controlledKey_ == 0) return;
    ecs::World& world = runtime_.World();
    script::CTransformBind* t = world.Get<script::CTransformBind>(controlledEntity_);
    if (!t) return;
    math::Vec3 correction;
    if (sync_.NeedsReconcile(controlledKey_, t->pos, &correction)) {
        // v1 snap-on-divergence: the server is authoritative. No replay yet
        // (T6.7 can add multi-frame re-simulation).
        t->pos = correction;
        if (!reconcileLogged_) {
            NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Warn,
                         "client: reconciled controlled entity to server (%.2f,%.2f,%.2f)",
                         correction.x, correction.y, correction.z);
            reconcileLogged_ = true;
        }
    }
    if (controlledKey_ != 0 && t->pos.z - controlledStartPos_.z > 0.5f) controlledMoved_ = true;
}

void PlayerApp::DrawNetworkWorld() {
    // Placeholder visual (T6.4): the LOCAL world is used only for prediction;
    // rendering shows the interpolated remote entities as wireframe boxes plus
    // the controlled entity at its locally PREDICTED position (highlighted).
    // A real networked renderer would draw interpolated meshes for the remote
    // set instead.
    const net::MsgSnapshot* latest = sync_.Latest();
    if (!latest) return;
    double renderTick = sync_.CurrentServerTick() - static_cast<double>(client::kInterpDelayTicks);
    if (renderTick < 0.0) renderTick = 0.0;
    const float half = 0.35f;
    for (const net::SnapshotEntity& e : latest->entities) {
        if (e.id == controlledKey_) continue; // the controlled one renders below
        core::Result<client::InterpolatedEntity> s = sync_.Sample(e.id, renderTick);
        if (!s.Ok()) continue;
        const math::Vec3 p = s.Value().pos;
        const math::Vec3 lo{p.x - half, p.y - half, p.z - half};
        const math::Vec3 hi{p.x + half, p.y + half, p.z + half};
        renderer_.DrawBox(math::AABB{lo, hi}, gfx::Color{0.4f, 0.8f, 1.0f, 1.0f});
    }
    if (controlledKey_ != 0) {
        const script::CTransformBind* t =
            runtime_.World().Get<script::CTransformBind>(controlledEntity_);
        if (t) {
            const math::Vec3 p = t->pos;
            const float h2 = half * 1.8f;
            const math::Vec3 lo{p.x - h2, p.y - h2, p.z - h2};
            const math::Vec3 hi{p.x + h2, p.y + h2, p.z + h2};
            renderer_.DrawBox(math::AABB{lo, hi}, gfx::Color::Yellow);
        }
    }
}

bool PlayerApp::SmokeOk() const {
    if (!networked_) return true; // local runs keep the legacy behavior
    return welcomed_ && snapshotsReceived_ > 0 && controlledMoved_;
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
    kernel_.Shutdown();  // microkernel: tear down the physics/script modules
    if (networked_) {
        NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                     "client: shutting down (welcomed=%d snapshots=%u controlledMoved=%d "
                     "buffered=%u)",
                     welcomed_ ? 1 : 0, snapshotsReceived_, controlledMoved_ ? 1 : 0,
                     sync_.BufferedSnapshots());
        clientChan_.Reset();
        clientSock_.Close();
        if (SmokeActive()) {
            NEON_LOG_INFO("client: smoke assertions %s", SmokeOk() ? "passed" : "FAILED");
        }
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
