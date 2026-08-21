// Single translation unit that owns the miniaudio implementation.
// Vendored from third_party/miniaudio/miniaudio.h (v0.11.25, MIT-0 /
// public domain). See docs/ARCHITECTURE.md section 3.6.
#define MINIAUDIO_IMPLEMENTATION
// Disable miniaudio's internal "silent device" fallback: when no real audio
// backend can be opened (headless CI, no ALSA/CoreAudio/WASAPI device) we
// want ma_device_init to FAIL so the engine's IAudioBackend selection can fall
// back to the WinMM mixer (Windows) or the engine's own Null backend.
#define MA_NO_NULL
#include "miniaudio.h"
