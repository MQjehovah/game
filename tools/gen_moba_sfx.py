#!/usr/bin/env python3
"""Generate small procedural WAV sound effects for NeonMOBA (no external assets)."""
import math
import os
import struct
import wave

SR = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                   "projects", "moba", "assets", "audio")


def write_wav(name, samples):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = bytearray()
        for s in samples:
            v = max(-1.0, min(1.0, s))
            frames += struct.pack("<h", int(v * 32000))
        w.writeframes(bytes(frames))
    return path


def env(i, n, a=0.01, d=0.2):
    t = i / SR
    at = max(1, int(a * SR))
    if i < at:
        return i / at
    return math.exp(-(i - at) / (d * SR))


def tone(freq, dur, kind="sine", decay=0.2, vol=0.6):
    n = int(dur * SR)
    out = []
    for i in range(n):
        t = i / SR
        if kind == "sine":
            s = math.sin(2 * math.pi * freq * t)
        elif kind == "square":
            s = 1.0 if math.sin(2 * math.pi * freq * t) > 0 else -1.0
        elif kind == "saw":
            s = 2.0 * ((freq * t) % 1.0) - 1.0
        else:
            s = math.sin(2 * math.pi * freq * t)
        out.append(s * env(i, n, 0.005, decay) * vol)
    return out


def noise(dur, decay=0.08, vol=0.7, seed=1):
    n = int(dur * SR)
    out = []
    x = seed
    for i in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        r = (x / 0x3FFFFFFF) - 1.0
        out.append(r * env(i, n, 0.001, decay) * vol)
    return out


def mix(*tracks):
    n = max(len(t) for t in tracks)
    out = [0.0] * n
    for tr in tracks:
        for i, s in enumerate(tr):
            out[i] += s
    return [max(-1.0, min(1.0, v)) for v in out]


def seq(parts):
    out = []
    for p in parts:
        out.extend(p)
    return out


def main():
    write_wav("attack", mix(noise(0.12, 0.05, 0.5, 7), tone(220, 0.12, "saw", 0.06, 0.3)))
    write_wav("hit", mix(noise(0.10, 0.04, 0.6, 3), tone(160, 0.10, "square", 0.05, 0.25)))
    write_wav("cast", seq([tone(440, 0.10, "sine", 0.08, 0.4), tone(660, 0.16, "sine", 0.12, 0.4)]))
    write_wav("spell_hit", mix(noise(0.18, 0.08, 0.5, 11), tone(300, 0.18, "saw", 0.1, 0.3)))
    write_wav("tower", tone(120, 0.22, "square", 0.12, 0.4))
    write_wav("minion_die", mix(noise(0.16, 0.06, 0.4, 5), tone(180, 0.16, "saw", 0.08, 0.25)))
    write_wav("kill", seq([tone(523, 0.09, "sine", 0.08, 0.5), tone(784, 0.20, "sine", 0.16, 0.5)]))
    write_wav("buy", seq([tone(880, 0.06, "sine", 0.05, 0.4), tone(1175, 0.12, "sine", 0.1, 0.4)]))
    write_wav("levelup", seq([tone(523, 0.09, "sine", 0.07, 0.45), tone(659, 0.09, "sine", 0.07, 0.45),
                              tone(784, 0.20, "sine", 0.14, 0.45)]))
    write_wav("victory", seq([tone(523, 0.16, "sine", 0.14, 0.5), tone(659, 0.16, "sine", 0.14, 0.5),
                              tone(784, 0.16, "sine", 0.14, 0.5), tone(1047, 0.5, "sine", 0.3, 0.5)]))
    write_wav("defeat", seq([tone(392, 0.2, "sine", 0.16, 0.45), tone(330, 0.2, "sine", 0.16, 0.45),
                             tone(262, 0.6, "sine", 0.4, 0.45)]))
    print("wrote sfx to", OUT)


if __name__ == "__main__":
    main()
