#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/game_manifest.hpp"
#include "neon/scene/game_runtime.hpp"

namespace neon::player {

// Result of booting a pack before the window exists: the unpacked temp dir
// (owned/cleaned by the caller) plus the parsed game.json manifest, so the
// window size/title can come from the manifest.
struct PackBoot {
    std::string unpackedDir;
    scene::GameManifest manifest;
};

// Reads, validates and unpacks `packPath` into a fresh OS temp dir, then loads
// game.json. Returns Err with a message on any failure (missing/invalid pack,
// unpack failure, bad manifest). The caller owns the unpacked dir's lifecycle
// (neon_game removes it at exit unless --keep).
core::Result<PackBoot> BootPack(const std::string& packPath);

// Command-line configuration for the generic neon_game player. Everything is
// data-driven: the pack supplies the manifest, scenes, scripts, behavior trees
// and assets; the app only reads a handful of flags.
struct PlayerConfig {
    std::string packPath;         // --pack <file> (informational; unpacked in main)
    std::string unpackedDir;      // pack already unpacked here (BootPack)
    scene::GameManifest manifest; // parsed game.json
    std::string sceneOverride;    // --scene <name>: override the manifest startScene
    int smokeFrames = 0;          // --smoke-test <n>: run n fixed ticks then exit 0
    std::string screenshotPath;   // --screenshot <file> <frame>: capture a PNG
    uint64_t screenshotFrame = 0;
    bool keep = false;            // --keep: leave the unpacked dir on exit (debug)
    bool dumpVars = false;        // --dump-vars: log every GameVar at exit
};

// The data-driven player: unpack a store-only pack to a temp dir, read the
// game.json manifest, boot the manifest's start scene into a GameRuntime, and
// render it with the engine renderer. No hardcoded gameplay — the scene's
// scripts and behavior trees are the whole game. Camera is a free orbit around
// a focus point (world origin by default, overridable by a script via the
// "cameraFocus" GameVar = {x,y,z}); mouse drag orbits, wheel zooms, Esc quits.
// WASD does NOT move the camera: data-driven scripts read input themselves.
//
// NOTE (input double-consume): the orbit camera reads MouseDelta/WheelDelta
// every frame, and scripts see the SAME accumulated values through the
// InputMouseX/Y bindings — a script "consuming" the mouse does not hide it
// from the camera (the input state only resets at EndFrame). A data-driven
// game that needs exclusive mouse control (e.g. an FPS look script) will
// require a capture/lock option on the player.
class PlayerApp : public core::Application {
public:
    explicit PlayerApp(PlayerConfig cfg) : cfg_(std::move(cfg)) {}

    bool OnCreate() override;
    void OnShutdown() override;
    void OnUpdate(float dt) override;
    void OnRender() override;
    void OnEvent(const platform::InputEvent& event) override;

private:
    bool LoadSceneJson();
    // Removes the unpacked pack dir unless --keep; safe to call repeatedly and
    // on boot-failure paths (Application::Run does not call OnShutdown when
    // OnCreate fails, so every OnCreate error path must clean up itself).
    void CleanupUnpackedDir();
    void UpdateCamera(float dt);
    void DrawOverlay();
    void CaptureScreenshotIfDue();
    void DumpGameVars();

    PlayerConfig cfg_;
    gfx::Renderer renderer_;
    assets::AssetManager assetMgr_;
    scene::GameRuntime runtime_;
    ui::Theme theme_;
    gfx::Font pixelFont_;
    gfx::Font cjkFont_;
    std::string sceneJson_;
    std::string title_;
    gfx::Camera camera_;
    math::Vec3 focus_ = {};
    float yaw_ = 0.6f;
    float pitch_ = 0.32f;
    float camDist_ = 12.0f;
    bool started_ = false;
};

} // namespace neon::player
