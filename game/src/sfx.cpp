#include "sfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace neon::demo {
namespace sfx {
namespace {

constexpr double kRate = 44100.0;
constexpr double kPi2 = 6.28318530717958647692;

enum class Wave { Sine, Square, Saw, Triangle, Noise };

uint64_t g_noiseState = 0x9E3779B97F4A7C15ull;

double NoiseValue() {
    g_noiseState ^= g_noiseState << 13;
    g_noiseState ^= g_noiseState >> 7;
    g_noiseState ^= g_noiseState << 17;
    return static_cast<double>((g_noiseState >> 11) & 0x7FFFFF) / 4194303.0 * 2.0 - 1.0;
}

double Sample(Wave wave, double phase01, double angle) {
    switch (wave) {
        case Wave::Sine: return std::sin(angle);
        case Wave::Square: return std::sin(angle) >= 0.0 ? 0.65 : -0.65;
        case Wave::Saw: return phase01 * 2.0 - 1.0;
        case Wave::Triangle: return 2.0 * std::fabs(phase01 * 2.0 - 1.0) - 1.0;
        case Wave::Noise: return NoiseValue();
    }
    return 0.0;
}

void AddTone(std::vector<int16_t>& out, double freq0, double freq1, float duration,
             float volume, Wave wave, float attack = 0.005f, float release = 0.05f) {
    size_t count = static_cast<size_t>(duration * kRate);
    size_t start = out.size();
    out.resize(start + count, 0);
    for (size_t i = 0; i < count; ++i) {
        double t = static_cast<double>(i) / kRate;
        double frac = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, count - 1));
        double freq = freq0 + (freq1 - freq0) * frac;
        double phase = std::fmod(t * freq, 1.0);
        double angle = kPi2 * freq * t;
        double envelope = 1.0;
        double attackS = attack * duration;
        double releaseS = release * duration;
        if (i < attackS) envelope = static_cast<double>(i) / attackS;
        if (count - i < releaseS) envelope = static_cast<double>(count - i) / releaseS;
        out[start + i] += static_cast<int16_t>(Sample(wave, phase, angle) * volume * envelope * 32767.0);
    }
}

void AddToneAt(std::vector<int16_t>& buffer, size_t startSample, double freq0, double freq1,
               float duration, float volume, Wave wave, float attack = 0.01f, float release = 0.03f) {
    size_t count = static_cast<size_t>(duration * kRate);
    for (size_t i = 0; i < count; ++i) {
        size_t idx = startSample + i;
        if (idx >= buffer.size()) break;
        double t = static_cast<double>(i) / kRate;
        double frac = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, count - 1));
        double freq = freq0 + (freq1 - freq0) * frac;
        double phase = std::fmod(t * freq, 1.0);
        double angle = kPi2 * freq * t;
        double envelope = 1.0;
        if (i < count * attack) envelope = static_cast<double>(i) / (count * attack);
        if (count - i < count * release) envelope = static_cast<double>(count - i) / (count * release);
        buffer[idx] += static_cast<int16_t>(Sample(wave, phase, angle) * volume * envelope * 32767.0);
    }
}

void Clamp(std::vector<int16_t>& samples) {
    for (int16_t& s : samples) s = static_cast<int16_t>(std::max(-32768, std::min(32767, static_cast<int>(s))));
}

} // namespace

audio::SoundFx MakeSwing() {
    audio::SoundFx fx;
    fx.name = "swing";
    AddTone(fx.samples, 300, 90, 0.16f, 0.22f, Wave::Square, 0.02f, 0.1f);
    AddTone(fx.samples, 1200, 200, 0.08f, 0.10f, Wave::Noise, 0.01f, 0.06f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeHit() {
    audio::SoundFx fx;
    fx.name = "hit";
    AddTone(fx.samples, 220, 110, 0.09f, 0.3f, Wave::Square, 0.005f, 0.04f);
    AddTone(fx.samples, 900, 300, 0.06f, 0.15f, Wave::Noise, 0.005f, 0.03f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeExplosion() {
    audio::SoundFx fx;
    fx.name = "explosion";
    AddTone(fx.samples, 160, 40, 0.55f, 0.5f, Wave::Saw, 0.01f, 0.3f);
    AddTone(fx.samples, 2200, 200, 0.45f, 0.45f, Wave::Noise, 0.005f, 0.3f);
    AddTone(fx.samples, 60, 30, 0.6f, 0.4f, Wave::Sine, 0.01f, 0.35f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakePickup() {
    audio::SoundFx fx;
    fx.name = "pickup";
    AddTone(fx.samples, 660, 660, 0.08f, 0.25f, Wave::Sine, 0.005f, 0.05f);
    AddTone(fx.samples, 880, 880, 0.08f, 0.25f, Wave::Sine, 0.005f, 0.05f);
    AddTone(fx.samples, 1320, 1320, 0.12f, 0.25f, Wave::Sine, 0.005f, 0.08f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeHurt() {
    audio::SoundFx fx;
    fx.name = "hurt";
    AddTone(fx.samples, 320, 80, 0.3f, 0.4f, Wave::Saw, 0.01f, 0.12f);
    AddTone(fx.samples, 160, 60, 0.28f, 0.3f, Wave::Square, 0.01f, 0.12f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeDash() {
    audio::SoundFx fx;
    fx.name = "dash";
    AddTone(fx.samples, 200, 900, 0.16f, 0.3f, Wave::Square, 0.01f, 0.06f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeWave() {
    audio::SoundFx fx;
    fx.name = "wave";
    AddTone(fx.samples, 440, 440, 0.1f, 0.25f, Wave::Square, 0.005f, 0.05f);
    AddTone(fx.samples, 660, 660, 0.16f, 0.25f, Wave::Square, 0.005f, 0.08f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeGameOver() {
    audio::SoundFx fx;
    fx.name = "gameover";
    const double notes[4] = {523.25, 392.0, 329.63, 261.63};
    for (double n : notes) AddTone(fx.samples, n, n * 0.98f, 0.2f, 0.3f, Wave::Square, 0.01f, 0.1f);
    AddTone(fx.samples, 180, 40, 0.7f, 0.35f, Wave::Saw, 0.01f, 0.4f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeClick() {
    audio::SoundFx fx;
    fx.name = "click";
    AddTone(fx.samples, 900, 700, 0.05f, 0.2f, Wave::Square, 0.005f, 0.03f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeFireball() {
    audio::SoundFx fx;
    fx.name = "fireball";
    AddTone(fx.samples, 300, 1100, 0.28f, 0.3f, Wave::Saw, 0.02f, 0.12f);
    AddTone(fx.samples, 1400, 300, 0.18f, 0.18f, Wave::Noise, 0.01f, 0.1f);
    Clamp(fx.samples);
    return fx;
}

audio::SoundFx MakeMusic() {
    audio::SoundFx fx;
    fx.name = "music";
    fx.loop = true;
    const double bpm = 132.0;
    const double stepDur = 60.0 / bpm / 4.0;
    const int steps = 64;
    const int samplesPerStep = static_cast<int>(stepDur * kRate);
    fx.samples.assign(static_cast<size_t>(steps) * samplesPerStep, 0);

    const double lead[16] = {220.0, 0, 261.63, 0, 293.66, 0, 261.63, 329.63,
                             349.23, 0, 329.63, 293.66, 261.63, 0, 196.0, 220.0};
    const double bass[4] = {110.0, 130.81, 146.83, 98.0};

    for (int step = 0; step < steps; ++step) {
        size_t start = static_cast<size_t>(step) * samplesPerStep;
        double note = lead[step % 16];
        if (note > 0.0) {
            AddToneAt(fx.samples, start, note, note, static_cast<float>(stepDur * 0.9f),
                      0.07f, Wave::Square);
        }
        if (step % 4 == 0) {
            double root = bass[(step / 4) % 4];
            AddToneAt(fx.samples, start, root, root, static_cast<float>(stepDur * 3.2f),
                      0.14f, Wave::Triangle);
        }
        if (step % 2 == 1) {
            AddToneAt(fx.samples, start, 4000, 3000, static_cast<float>(stepDur * 0.4f),
                      0.04f, Wave::Noise);
        }
    }
    Clamp(fx.samples);
    return fx;
}

} // namespace sfx
} // namespace neon::demo
