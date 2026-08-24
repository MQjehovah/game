#include "neon/audio/audio.hpp"

#include <cstring>
#include <fstream>
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
    void PlayMusic(const SoundFx&, float) override {}
    void Play3D(const SoundFx&, const math::Vec3&, const math::Vec3&, const math::Vec3&,
                float) override {}
    void SetBusVolume(AudioBus, float) override {}
    float BusVolume(AudioBus) const override { return 1.0f; }
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

// P2-2: RIFF WAV loader for 16-bit PCM (mono or stereo; stereo down-mixed by
// averaging channels). Little-endian fields read byte-wise for portability.
bool LoadWav(const std::string& path, SoundFx& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    auto readU32 = [&]() -> uint32_t {
        uint8_t b[4];
        in.read(reinterpret_cast<char*>(b), 4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    };
    auto readU16 = [&]() -> uint16_t {
        uint8_t b[2];
        in.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
    };
    char tag[5] = {};
    in.read(tag, 4);
    if (std::strncmp(tag, "RIFF", 4) != 0) return false;
    (void)readU32();  // RIFF size
    in.read(tag, 4);
    if (std::strncmp(tag, "WAVE", 4) != 0) return false;

    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    bool gotFormat = false;
    while (in) {
        in.read(tag, 4);
        const uint32_t chunkSize = readU32();
        if (std::strncmp(tag, "fmt ", 4) == 0) {
            const uint16_t audioFormat = readU16();
            channels = readU16();
            sampleRate = readU32();
            if (audioFormat != 1) return false;  // PCM only
            if (channels < 1 || channels > 2) return false;
            gotFormat = true;
            // Skip the rest of the format chunk (audioFormat + channels +
            // sampleRate = 8 bytes already consumed).
            const uint32_t skip = chunkSize > 8 ? chunkSize - 8 : 0;
            in.seekg(static_cast<std::streamoff>(skip), std::ios::cur);
        } else if (std::strncmp(tag, "data", 4) == 0) {
            if (!gotFormat) return false;
            const size_t bytes = chunkSize;
            std::vector<uint8_t> pcm(bytes);
            in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
            out.samples.clear();
            out.samples.reserve(bytes / 2 / channels);
            for (size_t i = 0; i + 1 < bytes; i += 2 * channels) {
                int16_t l = static_cast<int16_t>(pcm[i] | (pcm[i + 1] << 8));
                if (channels == 2) {
                    const int16_t r =
                        static_cast<int16_t>(pcm[i + 2] | (pcm[i + 3] << 8));
                    out.samples.push_back(
                        static_cast<int16_t>((static_cast<int>(l) + r) / 2));
                } else {
                    out.samples.push_back(l);
                }
            }
            out.sampleRate = sampleRate;
            out.name = path;
            return !out.samples.empty();
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize + (chunkSize & 1)),
                     std::ios::cur);
        }
    }
    return false;
}

} // namespace neon::audio
