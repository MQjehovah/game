#include "neon/audio/audio.hpp"
#include "neon/audio/mixer.hpp"
#include "neon/math/math.hpp"

#include "miniaudio.h"

#include <atomic>
#include <memory>
#include <vector>

#include "neon/core/log.hpp"

namespace neon::audio {
namespace {

constexpr ma_uint32 kSampleRate = 44100; // demo PCM (sfx.cpp) is 44.1 kHz
constexpr ma_uint32 kBufferFrames = 1024; // ~23 ms at 44.1 kHz
constexpr ma_uint32 kBufferPeriods = 4;
constexpr size_t kMaxVoices = 48;

// miniaudio playback backend: opens a device and software-mixes the queued
// SoundFx PCM (procedural, not decoded files) into the output callback.
// Same IAudioBackend contract as the WinMM mixer it replaces.
class MiniAudioBackend : public IAudioBackend {
public:
    ~MiniAudioBackend() override { Shutdown(); }

    bool Init() override {
        if (available_) return true; // idempotent: the backend selector pre-inits
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = 2;  // stereo for 3D panning (P2-2)
        config.sampleRate = kSampleRate;
        config.dataCallback = &MiniAudioBackend::DataCallback;
        config.pUserData = this;
        config.periodSizeInFrames = kBufferFrames;
        config.periods = kBufferPeriods;

        if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
            NEON_LOG_WARN("miniaudio: no playback device available");
            return false;
        }
        deviceInitialized_ = true;
        if (ma_device_start(&device_) != MA_SUCCESS) {
            NEON_LOG_WARN("miniaudio: failed to start playback device");
            ma_device_uninit(&device_);
            deviceInitialized_ = false;
            return false;
        }
        available_ = true;
        NEON_LOG_INFO("miniaudio: playback device \"%s\" @ %u Hz (stereo S16)",
                      device_.playback.name, device_.sampleRate);
        return true;
    }

    void Shutdown() override {
        if (!available_) return;
        available_ = false;
        {
            // ma_device_uninit waits for the audio thread; make sure no queued
            // voice is half-referenced when the last callback returns.
            ma_spinlock_lock(&lock_);
            voices_.clear();
            ma_spinlock_unlock(&lock_);
        }
        if (deviceInitialized_) {
            ma_device_uninit(&device_);
            deviceInitialized_ = false;
        }
        NEON_LOG_INFO("miniaudio: playback stopped");
    }

    void Play(const SoundFx& sound, float volume) override {
        PlayImpl(sound, volume, AudioBus::Sfx, false, {}, {}, {});
    }

    void PlayMusic(const SoundFx& sound, float volume) override {
        PlayImpl(sound, volume, AudioBus::Music, false, {}, {}, {});
    }

    void Play3D(const SoundFx& sound, const math::Vec3& pos,
                const math::Vec3& listenerPos, const math::Vec3& listenerForward,
                float volume) override {
        PlayImpl(sound, volume, AudioBus::Sfx, true, pos, listenerPos, listenerForward);
    }

    void SetBusVolume(AudioBus bus, float gain) override {
        ma_spinlock_lock(&lock_);
        busGains_[static_cast<size_t>(bus)] = gain;
        ma_spinlock_unlock(&lock_);
    }

    float BusVolume(AudioBus bus) const override {
        return busGains_[static_cast<size_t>(bus)];
    }

private:
    void PlayImpl(const SoundFx& sound, float volume, AudioBus bus, bool spatial,
                  const math::Vec3& pos, const math::Vec3& listenerPos,
                  const math::Vec3& listenerForward) {
        if (!available_ || sound.samples.empty()) return;
        float gain = volume * sound.volume;
        float pan = 0.0f;
        if (spatial) {
            const math::Vec3 delta = pos - listenerPos;
            const float dist = delta.Length();
            gain *= 1.0f / (1.0f + dist * dist);
            if (dist > 1e-4f) {
                const math::Vec3 dir = delta / dist;
                const math::Vec3 right = math::Cross(listenerForward, {0, 1, 0}).Normalized();
                pan = math::Clamp(math::Dot(dir, right), -1.0f, 1.0f);
            }
        }
        ma_spinlock_lock(&lock_);
        if (voices_.size() >= kMaxVoices) voices_.erase(voices_.begin());
        MixVoice voice;
        // The caller may pass a TEMPORARY SoundFx (e.g. Play(MakePvzSfx(...)))
        // that dies right after this call returns; the audio thread would then
        // mix a freed buffer. Own a copy so the voice's samples stay valid.
        voice.owned = sound.samples;
        voice.samples = voice.owned.data();
        voice.sampleCount = voice.owned.size();
        voice.volume = gain * busGains_[static_cast<size_t>(bus)] *
                       busGains_[static_cast<size_t>(AudioBus::Master)];
        voice.loop = sound.loop;
        voice.pan = pan;
        voice.music = bus == AudioBus::Music;
        if (voice.music) {
            for (auto it = voices_.begin(); it != voices_.end();) {
                if (it->music) it = voices_.erase(it);
                else ++it;
            }
        }
        // MOVE the voice into the vector. A copy here would re-copy `owned`
        // while `samples` still pointed at the LOCAL buffer; the local dies at
        // the end of this call and the audio thread would mix a freed buffer
        // (the exact WAV-triggered crash this ownership design exists to fix).
        voices_.push_back(std::move(voice));
        ma_spinlock_unlock(&lock_);
    }
    void StopAll() override {
        ma_spinlock_lock(&lock_);
        voices_.clear();
        ma_spinlock_unlock(&lock_);
    }

    bool Available() const override { return available_; }
    static void DataCallback(ma_device* device, void* output, const void*,
                             ma_uint32 frameCount) {
        static_cast<MiniAudioBackend*>(device->pUserData)->Mix(
            static_cast<int16_t*>(output), frameCount);
    }

    void Mix(int16_t* out, ma_uint32 frameCount) {
        ma_spinlock_lock(&lock_);
        MixVoicesStereo(voices_.data(), voices_.size(), out, frameCount);
        for (size_t i = 0; i < voices_.size();) {
            if (!voices_[i].loop && voices_[i].pos >= voices_[i].sampleCount) {
                // MOVE the tail voice into the finished slot: move transfers
                // the owned sample buffer (address unchanged), so the raw
                // `samples` pointer stays valid. A copy assignment here would
                // deep-copy `owned` while `samples` still pointed at the tail's
                // buffer, which pop_back then frees (dangling mix on the next
                // callback).
                if (i != voices_.size() - 1) {
                    voices_[i] = std::move(voices_.back());
                }
                voices_.pop_back();
            } else {
                ++i;
            }
        }
        ma_spinlock_unlock(&lock_);
    }

    ma_device device_{};
    volatile ma_spinlock lock_ = 0;
    std::vector<MixVoice> voices_;
    float busGains_[3] = {1.0f, 1.0f, 1.0f};
    std::atomic<bool> available_{false};
    bool deviceInitialized_ = false;
};

} // namespace

std::unique_ptr<IAudioBackend> CreateMiniAudioBackend() {
    return std::make_unique<MiniAudioBackend>();
}

// Full-format asset decoding (ogg/mp3/flac/wav/...) via ma_decoder, whose
// dr_mp3 + stb_vorbis backends are compiled into ma_impl.cpp. Decodes to the
// file's native sample rate, down-mixes stereo like LoadWav, and scales f32
// samples to the SoundFx 16-bit PCM representation. Returns false when the
// format is not recognized by this build (callers fall back to LoadWav or
// procedural synthesis).
bool LoadSoundFxMiniAudio(const std::string& path, SoundFx& out) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) return false;

    const ma_uint32 rate = decoder.outputSampleRate;
    const ma_uint32 channels = decoder.outputChannels;
    if (rate == 0 || channels < 1 || channels > 2) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    out.samples.clear();
    constexpr ma_uint64 kChunk = 4096;
    std::vector<float> frames(static_cast<size_t>(kChunk * channels));
    for (;;) {
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, &frames[0], kChunk, &read) != MA_SUCCESS)
            break;
        if (read == 0) break;
        const size_t start = out.samples.size();
        out.samples.resize(start + static_cast<size_t>(read));
        for (ma_uint64 i = 0; i < read; ++i) {
            const float* f = &frames[static_cast<size_t>(i) * channels];
            float mono = channels == 2 ? (f[0] + f[1]) * 0.5f : f[0];
            mono = mono < -1.0f ? -1.0f : (mono > 1.0f ? 1.0f : mono);
            out.samples[start + static_cast<size_t>(i)] =
                static_cast<int16_t>(mono * 32767.0f);
        }
    }
    ma_decoder_uninit(&decoder);
    out.sampleRate = rate;
    return !out.samples.empty();
}

} // namespace neon::audio
