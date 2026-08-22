#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "editor.hpp"
#include "neon/core/config.hpp"
#include "packager.hpp"

int main(int argc, char** argv) {
    neon::core::ApplyLogCli(argc, argv);
    int smokeFrames = 0;
    bool disableShadows = false;
    bool disableBloom = false;
    bool hotReload = false;
    std::string backend = "gl";
    std::string screenshot;
    uint64_t screenshotFrame = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hot") == 0) {
            hotReload = true;
        } else if (std::strcmp(argv[i], "--package") == 0 && i + 2 < argc) {
            const std::string projectDir = argv[++i];
            const std::string outDir = argv[++i];
            neon::editor::pack::PackConfig cfg;
            cfg.projectDir = projectDir;
            cfg.outDir = outDir;
            cfg.playerSource = "build/neon_game.exe";
            neon::editor::pack::PackageReport r = neon::editor::pack::PackProject(cfg);
            for (const std::string& e : r.errors) std::printf("PACK ERROR: %s\n", e.c_str());
            for (const std::string& w : r.warnings) std::printf("PACK WARN:  %s\n", w.c_str());
            if (r.ok) {
                std::printf("PACK OK: %s (%zu files, %zu bytes)\n", r.packPath.c_str(),
                            r.fileCount, r.bytesWritten);
                std::printf("PACK RUN: %s\n", r.runScriptPath.c_str());
                if (!r.playerPath.empty())
                    std::printf("PACK PLAYER: %s\n", r.playerPath.c_str());
            } else {
                std::printf("PACK FAILED: %zu errors, %zu warnings\n", r.errors.size(),
                            r.warnings.size());
            }
            return r.ok ? 0 : 1;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshot = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--disable-fbo") == 0 ||
                   std::strcmp(argv[i], "--no-shadows") == 0) {
            disableShadows = true;
        } else if (std::strcmp(argv[i], "--no-bloom") == 0) {
            disableBloom = true;
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("NeonEditor - NeonEngine scene editor\n"
                        "  --smoke-test <frames>  run N simulation frames then exit\n"
                        "  --hot                  enable hot reload (scripts/assets on mtime change)\n"
                        "  --backend <gl|vulkan>  graphics backend (default gl; vulkan is opt-in)\n"
                        "  --package <project> <out>  validate + pack a project into\n"
                        "                         <out>/game.pack (run.bat + neon_game.exe)\n"
                        "  --screenshot <path> <frame>  capture a PNG at frame N\n"
                        "  --disable-fbo          force-disable CSM shadow maps\n"
                        "  --no-shadows           alias for --disable-fbo\n"
                        "  --no-bloom             disable the HDR bloom post-process\n"
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
    if (disableBloom) app.SetBloomEnabled(false);
    if (hotReload) app.SetHotReload(true);
    if (backend != "gl") app.SetBackendName(backend);
    int code = app.Run(config);
    return app.SmokeFailed() ? 1 : code;
}
