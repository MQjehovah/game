# Generates the missing NeonOps sfx WAVs (16-bit PCM mono, 44.1 kHz).
# Run from the repo root: python tools/gen_fps_sfx.py
import math
import struct
import wave

SR = 44100


def write_wav(path, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    norm = min(1.0, 0.92 / peak) if peak > 0.92 else 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s * norm)) * 32767))
            for s in samples))


def env_exp(i, n, k):
    return math.exp(-k * i / n)


def noise():
    return math.sin(random_seed() * 12.9898) * 43758.5453 % 2.0 - 1.0


_seed_state = 22222


def random_seed():
    global _seed_state
    _seed_state = (_seed_state * 1103515245 + 12345) & 0x7FFFFFFF
    return _seed_state / 0x7FFFFFFF


def gen_shoot():
    # Gunshot: filtered noise crack + low sine thump, tight decay.
    n = int(SR * 0.22)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / SR
        crack = (random_seed() * 2 - 1) * env_exp(i, n, 14) * 0.9
        lp += 0.42 * (crack - lp)  # one-pole lowpass tames the hiss
        thump = math.sin(2 * math.pi * (150 - 90 * t / 0.22) * t) * env_exp(i, n, 9) * 0.8
        out.append(lp * 1.1 + thump)
    return out


def gen_click():
    # UI tick: short 700 Hz square blip.
    n = int(SR * 0.05)
    return [(math.copysign(1, math.sin(2 * math.pi * 700 * i / SR)) *
             env_exp(i, n, 7) * 0.5) for i in range(n)]


def gen_wave():
    # Wave alarm: two square tones, 500 Hz then 700 Hz.
    seg = int(SR * 0.18)
    out = []
    for base in (500, 700):
        for i in range(seg):
            e = env_exp(i, seg, 5) * (0.15 if i < seg * 0.1 else 1.0)
            out.append(math.copysign(1, math.sin(2 * math.pi * base * i / SR)) * 0.42 * e)
    return out


def gen_win():
    # Victory: rising major arpeggio C5-E5-G5 (sine + soft 2nd harmonic).
    notes = [(523.25, 0.16), (659.25, 0.16), (783.99, 0.30)]
    out = []
    for freq, dur in notes:
        n = int(SR * dur)
        for i in range(n):
            e = env_exp(i, n, 4) * (0.2 if i < SR * 0.012 else 1.0)
            s = math.sin(2 * math.pi * freq * i / SR)
            out.append((s * 0.8 + math.sin(2 * math.pi * freq * 2 * i / SR) * 0.18) * 0.55 * e)
    return out


if __name__ == "__main__":
    base = "projects/fps/assets/audio/"
    write_wav(base + "shoot.wav", gen_shoot())
    write_wav(base + "click.wav", gen_click())
    write_wav(base + "wave.wav", gen_wave())
    write_wav(base + "win.wav", gen_win())
    print("generated shoot/click/wave/win.wav")
