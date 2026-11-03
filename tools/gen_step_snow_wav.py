"""Generate a snowy footstep crunch WAV (16-bit mono 44.1 kHz).

A short filtered noise burst with a soft thump, ~0.18 s. Pure stdlib.
Writes projects/rv/assets/audio/step_snow.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.18
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/step_snow.wav"


def main() -> None:
    random.seed(101)
    n = int(RATE * DUR)
    samples = [0.0] * n
    prev = 0.0
    for i in range(n):
        t = i / RATE
        k = t / DUR
        env = math.sin(math.pi * k) ** 0.8
        # Low-passed noise gives a fluffy crunch.
        nz = random.uniform(-1.0, 1.0)
        prev = prev * 0.72 + nz * 0.28
        crunch = prev * 0.9
        thump = math.sin(2 * math.pi * 130 * t) * math.exp(-10 * k) * 0.35
        samples[i] = (crunch + thump) * env
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.34
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
