"""Generate a daytime birdsong chirp WAV (16-bit mono 44.1 kHz).

Two short rising/falling whistles, ~0.55 s. Writes
projects/rv/assets/audio/birds.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.55
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/birds.wav"


def main() -> None:
    random.seed(13)
    n = int(RATE * DUR)
    samples = [0.0] * n
    # Two whistles: (start, length, freq start, freq end)
    for start, length, f0, f1 in ((0.03, 0.16, 2300, 3100),
                                  (0.24, 0.20, 2900, 2100)):
        s0 = int(start * RATE)
        ln = int(length * RATE)
        phase = 0.0
        for k in range(ln):
            j = s0 + k
            if j >= n:
                break
            t = k / ln
            freq = f0 + (f1 - f0) * t
            phase += 2 * math.pi * freq / RATE
            env = math.sin(math.pi * t) ** 1.2
            samples[j] += math.sin(phase) * env * 0.7
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.38
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
