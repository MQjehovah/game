#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "neon/neon.hpp"
#include "neon/assets/asset_variants.hpp"
#include "neon/scene/game_manifest.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/net/rpc.hpp"
#include "client_input.hpp"
#include "client_sync.hpp"

namespace neon::player {

// Result of booting a pack before the window exists: the unpacked temp dir
// (owned/cleaned by the caller) plus the parsed game.json manifest, so the
// window size/title can come from the manifest.
struct PackBoot {
    std::string unpackedDir;
    scene::GameManifest manifest;
    // G7-1: layered file system [pack, mod dirs...] kept alive for the
    // session; mods override base-pack assets via the AssetManager.
    std::shared_ptr<neon::io::MountStack> vfs;
};

// Reads, validates and unpacks `packPath` into a fresh OS temp dir, then loads
// game.json. Returns Err with a message on any failure (missing/invalid pack,
// unpack failure, bad manifest). The caller owns the unpacked dir's lifecycle
// (neon_game removes it at exit unless --keep).
core::Result<PackBoot> BootPack(const std::string& packPath,
                                const std::vector<std::string>& modDirs);

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
    // T6.4 --connect host:port: join a GameServer, send inputs, receive
    // snapshots and render the interpolated/reconciled state.
    std::string connectHost;      // --connect host:port ("" = local-only)
    uint16_t connectPort = 0;
    std::string playerName = "neon_player"; // --name <n>: anonymous login name (T6.6)
    std::string scriptsDir;       // --scripts DIR: scene script base (loose scene mode)
    std::string looseScenePath;   // --scene <path.json> in connect mode (direct file)
    int connectTicks = 0;         // --ticks <n>: run n frames then exit (connect smoke)
    uint64_t rngSeed = 20260821u; // --seed: local prediction RNG (must match the server)
    std::string backend = "gl";   // --backend gl|vulkan: graphics backend (default gl)
    // G7-1: layered file system (pack + mods) installed on the AssetManager.
    std::shared_ptr<neon::io::MountStack> vfs;
    // G7-1 --mod <dir> (repeatable): a Mod overlay directory mounted over the
    // base pack; later mods win. Overrides both assets (VFS) and the unpacked
    // script/prefab/locale tree.
    std::vector<std::string> modDirs;
    // G6-1 --variant <name>: platform/LOD asset variant selected from the
    // project's variants.json ("mobile"/"pc"/...). "" = base assets only.
    std::string variant;
};

// The data-driven player: unpack a store-only pack to a temp dir, read the
// game.json manifest, boot the manifest's start scene into a GameRuntime, and
// render it with the engine renderer. No hardcoded gameplay — the scene's
// scripts and behavior trees are the whole game. Camera is a free orbit around
// a focus point (world origin by default, overridable by a script via the
// "cameraFocus" GameVar = {x,y,z}); mouse drag orbits, wheel zooms, Esc quits.
// WASD does NOT move the camera: data-driven scripts read input themselves.
//
// CONNECT MODE (T6.4, --connect host:port): the player joins a GameServer as
// the input controller and renders the networked world. Local prediction: the
// same scene runs locally in the runtime with the same inputs. Every frame the
// player sends a MsgInput derived from the real input (ClientInput), steps the
// local runtime (prediction), receives MsgSnapshot via a reliable channel into
// ClientSync, and reconciles the controlled entity: when the locally predicted
// position diverges past the threshold, it snaps to the authoritative server
// position (v1 snap-on-divergence; no replay — T6.7 can add it). Rendering is
// a placeholder visual: the interpolated remote entities (from the snapshot
// buffer) plus the controlled entity at its locally predicted position are
// drawn as wireframe boxes. --ticks N runs N frames then exits 0 when the
// smoke assertions hold (welcomed, snapshots received, controlled entity
// moved) — the loopback smoke test for the networking layer.
//
// MOUSE OWNERSHIP: by default the orbit camera consumes MouseDelta/WheelDelta
// each frame, so scripts see 0 via InputMouseX/Y (no double-consume). A scene
// that needs exclusive mouse control (e.g. an FPS look script) sets the
// GameVar "cameraMouseLock" (truthy) and the camera yields entirely, letting
// the script read the raw delta. See UpdateCamera for details.
class PlayerApp : public core::Application {
public:
    explicit PlayerApp(PlayerConfig cfg) : cfg_(std::move(cfg)) {}

    bool OnCreate() override;
    void OnShutdown() override;
    void OnUpdate(float dt) override;
    void OnRender() override;
    void OnEvent(const platform::InputEvent& event) override;

    // Smoke-mode exit gate (--ticks in --connect mode): true when the network
    // smoke assertions held (welcomed, snapshots received, controlled entity
    // moved). Always true without --connect.
    bool SmokeOk() const;

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

    // ---- T6.4 networked-client helpers ----------------------------------
    bool StartNetwork();
    void SendJoin(); // T6.6: the game join, only sent after MsgLoginOk
    void OnClientMessage(const net::DecodedMessage& msg);
    // P2-4 production RPC: client-side dispatch + send.
    void HandleRpc(const net::MsgRpc& rpc);
    void SendRpc(const std::string& name, const std::string& argsJson);
    void PumpNetwork();
    void SendInputPacket();
    void ResolveControlledEntity();
    void ReconcileControlled();
    void DrawNetworkWorld();
    uint64_t EntityKey(const ecs::Entity& e) const;
    bool SmokeActive() const;

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
    // G6-1: platform/LOD asset variant table (loaded from variants.json when
    // cfg_.variant is set; must outlive the runtime, so it lives here).
    assets::AssetVariantTable variantTable_;

    // T6.4 network state.
    bool networked_ = false;
    client::ClientInput clientInput_;      // bridges real input -> prediction + wire
    net::UdpSocket clientSock_;
    net::ReliableChannel clientChan_;
    net::RpcDispatcher clientRpc_;
    client::ClientSync sync_;              // snapshot buffer + interp + reconcile query
    bool loggedIn_ = false;  // MsgLoginOk received (T6.6 account step done)
    bool joinSent_ = false;  // MsgJoin sent (only after login)
    bool welcomed_ = false;
    bool connectedLost_ = false;
    uint32_t inputSeq_ = 0;
    uint64_t lastPingMs_ = 0; // A10: last heartbeat Ping send time (1 Hz)
    ecs::Entity controlledEntity_;
    uint64_t controlledKey_ = 0;           // stable (id<<32)|generation key
    math::Vec3 controlledStartPos_;
    bool controlledMoved_ = false;
    uint32_t snapshotsReceived_ = 0;
    bool reconcileLogged_ = false;
    uint32_t sendDropLogs_ = 0;
};

} // namespace neon::player
