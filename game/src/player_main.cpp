#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "neon/core/config.hpp"
#include "neon/core/crash.hpp"
#include "player.hpp"

namespace {

void PrintHelp() {
    std::printf(
        "neon_game - NeonEngine generic data-driven player\n"
        "Usage: neon_game --pack <file> [options]  |  --scene <file> [--scripts DIR]\n"
        "  --pack <file>              game.pack to run\n"
        "  --scene <name|path>        a file path loads it as a loose scene (standalone,\n"
        "                             no --pack). a bare name maps to scenes/<name>.json\n"
        "                             inside the pack (with --pack)\n"
        "  --connect host:port        join a GameServer as the input controller (T6.4):\n"
        "                             local prediction + snapshot interpolation + reconcile\n"
        "  --name <n>                 anonymous login name (T6.6; default neon_player)\n"
        "  --mod <dir>                Mod overlay dir mounted over the base pack\n"
        "                             (repeatable; later mods win)\n"
        "  --scripts DIR              scene script base dir (loose --scene mode; defaults\n"
        "                             to the scene file's directory, like neon_server)\n"
        "  --ticks <n>                run n frames then exit; in --connect mode the exit is\n"
        "                             0 only when snapshots were received and the controlled\n"
        "                             entity moved (smoke assertion)\n"
        "  --seed <n>                 local prediction RNG seed (must match the server's\n"
        "                             --seed for deterministic scenes)\n"
        "  --backend <gl|vulkan>      graphics backend (default gl; vulkan is opt-in and\n"
        "                             requires a NEON_ENABLE_VULKAN build)\n"
        "  --smoke-test <n>           run n fixed ticks then exit 0 (verification)\n"
        "  --screenshot <file> <n>    capture a PNG at frame n\n"
        "  --dump-vars                log every GameVar at exit (verification)\n"
        "  --keep                     keep the unpacked temp dir on exit (debug)\n"
        "  --log-level <level>        log filter: debug|info|warn|error (default debug)\n"
        "  --log-cat <n>:<level>      per-category override (repeatable, comma-separated,\n"
        "                             e.g. gfx:debug); names: core,gfx,audio,physics,scene,\n"
        "                             script,bt,net,editor,game\n"
        "  --help                     show this help\n");
}

// True when a --scene value is a direct file path rather than a pack scene
// name (contains a path separator or ends with .json).
bool LooksLikePath(const std::string& s) {
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
           s.size() > 5 && s.compare(s.size() - 5, 5, ".json") == 0;
}

} // namespace

int main(int argc, char** argv) {
    neon::core::ApplyLogCli(argc, argv);
    neon::core::InstallCrashHandler();
    neon::player::PlayerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            cfg.packPath = argv[++i];
        } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            cfg.sceneOverride = argv[++i];
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            std::string hostPort = argv[++i];
            const size_t colon = hostPort.rfind(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "neon_game: --connect expects host:port\n");
                return 2;
            }
            cfg.connectHost = hostPort.substr(0, colon);
            cfg.connectPort =
                static_cast<uint16_t>(std::atoi(hostPort.substr(colon + 1).c_str()));
            if (cfg.connectHost.empty() || cfg.connectPort == 0) {
                std::fprintf(stderr, "neon_game: bad --connect endpoint '%s'\n",
                             hostPort.c_str());
                return 2;
            }
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            cfg.playerName = argv[++i];
        } else if (std::strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
            cfg.modDirs.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--scripts") == 0 && i + 1 < argc) {
            cfg.scriptsDir = argv[++i];
        } else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            cfg.connectTicks = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.rngSeed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            cfg.variant = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            cfg.backend = argv[++i];
        } else if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            cfg.smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            cfg.screenshotPath = argv[++i];
            cfg.screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--dump-vars") == 0) {
            cfg.dumpVars = true;
        } else if (std::strcmp(argv[i], "--keep") == 0) {
            cfg.keep = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintHelp();
            return 0;
        }
    }

    const bool connectMode = !cfg.connectHost.empty() && cfg.connectPort != 0;
    // 单机松散场景: `--scene <file>` 且像文件路径 -> 直接加载该场景文件(无需 pack/网络)。
    // (联网模式同样把 --connect --scene <file> 当松散场景, 与原语义一致。)
    if (!cfg.sceneOverride.empty() && LooksLikePath(cfg.sceneOverride)) {
        cfg.looseScenePath = cfg.sceneOverride;
        cfg.sceneOverride.clear();
    }

    if (cfg.packPath.empty() && cfg.looseScenePath.empty()) {
        std::fprintf(stderr, "neon_game: no --pack (or --scene <file>) given (see --help)\n");
        return 1;
    }

    if (cfg.looseScenePath.empty()) {
        auto boot = neon::player::BootPack(cfg.packPath, cfg.modDirs);
        if (!boot.Ok()) {
            std::fprintf(stderr, "neon_game: %s\n", boot.Error().c_str());
            return 1;
        }
        cfg.unpackedDir = boot.Value().unpackedDir;
        cfg.manifest = boot.Value().manifest;
        cfg.vfs = std::move(boot.Value().vfs);
    }
    const int frames = cfg.connectTicks > 0 ? cfg.connectTicks : cfg.smokeFrames;

    neon::platform::WindowConfig config;
    config.title = cfg.manifest.title.empty()
                       ? (connectMode ? "Neon Client" : "Neon Game")
                       : cfg.manifest.title;
    config.width = cfg.manifest.window.w > 0 ? cfg.manifest.window.w : 1280;
    config.height = cfg.manifest.window.h > 0 ? cfg.manifest.window.h : 720;
    config.resizable = true;
    config.vsync = true;
    config.glMajor = 4;
    config.glMinor = 6;

    neon::player::PlayerApp app(std::move(cfg));
    if (frames > 0) app.SetSmokeTestFrames(frames);
    int result = app.Run(config);
    if (connectMode && frames > 0) {
        if (!app.SmokeOk()) {
            std::fprintf(stderr, "neon_game: connect smoke FAILED (snapshots received? "
                                 "controlled moved?)\n");
            return 1;
        }
    }
    std::printf("neon_game exited with code %d\n", result);
    return result;
}
