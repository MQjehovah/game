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
        "  --no-bloom             disable the HDR bloom post-process (HDR still on)\n"
        "  --no-tonemap           composite with the legacy clamp instead of ACES (for diffing)\n"
        "  --no-msaa              force the single-sample HDR target (for diffing)\n"
        "  --exposure <v>         composite exposure for ACES tonemapping (default 1.0)\n"
        "  --ibl <0..1>           IBL environment lighting intensity (default 1.0);\n"
        "                         0 keeps the legacy flat ambient (for diffing)\n"
        "  --backend <gl|vulkan>  graphics backend (default gl; vulkan is opt-in and\n"
        "                         requires a NEON_ENABLE_VULKAN build)\n"
        "  --bloom-compare <off.png> <on.png> <frame>  write the SAME frame twice\n"
        "                         (bloom off then on) from one HDR target for diffing\n"
        "  --tonemap-compare <clamped.png> <aces.png> <frame>  write the SAME frame twice\n"
        "                         (legacy clamp then ACES tonemap) for diffing\n"
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
    bool disableBloom = false;
    std::string screenshotPath;
    uint64_t screenshotFrame = 0;
    std::string compareOff, compareOn;
    uint64_t compareFrame = 0;
    std::string tonemapClamped, tonemapAces;
    uint64_t tonemapFrame = 0;
    float exposure = 1.0f;
    bool disableTonemap = false;
    bool disableMsaa = false;
    float iblStrength = 1.0f;
    std::string backend = "gl";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--smoke-test") == 0 && i + 1 < argc) {
            smokeFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc) {
            screenshotPath = argv[++i];
            screenshotFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--bloom-compare") == 0 && i + 3 < argc) {
            compareOff = argv[++i];
            compareOn = argv[++i];
            compareFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--tonemap-compare") == 0 && i + 3 < argc) {
            tonemapClamped = argv[++i];
            tonemapAces = argv[++i];
            tonemapFrame = static_cast<uint64_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            exposure = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--ibl") == 0 && i + 1 < argc) {
            iblStrength = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-tonemap") == 0) {
            disableTonemap = true;
        } else if (std::strcmp(argv[i], "--no-msaa") == 0) {
            disableMsaa = true;
        } else if (std::strcmp(argv[i], "--no-audio") == 0) {
            noAudio = true;
        } else if (std::strcmp(argv[i], "--disable-fbo") == 0 ||
                   std::strcmp(argv[i], "--no-shadows") == 0) {
            disableShadows = true;
        } else if (std::strcmp(argv[i], "--no-bloom") == 0) {
            disableBloom = true;
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

    neon::demo::NeonApp app;
    if (smokeFrames > 0) {
        app.SetSmokeMode(true);
        app.SetSmokeTestFrames(smokeFrames);
    }
    if (noAudio) app.SetNoAudio(true);
    if (!screenshotPath.empty()) app.RequestScreenshot(screenshotPath, screenshotFrame);
    if (!compareOff.empty()) app.RequestBloomCompare(compareOff, compareOn, compareFrame);
    if (!tonemapClamped.empty()) app.RequestTonemapCompare(tonemapClamped, tonemapAces, tonemapFrame);
    if (disableShadows) app.SetDisableShadows(true);
    if (disableBloom) app.SetBloomEnabled(false);
    if (disableTonemap) app.SetTonemapEnabled(false);
    if (disableMsaa) app.SetMsaaEnabled(false);
    if (exposure != 1.0f) app.SetExposure(exposure);
    if (iblStrength != 1.0f) app.SetIblStrength(iblStrength);
    if (backend != "gl") app.SetBackendName(backend);
    int result = app.Run(config);
    std::printf("NeonRealm exited with code %d\n", result);
    return result;
}
