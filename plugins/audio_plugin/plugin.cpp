// G5-1 audio native plugin: a real audio backend shipped as a DLL/SO. The
// plugin compiles the engine's WinMM waveOut mixer (engine/src/audio/winmm/
// winmm_audio.cpp) plus the tiny log implementation it uses into itself, so the
// DLL is a self-contained audio middleware module — the same shape as the
// physics plugin. Exports the generic plugin ABI (NeonPlugin_GetInfo) and the
// module-specific backend factory (NeonAudio_GetApi). Windows-only: winmm is
// the Windows mixer API.
#define NEON_PLUGIN_AUDIO_BUILD
#include <cstdint>

#include "api.h"
#include "neon/audio/audio.hpp"
#include "neon/plugin/native.hpp"

#if defined(_WIN32)
#define NEON_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define NEON_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The real mixer, compiled into this DLL (see CMake sources). Returns a fresh
// backend the caller owns.
namespace neon::audio {
std::unique_ptr<IAudioBackend> CreateWinMMAudioBackend();
}

namespace {

// create/destroy for the generic plugin ABI: the instance is a fresh backend,
// so the plugin's own lifecycle mirrors the audio backend's (destroy calls
// back into the backend's virtual dtor, which shuts the mixer down).
void* CreateInstance(uint32_t apiVersion) {
    if (apiVersion != neon::plugin::kNativeApiVersion) return nullptr;
    auto backend = neon::audio::CreateWinMMAudioBackend();
    return backend ? backend.release() : nullptr;
}

void DestroyInstance(void* instance) {
    delete static_cast<neon::audio::IAudioBackend*>(instance);
}

void* CreateBackend() {
    auto backend = neon::audio::CreateWinMMAudioBackend();
    return backend ? backend.release() : nullptr;
}

void DestroyBackend(void* backend) {
    delete static_cast<neon::audio::IAudioBackend*>(backend);
}

const char* BackendName() { return "winmm"; }

const NeonAudioApi kApi = {
    /*.create_backend=*/CreateBackend,
    /*.destroy_backend=*/DestroyBackend,
    /*.name=*/BackendName,
};

} // namespace

NEON_PLUGIN_EXPORT bool NeonPlugin_GetInfo(neon::plugin::NativePluginInfo* out) {
    if (!out) return false;
    out->apiVersion = neon::plugin::kNativeApiVersion;
    out->size = sizeof(neon::plugin::NativePluginInfo);
    out->name = "audio_plugin";
    out->version = "1.0.0";
    out->create = &CreateInstance;
    out->destroy = &DestroyInstance;
    return true;
}

const NeonAudioApi* NeonAudio_GetApi() { return &kApi; }
