#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor.hpp"

int main(int argc, char** argv) {
    int smokeFrames = 0;
    std::string screenshot;
    uint64_t screenshotFrame = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshot = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("NeonEditor - NeonEngine scene editor\n"
                        "  --smoke-test <frames>  run N simulation frames then exit\n"
                        "  --screenshot <path> <frame>  capture a PNG at frame N\n");
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
    int code = app.Run(config);
    return app.SmokeFailed() ? 1 : code;
}
