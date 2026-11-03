"""Generate a second looping camp-radio pad (16-bit mono 44.1 kHz).

Slower, warmer chord progression (C - G - Am - F) with a soft tremolo.
Tail is cross-faded into the head for seamless looping.
Writes projects/rv/assets/audio/radio2.wav.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 44100
BEAT = 2.8
CHORDS = [
    (261.63, 329.63, 392.00),   # C
    (196.00, 246.94, 293.66),   # G
    (220.00, 261.63, 329.63),   # Am
    (174.61, 220.00, 261.63),   # F
]
CROSS = 0.6
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/radio2.wav"


def main() -> None:
    loop_n = int(RATE * BEAT * len(CHORDS))
    cross_n = int(RATE * CROSS)
    total = loop_n + cross_n
    raw = [0.0] * total
    for i in range(total):
        t = i / RATE
        idx = int(t / BEAT) % len(CHORDS)
        local = t - idx * BEAT
        env = min(1.0, local / 0.8) * min(1.0, (BEAT - local) / 0.9)
        env = max(0.0, env)
        trem = 0.90 + 0.10 * math.sin(2 * math.pi * 0.18 * t)
        s = 0.0
        for k, f in enumerate(CHORDS[idx]):
            s += (0.50 - 0.08 * k) * math.sin(2 * math.pi * f * t + k * 0.5)
        raw[i] = s * env * trem
    prev = 0.0
    for i in range(total):
        prev = prev * 0.45 + raw[i] * 0.55
        raw[i] = prev

    out = [0.0] * loop_n
    for i in range(cross_n):
        w = i / cross_n
        out[i] = raw[i] * w + raw[loop_n + i] * (1.0 - w)
    for i in range(cross_n, loop_n):
        out[i] = raw[i]

    peak = max(1e-6, max(abs(v) for v in out))
    frames = bytearray()
    for v in out:
        s = (v / peak) * 0.46
        frames += struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(OUT), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
