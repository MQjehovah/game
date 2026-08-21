#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor.hpp"
#include "neon/core/config.hpp"

int main(int argc, char** argv) {
    neon::core::ApplyLogCli(argc, argv);
    int smokeFrames = 0;
    bool disableShadows = false;
    std::string screenshot;
    uint64_t screenshotFrame = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshot = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--disable-fbo") == 0 ||
                   std::strcmp(argv[i], "--no-shadows") == 0) {
            disableShadows = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("NeonEditor - NeonEngine scene editor\n"
                        "  --smoke-test <frames>  run N simulation frames then exit\n"
                        "  --screenshot <path> <frame>  capture a PNG at frame N\n"
                        "  --disable-fbo          force-disable CSM shadow maps\n"
                        "  --no-shadows           alias for --disable-fbo\n"
                        "  --log-level <level>    log filter: debug|info|warn|error\n"
                        "  --log-cat <n>:<level>  per-category override (repeatable,\n"
                        "                         comma-separated, e.g. gfx:debug)\n");
            return 0;
        }
    }

    neon::platform::WindowConfig config;
    config.title = "NeonEditor";
    config.width = 1280;
    config.height = 720;
    config.resizable = true;
    config.vsync = true;
    config.glMajor = 4;
    config.glMinor = 6;

    neon::editor::EditorApp app;
    if (smokeFrames > 0) {
        app.SetSmokeMode(true);
        app.SetSmokeTestFrames(smokeFrames);
    }
    if (!screenshot.empty()) app.RequestScreenshot(screenshot, screenshotFrame);
    if (disableShadows) app.SetDisableShadows(true);
    int code = app.Run(config);
    return app.SmokeFailed() ? 1 : code;
}
