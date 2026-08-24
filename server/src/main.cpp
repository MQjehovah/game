#include <cstdio>
#include <cstdlib>
#include <string>

#include "neon/core/log.hpp"
#include "game_server.hpp"
#include "neon/core/crash.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

// Platform-native pause (the toolchain has no std::this_thread support).
void SleepMs(unsigned ms) {
#if defined(_WIN32)
    ::Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

void PrintUsage(const char* prog) {
    std::printf(
        "Usage: %s [--port N] --scene FILE [options]\n"
        "  --port N      UDP port to bind (default 26000; 0 = OS ephemeral)\n"
        "  --scene FILE  scene JSON file to load (required)\n"
        "  --scripts DIR base dir for scripts/behaviors/prefabs (default: scene's dir)\n"
        "  --assets DIR  asset base dir (unused headless; kept for parity)\n"
        "  --ticks N     run exactly N fixed 60Hz simulation steps then exit 0\n"
        "  --seed N      deterministic simulation seed (default: fixed constant)\n"
        "  --physics B   physics backend: 'jolt' (default when compiled) or\n"
        "                'custom' (deterministic sphere/AABB solver)\n"
        "  --loopback    bind 127.0.0.1 instead of 0.0.0.0\n"
        "  --help        show this help\n",
        prog);
}

// Parent directory of a path ("a/b/c.json" -> "a/b", "scene.json" -> ".").
std::string DirName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

} // namespace

// Headless authoritative server (T6.3). No window, no GL, no audio: it binds a
// UDP socket, runs the data-driven scene (scripts + BT + physics) at a fixed
// 60Hz step, and broadcasts MsgSnapshot to every connected client. With
// --ticks N it runs exactly N simulation steps and exits 0 (smoke test).
int main(int argc, char** argv) {
    neon::core::InstallCrashHandler();
    neon::server::GameServer::Config cfg;
    std::string scenePath;
    std::string scriptsDir;
    int ticksLimit = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::printf("error: missing value for %s\n", name);
            return nullptr;
        };
        if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "--port") {
            const char* v = value("--port");
            if (v) cfg.port = static_cast<uint16_t>(std::atoi(v));
        } else if (arg == "--scene") {
            const char* v = value("--scene");
            if (v) scenePath = v;
        } else if (arg == "--scripts") {
            const char* v = value("--scripts");
            if (v) scriptsDir = v;
        } else if (arg == "--assets") {
            const char* v = value("--assets");
            if (v) cfg.assetBaseDir = v;
        } else if (arg == "--ticks") {
            const char* v = value("--ticks");
            if (v) ticksLimit = std::atoi(v);
        } else if (arg == "--seed") {
            const char* v = value("--seed");
            if (v) cfg.rngSeed = std::strtoull(v, nullptr, 10);
        } else if (arg == "--physics") {
            const char* v = value("--physics");
            if (v) cfg.physicsBackend = v;
        } else if (arg == "--loopback") {
            cfg.loopback = true;
        } else {
            std::printf("error: unknown option '%s'\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (scenePath.empty()) {
        std::printf("error: --scene <file> is required\n");
        PrintUsage(argv[0]);
        return 2;
    }
    cfg.sceneJsonPath = scenePath;
    if (scriptsDir.empty()) scriptsDir = DirName(scenePath);
    cfg.scriptBaseDir = scriptsDir;

    neon::server::GameServer server;
    if (!server.Start(cfg)) {
        NEON_LOG_ERROR("server: failed to start; exiting");
        return 1;
    }
    NEON_LOG_INFO("server: port=%u scene='%s' scripts='%s' ticks=%d%s", server.Port(),
                  scenePath.c_str(), scriptsDir.c_str(), ticksLimit,
                  cfg.loopback ? " loopback" : "");

    // Virtual clock. GameServer::Step runs AT MOST ONE fixed tick per call and
    // the accumulator residual drains on later calls, so the loop counts tick
    // consumptions: each iteration advances the clock by >= one fixed step and
    // runs exactly one tick. `--ticks N` therefore stops at exactly N (no
    // overshoot from the 17ms advance vs. the 16.667ms fixed step).
    uint64_t now = 0;
    constexpr uint64_t kStepMs = 17; // >= one fixed step (1000/60 ~= 16.667ms)
    constexpr uint64_t kRealSleepMs = 16;
    while (ticksLimit <= 0 || server.CurrentTick() < static_cast<uint32_t>(ticksLimit)) {
        now += kStepMs;
        server.Step(now);
        if (ticksLimit > 0 && server.CurrentTick() >= static_cast<uint32_t>(ticksLimit)) break;
        SleepMs(kRealSleepMs);
    }

    NEON_LOG_CAT(neon::core::LogCategory::Net, neon::core::LogLevel::Info,
                 "server: reached tick %u (%u clients) and stopped", server.CurrentTick(),
                 server.ClientCount());
    server.Shutdown();
    return 0;
}
