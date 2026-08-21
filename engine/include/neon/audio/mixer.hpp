#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace neon::audio {

// A single active voice ready for mixing. `pos` is both input and output so
// the caller can persist playback position across buffers. The mixer does NOT
// own the sample memory; `samples` must outlive the call.
struct MixVoice {
    const int16_t* samples = nullptr;
    size_t sampleCount = 0;
    float volume = 1.0f; // final per-voice gain (already includes fx.volume)
    bool loop = false;
    size_t pos = 0;
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

} // namespace neon::audio
