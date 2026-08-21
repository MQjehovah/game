#include "neon/audio/audio.hpp"

#include <cstdlib>
#include <memory>

#include "neon/core/log.hpp"

#if defined(_WIN32)
namespace neon::audio {
std::unique_ptr<IAudioBackend> CreateWinMMAudioBackend();
}
#endif

namespace neon::audio {
namespace {

class NullAudio : public IAudioBackend {
public:
    bool Init() override { return true; }
    void Shutdown() override {}
    void Play(const SoundFx&, float) override {}
    void StopAll() override {}
    bool Available() const override { return false; }
};

} // namespace

std::unique_ptr<IAudioBackend> CreateMiniAudioBackend();

std::unique_ptr<IAudioBackend> CreatePlatformAudioBackend() {
    // miniaudio is the primary backend on every platform (WASAPI/DirectSound/
    // CoreAudio/ALSA). It reports unavailable when no device can be opened
    // (headless CI, missing audio server) or when the NEON_NO_MINIAUDIO test
    // hook is set; fall back to the WinMM mixer on Windows and the Null
    // backend elsewhere.
    if (std::getenv("NEON_NO_MINIAUDIO") != nullptr) {
        NEON_LOG_WARN("Audio: miniaudio disabled (NEON_NO_MINIAUDIO), falling back");
    } else {
        std::unique_ptr<IAudioBackend> backend = CreateMiniAudioBackend();
        if (backend->Init()) {
            NEON_LOG_INFO("Audio: miniaudio backend active");
            return backend;
        }
        backend->Shutdown();
        NEON_LOG_WARN("Audio: miniaudio unavailable, falling back");
    }
#if defined(_WIN32)
    return CreateWinMMAudioBackend();
#else
    NEON_LOG_WARN("Audio: no fallback backend for this platform, using null backend");
    return std::make_unique<NullAudio>();
#endif
}

} // namespace neon::audio
