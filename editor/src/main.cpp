#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "editor.hpp"
#include "neon/core/config.hpp"
#include "neon/core/crash.hpp"
#include "packager.hpp"

int main(int argc, char** argv) {
    neon::core::InstallCrashHandler();
    // Anchor the working directory to the repo root (the folder containing
    // projects/ + CMakeLists.txt), walking up from the executable. Launching
    // from Explorer (cwd = build/) would otherwise leave projects/, assets/
    // and plugins/ unresolvable and the project/scene switchers empty.
#if defined(_WIN32)
    {
        char exePath[MAX_PATH];
        const DWORD exeLen = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (exeLen > 0 && exeLen < MAX_PATH) {
            std::string dir = exePath;
            const size_t slash = dir.find_last_of("/\\");
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            for (int depth = 0; depth < 6; ++depth) {
                const bool hasRoot =
                    GetFileAttributesA((dir + "/CMakeLists.txt").c_str()) !=
                        INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesA((dir + "/projects").c_str()) !=
                        INVALID_FILE_ATTRIBUTES;
                if (hasRoot) {
                    SetCurrentDirectoryA(dir.c_str());
                    break;
                }
                const size_t up = dir.find_last_of("/\\");
                if (up == std::string::npos || up == 0) break;
                dir = dir.substr(0, up);
            }
        }
    }
#endif
    neon::core::ApplyLogCli(argc, argv);
    int smokeFrames = 0;
    bool disableShadows = false;
    bool disableBloom = false;
    bool disableMsaa = false;
    bool disableTonemap = false;
    bool benchMode = false;
    bool hotReload = false;
    bool twoD = false;
    bool twoDPlay = false;
    bool play = false;
    bool uiEditor = false;
    std::string projectDir;
    std::string backend = "gl";
    std::string screenshot;
    std::string previewPath;
    uint64_t screenshotFrame = 0;
    int exitAfterFrames = 0;
    std::string packVersion = "0.1.0";
    std::string packUpdateUrl;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hot") == 0) {
            hotReload = true;
        } else if (std::strcmp(argv[i], "--2d") == 0) {
            twoD = true;
        } else if (std::strcmp(argv[i], "--2d-play") == 0) {
            twoD = true;
            twoDPlay = true;
        } else if (std::strcmp(argv[i], "--play") == 0) {
            play = true;
        } else if (std::strcmp(argv[i], "--ui-editor") == 0) {
            uiEditor = true;
        } else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (std::strcmp(argv[i], "--preview") == 0 && i + 1 < argc) {
            previewPath = argv[++i];
        } else if (std::strcmp(argv[i], "--package") == 0 && i + 2 < argc) {
            const std::string projectDir = argv[++i];
            const std::string outDir = argv[++i];
            neon::editor::pack::PackConfig cfg;
            cfg.projectDir = projectDir;
            cfg.outDir = outDir;
            cfg.playerSource = "build/neon_game.exe";
            cfg.version = packVersion;
            cfg.updateUrl = packUpdateUrl;
            neon::editor::pack::PackageReport r = neon::editor::pack::PackProject(cfg);
            for (const std::string& e : r.errors) std::printf("PACK ERROR: %s\n", e.c_str());
            for (const std::string& w : r.warnings) std::printf("PACK WARN:  %s\n", w.c_str());
            if (r.ok) {
                std::printf("PACK OK: %s (%zu files, %zu bytes)\n", r.packPath.c_str(),
                            r.fileCount, r.bytesWritten);
                std::printf("PACK RUN: %s\n", r.runScriptPath.c_str());
                std::printf("PACK UPDATE-MANIFEST: %s\n", r.updatePath.c_str());
                std::printf("PACK INSTALL: %s\n", r.installPath.c_str());
                if (!r.playerPath.empty())
                    std::printf("PACK PLAYER: %s\n", r.playerPath.c_str());
            } else {
                std::printf("PACK FAILED: %zu errors, %zu warnings\n", r.errors.size(),
                            r.warnings.size());
            }
            return r.ok ? 0 : 1;
        } else if (std::strcmp(argv[i], "--pack-version") == 0 && i + 1 < argc) {
            packVersion = argv[++i];
        } else if (std::strcmp(argv[i], "--update-url") == 0 && i + 1 < argc) {
            packUpdateUrl = argv[++i];
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshot = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            exitAfterFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--disable-fbo") == 0 ||
                   std::strcmp(argv[i], "--no-shadows") == 0) {
            disableShadows = true;
        } else if (std::strcmp(argv[i], "--no-bloom") == 0) {
            disableBloom = true;
        } else if (std::strcmp(argv[i], "--no-msaa") == 0) {
            disableMsaa = true;
        } else if (std::strcmp(argv[i], "--no-tonemap") == 0) {
            disableTonemap = true;
        } else if (std::strcmp(argv[i], "--bench") == 0) {
            benchMode = true;
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("NeonEditor - NeonEngine scene editor\n"
                        "  --smoke-test <frames>  run N simulation frames then exit\n"
                        "  --hot                  enable hot reload (scripts/assets on mtime change)\n"
                        "  --2d                   start in the 2D canvas mode (NeonPvZ lawn editor)\n"
                         "  --2d-play              start 2D mode with the PvZ play running\n"
                         "  --play                 auto-start the open project's play (any mode)\n"
                         "  --ui-editor            open the UI editor panel at startup\n"
                        "  --project <dir>        open a data-driven project (game.json startScene)\n"
                        "  --backend <gl|vulkan>  graphics backend (default gl; vulkan is opt-in)\n"
                        "  --package <project> <out>  validate + pack a project into\n"
                        "                         <out>/game.pack (run.bat + neon_game.exe)\n"
                        "  --screenshot <path> <frame>  capture a PNG at frame N\n"
                        "  --disable-fbo          force-disable CSM shadow maps\n"
                        "  --no-shadows           alias for --disable-fbo\n"
                        "  --no-bloom             disable the HDR bloom post-process\n"
                        "  --no-msaa              force the single-sample HDR target (for diffing)\n"
                        "  --no-tonemap           composite with the legacy clamp instead of ACES (for diffing)\n"
                        "  --bench                log frame-time/draw stats every 60 frames and a summary at exit\n"
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
    // Headless/CI smoke runs must not be throttled by swap-vsync: a hidden or
    // unocused window can stall on vblank and the fixed 120-frame smoke would
    // take minutes instead of seconds.
    config.vsync = !(smokeFrames > 0);
    config.glMajor = 4;
    config.glMinor = 6;

    neon::editor::EditorApp app;
    if (smokeFrames > 0) {
        app.SetSmokeMode(true);
        app.SetSmokeTestFrames(smokeFrames);
    } else if (exitAfterFrames > 0) {
        // Non-smoke run that still exits after a fixed number of frames, so a
        // real project open (e.g. --project) can be captured with --screenshot.
        app.SetSmokeTestFrames(exitAfterFrames);
    }
    if (!screenshot.empty()) app.RequestScreenshot(screenshot, screenshotFrame);
    if (disableShadows) app.SetDisableShadows(true);
    if (disableBloom) app.SetBloomEnabled(false);
    if (disableMsaa) app.SetMsaaEnabled(false);
    if (disableTonemap) app.SetTonemapEnabled(false);
    if (benchMode) app.SetBenchMode(true);
    if (hotReload) app.SetHotReload(true);
    if (twoD) app.Set2DMode(true);
    if (twoDPlay) app.SetPvzPlayOnStart(true);
    if (play) app.SetPlayOnStart(true);
    if (uiEditor) app.SetUIEditorOnStart(true);
    if (!projectDir.empty()) app.SetProjectOnStart(projectDir, true);
    if (!previewPath.empty()) app.SetPreviewOnStart(previewPath);
    if (backend != "gl") app.SetBackendName(backend);
    int code = app.Run(config);
    return app.SmokeFailed() ? 1 : code;
}
