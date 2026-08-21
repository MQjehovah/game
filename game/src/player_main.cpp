#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "neon/core/config.hpp"
#include "player.hpp"

namespace {

void PrintHelp() {
    std::printf(
        "neon_game - NeonEngine generic data-driven player\n"
        "Usage: neon_game --pack <file> [options]\n"
        "  --pack <file>              game.pack to run (required)\n"
        "  --scene <name>             override the manifest startScene; a bare name\n"
        "                             maps to scenes/<name>.json\n"
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

} // namespace

int main(int argc, char** argv) {
    neon::core::ApplyLogCli(argc, argv);
    neon::player::PlayerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            cfg.packPath = argv[++i];
        } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            cfg.sceneOverride = argv[++i];
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

    if (cfg.packPath.empty()) {
        std::fprintf(stderr, "neon_game: no --pack file given (see --help)\n");
        return 1;
    }

    auto boot = neon::player::BootPack(cfg.packPath);
    if (!boot.Ok()) {
        std::fprintf(stderr, "neon_game: %s\n", boot.Error().c_str());
        return 1;
    }
    cfg.unpackedDir = boot.Value().unpackedDir;
    cfg.manifest = boot.Value().manifest;
    const int smokeFrames = cfg.smokeFrames;

    neon::platform::WindowConfig config;
    config.title = cfg.manifest.title.empty() ? "Neon Game" : cfg.manifest.title;
    config.width = cfg.manifest.window.w;
    config.height = cfg.manifest.window.h;
    config.resizable = true;
    config.vsync = true;
    config.glMajor = 4;
    config.glMinor = 6;

    neon::player::PlayerApp app(std::move(cfg));
    if (smokeFrames > 0) app.SetSmokeTestFrames(smokeFrames);
    int result = app.Run(config);
    std::printf("neon_game exited with code %d\n", result);
    return result;
}
