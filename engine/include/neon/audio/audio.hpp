#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "neon/math/vec3.hpp"

namespace neon::audio {

struct SoundFx {
    std::string name;
    std::vector<int16_t> samples; // 16-bit PCM
    uint32_t sampleRate = 44100;
    bool loop = false;
    float volume = 1.0f;
};

// P2-2 audio buses: master + SFX + music gain groups (Godot-style buses).
enum class AudioBus : uint8_t { Master = 0, Sfx = 1, Music = 2 };

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual void Play(const SoundFx& sound, float volume = 1.0f) = 0;
    // P2-2 music bus: loops `sound` on the Music bus (stop with StopAll or by
    // setting the bus volume to 0). The first music voice replaces any prior
    // music voice.
    virtual void PlayMusic(const SoundFx& sound, float volume = 1.0f) = 0;
    // P2-2 3D spatial audio: distance attenuation (1/(1+d^2)) + horizontal
    // pan against the listener's forward vector (up = +Y).
    virtual void Play3D(const SoundFx& sound, const math::Vec3& pos,
                        const math::Vec3& listenerPos, const math::Vec3& listenerForward,
                        float volume = 1.0f) = 0;
    virtual void SetBusVolume(AudioBus bus, float gain) = 0;
    virtual float BusVolume(AudioBus bus) const = 0;
    virtual void StopAll() = 0;
    virtual bool Available() const = 0;
};

// Platform-specific: WinMM mixer on Windows, Null backend elsewhere for now.
std::unique_ptr<IAudioBackend> CreatePlatformAudioBackend();

// P2-2 streaming/asset path: loads a 16-bit PCM WAV (mono or stereo; stereo is
// down-mixed) into a SoundFx. Returns false on any format error.
bool LoadWav(const std::string& path, SoundFx& out);

} // namespace neon::audio
