#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "demo.hpp"
#include "neon/core/config.hpp"

namespace {

void PrintHelp() {
    std::printf(
        "NeonRealm - NeonEngine 3D demo\n"
        "Usage: neon_rush [options]\n"
        "  --smoke-test <frames>  run headless-ish for N simulation frames then exit\n"
        "  --no-audio             disable audio\n"
        "  --disable-fbo          force-disable CSM shadow maps (CPU projected shadows)\n"
        "  --no-shadows           alias for --disable-fbo\n"
        "  --fullscreen           start in fullscreen\n"
        "  --log-level <level>    log filter: debug|info|warn|error (default debug)\n"
        "  --log-cat <n>:<level>  per-category override (repeatable, comma-separated,\n"
        "                         e.g. gfx:debug); names: core,gfx,audio,physics,scene,\n"
        "                         script,bt,net,editor,game\n"
        "  --help                 show this help\n");
}

} // namespace

int main(int argc, char** argv) {
    neon::core::ApplyLogCli(argc, argv);
    int smokeFrames = 0;
    bool noAudio = false;
    bool fullscreen = false;
    bool disableShadows = false;
    std::string screenshotPath;
    uint64_t screenshotFrame = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshotPath = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-audio") == 0) {
            noAudio = true;
        } else if (std::strcmp(argv[i], "--disable-fbo") == 0 ||
                   std::strcmp(argv[i], "--no-shadows") == 0) {
            disableShadows = true;
        } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintHelp();
            return 0;
        }
    }

    neon::platform::WindowConfig config;
    config.title = "NeonRealm - NeonEngine 3D Demo";
    config.width = 1280;
    config.height = 720;
    config.resizable = true;
    config.vsync = true;
    config.glMajor = 4;
    config.glMinor = 6;
    (void)fullscreen;
    (void)noAudio;

    neon::demo::NeonApp app;
    if (smokeFrames > 0) {
        app.SetSmokeMode(true);
        app.SetSmokeTestFrames(smokeFrames);
    }
    if (!screenshotPath.empty()) app.RequestScreenshot(screenshotPath, screenshotFrame);
    if (disableShadows) app.SetDisableShadows(true);
    int result = app.Run(config);
    std::printf("NeonRealm exited with code %d\n", result);
    return result;
}
