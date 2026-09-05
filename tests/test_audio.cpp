#include <cstdlib>

#include "neon/neon.hpp"
#include "neon/audio/mixer.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Pure mixer: math is tested headlessly (no audio device required)
// ---------------------------------------------------------------------------

TEST(AudioMixerSingleVoice) {
    const int16_t clip[] = {1000, -1000, 32767, 0};
    audio::MixVoice voices[1];
    voices[0].samples = clip;
    voices[0].sampleCount = 4;
    voices[0].volume = 2.0f;

    int16_t out[6] = {};
    size_t finished = audio::MixVoices(voices, 1, out, 6);

    CHECK_EQ(finished, 1u); // non-looping clip ran to its end
    CHECK_EQ(out[0], 2000);
    CHECK_EQ(out[1], -2000);
    CHECK_EQ(out[2], 32767); // hard-clamped to int16 range
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[4], 0); // silence once the clip ends
    CHECK_EQ(out[5], 0);
    CHECK_EQ(voices[0].pos, 4u);
}

TEST(AudioMixerLoops) {
    const int16_t clip[] = {100, 200, 300};
    audio::MixVoice v;
    v.samples = clip;
    v.sampleCount = 3;
    v.volume = 1.0f;
    v.loop = true;

    int16_t out[5] = {};
    size_t finished = audio::MixVoices(&v, 1, out, 5);

    CHECK_EQ(finished, 0u); // looping voice never finishes
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 200);
    CHECK_EQ(out[2], 300);
    CHECK_EQ(out[3], 100); // wrapped seamlessly
    CHECK_EQ(out[4], 200);
    CHECK_EQ(v.pos, 2u);
}

TEST(AudioMixerMultiVoice) {
    const int16_t a[] = {100, 100};
    const int16_t b[] = {50, 50, 50};
    audio::MixVoice v[2];
    v[0].samples = a;
    v[0].sampleCount = 2;
    v[0].volume = 1.0f;
    v[1].samples = b;
    v[1].sampleCount = 3;
    v[1].volume = 1.0f;

    int16_t out[3] = {};
    size_t finished = audio::MixVoices(v, 2, out, 3);

    CHECK_EQ(finished, 2u); // both clips ran to their end
    CHECK_EQ(out[0], 150);
    CHECK_EQ(out[1], 150);
    CHECK_EQ(out[2], 50); // only the longer voice contributes
    CHECK_EQ(v[0].pos, 2u);
    CHECK_EQ(v[1].pos, 3u);
}

TEST(AudioMixerPositionResumes) {
    // A partially-consumed voice resumes where it left off (backend buffers
    // advance `pos` across calls) and the output is additive.
    const int16_t clip[] = {10, 20, 30, 40};
    audio::MixVoice v;
    v.samples = clip;
    v.sampleCount = 4;
    v.volume = 1.0f;
    v.pos = 2; // already played the first two samples

    int16_t out1[2] = {};
    audio::MixVoices(&v, 1, out1, 2);
    CHECK_EQ(out1[0], 30);
    CHECK_EQ(out1[1], 40);
    CHECK_EQ(v.pos, 4u);

    int16_t out2[2] = {};
    size_t finished = audio::MixVoices(&v, 1, out2, 2);
    CHECK_EQ(finished, 1u); // fully consumed this pass
    CHECK_EQ(out2[0], 0);
    CHECK_EQ(out2[1], 0);
}

// ---------------------------------------------------------------------------
// Backend selection: device-dependent paths must not crash, with or without
// a real audio device, and the NEON_NO_MINIAUDIO hook forces the fallback.
// ---------------------------------------------------------------------------

namespace {
void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    std::string pair = std::string(name) + "=" + (value ? value : "");
    _putenv(pair.c_str());
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
} // namespace

TEST(AudioBackendSelectionDefault) {
    SetEnv("NEON_NO_MINIAUDIO", nullptr);
    auto backend = audio::CreatePlatformAudioBackend();
    CHECK(backend != nullptr);
    // Init may open a real device (miniaudio) or fall back to WinMM/Null on a
    // headless box; either way it must not crash.
    (void)backend->Init();
    backend->Play(audio::SoundFx{}, 1.0f); // empty clip must be a no-op
    backend->StopAll();
    backend->Shutdown();
}

TEST(AudioBackendSelectionFallback) {
    // Force miniaudio off: the selector must still return a working backend
    // (WinMM on Windows / Null elsewhere) without crashing, even though no
    // sound can come out of a device that was never opened.
    SetEnv("NEON_NO_MINIAUDIO", "1");
    auto backend = audio::CreatePlatformAudioBackend();
    CHECK(backend != nullptr);
    (void)backend->Init();
    backend->Play(audio::SoundFx{}, 1.0f);
    backend->StopAll();
    backend->Shutdown();
    SetEnv("NEON_NO_MINIAUDIO", nullptr);
}

TEST(AudioMixerContractWithSfx) {
    // The demo's SoundFx (16-bit mono PCM, 44.1 kHz) must round-trip through
    // the mixer unchanged at unity volume (the math used by the backend).
    audio::SoundFx fx;
    fx.samples = {100, -200, 300};
    audio::MixVoice v;
    v.samples = fx.samples.data();
    v.sampleCount = fx.samples.size();
    v.volume = 1.0f;

    int16_t out[3] = {};
    audio::MixVoices(&v, 1, out, 3);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], -200);
    CHECK_EQ(out[2], 300);
}

// P2-2: stereo mixer splits a voice by pan and clamps per channel.
TEST(AudioMixerStereoPan) {
    const int16_t clip[] = {1000, 1000};
    audio::MixVoice center;
    center.samples = clip;
    center.sampleCount = 2;
    center.volume = 1.0f;
    center.pan = 0.0f;
    int16_t out[4] = {};
    audio::MixVoicesStereo(&center, 1, out, 2);
    CHECK_EQ(out[0], 707);  // cos(pi/4)*1000
    CHECK_EQ(out[1], 707);  // sin(pi/4)*1000

    audio::MixVoice left;
    left.samples = clip;
    left.sampleCount = 2;
    left.volume = 1.0f;
    left.pan = -1.0f;
    int16_t outL[4] = {};
    audio::MixVoicesStereo(&left, 1, outL, 2);
    CHECK_EQ(outL[0], 1000);  // full left
    CHECK_EQ(outL[1], 0);

    audio::MixVoice right;
    right.samples = clip;
    right.sampleCount = 2;
    right.volume = 1.0f;
    right.pan = 1.0f;
    int16_t outR[4] = {};
    audio::MixVoicesStereo(&right, 1, outR, 2);
    CHECK_EQ(outR[0], 0);
    CHECK_EQ(outR[1], 1000);  // full right
}

// P2-2: WAV loader parses 16-bit PCM mono and down-mixes stereo.
TEST(AudioWavLoader) {
    const char* path = "smoke_tone.wav";
    // Minimal 44-byte mono PCM WAV: 2 samples (1000, -1000).
    std::vector<uint8_t> wav = {
        'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x44, 0xAC, 0, 0, 0x88, 0x58, 0x01, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0,
        0xE8, 0x03, 0x18, 0xFC};
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(wav.data()),
                  static_cast<std::streamsize>(wav.size()));
    }
    audio::SoundFx fx;
    CHECK(audio::LoadWav(path, fx));
    CHECK_EQ(fx.samples.size(), 2u);
    CHECK_EQ(fx.samples[0], 1000);
    CHECK_EQ(fx.samples[1], -1000);
    CHECK_EQ(fx.sampleRate, 44100u);
}

// Full-format loader: the bundled PvZ project ships real MP3 sound effects
// (dr_mp3 inside the vendored miniaudio). Decode one and sanity-check the
// result, then verify the WAV fast path still routes through LoadSoundFx.
TEST(AudioLoadSoundFxFormats) {
    // MP3: decode a real asset from the repo (read-only; ctest runs at root).
    audio::SoundFx mp3;
    std::string mp3Probe;
    if (test::ReadFileAll("projects/pvz/assets/audio/click.mp3", mp3Probe)) {
        CHECK(audio::LoadSoundFx("projects/pvz/assets/audio/click.mp3", mp3));
        CHECK(mp3.samples.size() > 1000u);   // a real click is ~16KB compressed
        CHECK(mp3.sampleRate >= 8000u);      // sane sample rate
    }

    // WAV: byte-identical result whether routed via LoadWav or LoadSoundFx.
    const char* path = "smoke_tone_fx.wav";
    std::vector<uint8_t> wav = {
        'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x44, 0xAC, 0, 0, 0x88, 0x58, 0x01, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0,
        0xE8, 0x03, 0x18, 0xFC};
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(wav.data()),
                  static_cast<std::streamsize>(wav.size()));
    }
    audio::SoundFx viaWav, viaAny;
    CHECK(audio::LoadWav(path, viaWav));
    CHECK(audio::LoadSoundFx(path, viaAny));
    CHECK_EQ(viaAny.samples.size(), viaWav.samples.size());
    CHECK_EQ(viaAny.samples[0], viaWav.samples[0]);

    // Garbage bytes fail closed on both paths.
    CHECK(test::WriteFileAll("smoke_bad_audio.bin", "this is not audio"));
    audio::SoundFx bad;
    CHECK(!audio::LoadSoundFx("smoke_bad_audio.bin", bad));
}

// Pack/VFS path: audio inside a .pack has no file path, so decoding happens
// from memory. Byte-parity with the file-path loader on the same RIFF bytes.
TEST(AudioWavFromMemory) {
    // Minimal 44-byte mono PCM WAV: 2 samples (1000, -1000).
    const std::vector<uint8_t> wav = {
        'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x44, 0xAC, 0, 0, 0x88, 0x58, 0x01, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0,
        0xE8, 0x03, 0x18, 0xFC};
    audio::SoundFx mem, file;
    CHECK(audio::LoadWavFromMemory(wav.data(), wav.size(), mem));
    CHECK_EQ(mem.samples.size(), 2u);
    CHECK_EQ(mem.samples[0], 1000);
    CHECK_EQ(mem.samples[1], -1000);
    CHECK_EQ(mem.sampleRate, 44100u);

    // LoadSoundFxFromMemory sniffs the same container and agrees.
    CHECK(audio::LoadSoundFxFromMemory(wav.data(), wav.size(), file));
    CHECK_EQ(file.samples.size(), mem.samples.size());
    CHECK_EQ(file.samples[0], mem.samples[0]);

    // Truncated + garbage buffers fail closed.
    audio::SoundFx bad;
    CHECK(!audio::LoadWavFromMemory(wav.data(), 10, bad));
    CHECK(!audio::LoadSoundFxFromMemory(
        reinterpret_cast<const uint8_t*>("not audio at all"), 16, bad));
}

// Real pack asset end-to-end: a shipped project WAV decodes from memory
// identically to the file path, and lands at the mixer's 44.1 kHz rate.
TEST(AudioPackAssetFromMemory) {
    std::string bytes;
    if (!test::ReadFileAll("projects/fps/assets/audio/hit.wav", bytes)) return;
    audio::SoundFx file, mem;
    CHECK(audio::LoadSoundFx("projects/fps/assets/audio/hit.wav", file));
    CHECK(audio::LoadSoundFxFromMemory(reinterpret_cast<const uint8_t*>(bytes.data()),
                                       bytes.size(), mem));
    CHECK_EQ(mem.samples.size(), file.samples.size());
    CHECK_EQ(mem.sampleRate, file.sampleRate);
    CHECK(mem.samples.size() > 100u);
}

// ResampleTo44100: 22.05 kHz content doubles in length; 44.1 kHz is a no-op.
TEST(AudioResampleTo44100) {
    audio::SoundFx low;
    low.sampleRate = 22050;
    for (int i = 0; i < 220; ++i) low.samples.push_back(static_cast<int16_t>(i));
    audio::SoundFx up = audio::ResampleTo44100(std::move(low));
    CHECK_EQ(up.sampleRate, 44100u);
    CHECK_EQ(up.samples.size(), 440u);  // exact 2x upsample

    // Already-44100 content passes through untouched (no realloc/copies).
    audio::SoundFx native;
    native.sampleRate = 44100;
    native.samples = {100, -100, 200};
    audio::SoundFx same = audio::ResampleTo44100(std::move(native));
    CHECK_EQ(same.samples.size(), 3u);
    CHECK_EQ(same.samples[0], 100);
}
