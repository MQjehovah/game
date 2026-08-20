#include "neon/audio/audio.hpp"

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

std::unique_ptr<IAudioBackend> CreatePlatformAudioBackend() {
#if defined(_WIN32)
    return CreateWinMMAudioBackend();
#else
    NEON_LOG_WARN("Audio: no backend for this platform yet, using null backend (see ROADMAP)");
    return std::make_unique<NullAudio>();
#endif
}

} // namespace neon::audio
