"""Generate a short tire-squeal WAV (16-bit mono 44.1 kHz).

A noisy 900-1400 Hz screech with a quick attack/decay, ~0.55 s.
Writes projects/rv/assets/audio/squeal.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.55
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/squeal.wav"


def main() -> None:
    random.seed(31)
    n = int(RATE * DUR)
    samples = [0.0] * n
    for i in range(n):
        k = i / n
        t = i / RATE
        env = (math.sin(math.pi * min(1.0, k * 1.4)) ** 0.7) * (1.0 - 0.25 * k)
        # Two close partials plus noise give a rubbery screech.
        tone = (0.55 * math.sin(2 * math.pi * 1150 * t) +
                0.30 * math.sin(2 * math.pi * 1620 * t) +
                0.15 * math.sin(2 * math.pi * 590 * t))
        noise = random.uniform(-1.0, 1.0) * 0.35
        samples[i] = (tone + noise) * env
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.40
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
