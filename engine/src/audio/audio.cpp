#include "neon/audio/audio.hpp"

#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iterator>
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
// Buffer-based RIFF core shared by LoadWav (file path) and LoadWavFromMemory
// (pack/VFS). Little-endian fields read byte-wise for portability.
bool LoadWavFromMemory(const uint8_t* data, size_t size, SoundFx& out) {
    if (data == nullptr || size < 12) return false;
    auto readU32 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) |
               (static_cast<uint32_t>(data[off + 3]) << 24);
    };
    auto readU16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(static_cast<uint16_t>(data[off]) |
                                     static_cast<uint16_t>(static_cast<uint16_t>(data[off + 1])
                                                           << 8));
    };
    if (std::memcmp(data, "RIFF", 4) != 0) return false;
    if (std::memcmp(data + 8, "WAVE", 4) != 0) return false;

    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    bool gotFormat = false;
    size_t off = 12;
    while (off + 8 <= size) {
        const uint8_t* tag = data + off;
        const uint32_t chunkSize = readU32(off + 4);
        off += 8;
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            if (off + chunkSize > size) return false;
            const uint16_t audioFormat = readU16(off);
            channels = readU16(off + 2);
            sampleRate = readU32(off + 4);
            if (audioFormat != 1) return false;  // PCM only
            if (channels < 1 || channels > 2) return false;
            gotFormat = true;
        } else if (std::memcmp(tag, "data", 4) == 0) {
            if (!gotFormat) return false;
            const size_t bytes = std::min<size_t>(chunkSize, size - off);
            out.samples.clear();
            out.samples.reserve(bytes / 2 / channels);
            // Frame-strided walk keeps the stereo pair in bounds (the old
            // loop could read up to 2 bytes past the buffer on odd sizes).
            for (size_t i = 0; i + 2 * channels <= bytes; i += 2 * channels) {
                const int16_t l = static_cast<int16_t>(data[off + i] | (data[off + i + 1] << 8));
                if (channels == 2) {
                    const int16_t r = static_cast<int16_t>(data[off + i + 2] |
                                                           (data[off + i + 3] << 8));
                    out.samples.push_back(
                        static_cast<int16_t>((static_cast<int>(l) + r) / 2));
                } else {
                    out.samples.push_back(l);
                }
            }
            out.sampleRate = sampleRate;
            return !out.samples.empty();
        }
        // RIFF chunks are word-aligned.
        off += chunkSize + (chunkSize & 1);
    }
    return false;
}

bool LoadWav(const std::string& path, SoundFx& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (!LoadWavFromMemory(bytes.data(), bytes.size(), out)) return false;
    out.name = path;
    return true;
}

bool LoadSoundFx(const std::string& path, SoundFx& out) {
    // WAV has a dependency-free parser; everything else defers to the
    // vendored miniaudio decoders (ogg/mp3/flac). The extension only picks
    // the fast path — ma_decoder sniffs the container either way, so a
    // mislabeled extension still decodes.
    if (path.size() >= 4) {
        const std::string ext = path.substr(path.size() - 4);
        if (ext == ".wav" || ext == ".WAV") return LoadWav(path, out);
    }
    if (LoadSoundFxMiniAudio(path, out)) {
        out.name = path;
        return true;
    }
    return LoadWav(path, out);
}

bool LoadSoundFxFromMemory(const uint8_t* data, size_t size, SoundFx& out) {
    // RIFF first (dependency-free fast path), then the miniaudio decoders,
    // mirroring LoadSoundFx's file path fallbacks.
    if (LoadWavFromMemory(data, size, out)) return true;
    if (LoadSoundFxMiniAudioFromMemory(data, size, out)) return true;
    return false;
}

SoundFx ResampleTo44100(SoundFx fx) {
    if (fx.sampleRate == 44100 || fx.samples.empty()) return fx;
    const double ratio = static_cast<double>(fx.sampleRate) / 44100.0;
    const size_t outCount = static_cast<size_t>(static_cast<double>(fx.samples.size()) / ratio);
    SoundFx out;
    out.name = std::move(fx.name);
    out.sampleRate = 44100;
    out.loop = fx.loop;
    out.volume = fx.volume;
    out.samples.reserve(outCount);
    for (size_t i = 0; i < outCount; ++i) {
        const double srcPos = static_cast<double>(i) * ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        const size_t i1 = std::min(i0 + 1, fx.samples.size() - 1);
        const double frac = srcPos - static_cast<double>(i0);
        const double s = static_cast<double>(fx.samples[i0]) * (1.0 - frac) +
                         static_cast<double>(fx.samples[i1]) * frac;
        out.samples.push_back(static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, s))));
    }
    return out;
}

} // namespace neon::audio
