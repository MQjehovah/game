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

// Full-format loader: WAV via the built-in RIFF parser, everything the
// vendored miniaudio decoders support (ogg/mp3/flac/...) via ma_decoder.
// Output is 16-bit PCM mono at the file's native sample rate. Returns false
// when the file cannot be opened or the format is not available in this build.
bool LoadSoundFx(const std::string& path, SoundFx& out);

// ma_decoder decode path (defined in the miniaudio backend TU where the
// vendored implementation lives). Internal; use LoadSoundFx.
bool LoadSoundFxMiniAudio(const std::string& path, SoundFx& out);

// Memory-based loaders (pack/VFS path): audio assets inside a .pack have no
// file path, so callers read the bytes through the VFS and decode in memory.
// LoadWavFromMemory parses 16-bit PCM RIFF (stereo down-mixed, same as
// LoadWav). LoadSoundFxFromMemory tries the RIFF parser first, then the
// miniaudio decoders (ogg/mp3/flac/...). Returns false on format error.
bool LoadWavFromMemory(const uint8_t* data, size_t size, SoundFx& out);
bool LoadSoundFxFromMemory(const uint8_t* data, size_t size, SoundFx& out);

// ma_decoder memory decode path (miniaudio backend TU). Internal; use
// LoadSoundFxFromMemory.
bool LoadSoundFxMiniAudioFromMemory(const uint8_t* data, size_t size, SoundFx& out);

// Linear resample to the mixer's fixed 44.1 kHz device rate (no per-voice
// resampling in the backends, so assets recorded at other rates must be
// converted before playback). No-op at 44100.
SoundFx ResampleTo44100(SoundFx fx);

} // namespace neon::audio
