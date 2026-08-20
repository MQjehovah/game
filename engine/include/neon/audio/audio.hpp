#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neon::audio {

struct SoundFx {
    std::string name;
    std::vector<int16_t> samples; // 16-bit PCM
    uint32_t sampleRate = 44100;
    bool loop = false;
    float volume = 1.0f;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual void Play(const SoundFx& sound, float volume = 1.0f) = 0;
    virtual void StopAll() = 0;
    virtual bool Available() const = 0;
};

// Platform-specific: WinMM mixer on Windows, Null backend elsewhere for now.
std::unique_ptr<IAudioBackend> CreatePlatformAudioBackend();

} // namespace neon::audio
