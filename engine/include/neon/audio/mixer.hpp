#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neon::audio {

// A single active voice ready for mixing. `pos` is both input and output so
// the caller can persist playback position across buffers. The mixer does NOT
// own the sample memory; `samples` must outlive the call.
//
// `owned` gives a backend the option to take ownership of the sample data when
// the caller's SoundFx is a temporary (Play stores the pointer, so without a
// copy the voice would read a freed buffer on the audio thread). When `owned`
// is populated, `samples` must point into it.
struct MixVoice {
    const int16_t* samples = nullptr;
    size_t sampleCount = 0;
    std::vector<int16_t> owned; // optional: owned copy of the sample data
    float volume = 1.0f; // final per-voice gain (already includes fx.volume)
    bool loop = false;
    size_t pos = 0;
    // P2-2 3D spatial audio: pan in [-1,1] (left..right) and a per-bus gain
    // already folded into `volume` by the caller; kept here for stereo mixing.
    float pan = 0.0f;
    bool music = false;  // P2-2: music-bus voice (PlayMusic replaces these)
};

// Pure software mixer shared by the miniaudio backend (and unit-tested
// headlessly). Zero-fills `out` then sums every active voice into it.
//
// Semantics match the WinMM mixer this replaced:
//   * 16-bit signed mono frames at the backend's sample rate.
//   * per-sample addition with hard clamping to the int16 range.
//   * a non-looping voice stops when its samples run out (pos == sampleCount);
//     looping voices wrap seamlessly at the end of their clip.
//
// Returns the number of voices that hit the end of their samples this pass
// (non-looping voices the caller should drop).
inline size_t MixVoices(MixVoice* voices, size_t count, int16_t* out,
                        size_t frameCount) {
    std::memset(out, 0, frameCount * sizeof(int16_t));
    size_t finished = 0;
    for (size_t v = 0; v < count; ++v) {
        MixVoice& voice = voices[v];
        if (voice.samples == nullptr || voice.sampleCount == 0) {
            voice.pos = 0;
            continue;
        }
        size_t pos = voice.pos;
        for (size_t f = 0; f < frameCount; ++f) {
            if (pos >= voice.sampleCount) {
                if (!voice.loop) break;
                pos = 0;
            }
            int mixed = static_cast<int>(out[f]) +
                        static_cast<int>(static_cast<float>(voice.samples[pos]) *
                                         voice.volume);
            out[f] = static_cast<int16_t>(std::max(-32768, std::min(32767, mixed)));
            ++pos;
        }
        if (!voice.loop && pos >= voice.sampleCount) ++finished;
        voice.pos = pos;
    }
    return finished;
}

// P2-2 stereo mixer: `out` holds interleaved L/R int16 frames. Each voice is
// split by its pan (equal-power): leftGain = cos((pan+1)*pi/4),
// rightGain = sin((pan+1)*pi/4). Returns finished non-looping voices like the
// mono MixVoices.
inline size_t MixVoicesStereo(MixVoice* voices, size_t count, int16_t* out,
                              size_t frameCount) {
    std::memset(out, 0, frameCount * 2 * sizeof(int16_t));
    size_t finished = 0;
    for (size_t v = 0; v < count; ++v) {
        MixVoice& voice = voices[v];
        if (voice.samples == nullptr || voice.sampleCount == 0) {
            voice.pos = 0;
            continue;
        }
        const float t = (voice.pan + 1.0f) * 0.7853981633974483f;  // (pan+1)*pi/4
        const float lg = std::cos(t) * voice.volume;
        const float rg = std::sin(t) * voice.volume;
        size_t pos = voice.pos;
        for (size_t f = 0; f < frameCount; ++f) {
            if (pos >= voice.sampleCount) {
                if (!voice.loop) break;
                pos = 0;
            }
            const float s = static_cast<float>(voice.samples[pos]);
            int l = static_cast<int>(out[f * 2]) + static_cast<int>(s * lg);
            int r = static_cast<int>(out[f * 2 + 1]) + static_cast<int>(s * rg);
            out[f * 2] = static_cast<int16_t>(std::max(-32768, std::min(32767, l)));
            out[f * 2 + 1] = static_cast<int16_t>(std::max(-32768, std::min(32767, r)));
            ++pos;
        }
        if (!voice.loop && pos >= voice.sampleCount) ++finished;
        voice.pos = pos;
    }
    return finished;
}

} // namespace neon::audio
