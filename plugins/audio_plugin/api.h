#pragma once

// C-compatible API of the G5-1 audio native plugin: a real audio backend
// (WinMM waveOut software mixer) shipped as a DLL/SO, discovered by the host
// via plugin::LoadNativeAudioBackend. Exports `NeonPlugin_GetInfo` (generic
// ABI, see neon/plugin/native.hpp) plus `NeonAudio_GetApi()` which returns the
// factory table below. The created backend is an opaque
// neon::audio::IAudioBackend*; the host drives it through the C++ interface
// (same-toolchain build — exactly how middleware ships binary SDKs), and
// create/destroy both run inside this module so new/delete stay on one CRT.

#include <stdint.h>

#if defined(_WIN32)
#if defined(NEON_PLUGIN_AUDIO_BUILD)
#define NEON_AUDIO_API __declspec(dllexport)
#else
#define NEON_AUDIO_API
#endif
#else
#define NEON_AUDIO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Backend factory table. create_backend() returns an opaque
// neon::audio::IAudioBackend* (or null on failure); destroy_backend() frees it
// and MUST run on the same module that allocated it.
typedef struct NeonAudioApi {
    void* (*create_backend)(void);
    void (*destroy_backend)(void* backend);
    const char* (*name)(void);
} NeonAudioApi;

// Returns the audio backend factory table (null on mismatch).
NEON_AUDIO_API const NeonAudioApi* NeonAudio_GetApi(void);

#ifdef __cplusplus
}
#endif
