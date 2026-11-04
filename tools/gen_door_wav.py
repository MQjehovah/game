"""Generate a short camper-van door creak WAV (16-bit mono 44.1 kHz).

A low friction squeak with a slight pitch wobble, ~0.5 s. Pure stdlib.
Writes projects/rv/assets/audio/door.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.5
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/door.wav"


def main() -> None:
    random.seed(131)
    n = int(RATE * DUR)
    samples = [0.0] * n
    for i in range(n):
        t = i / RATE
        k = t / DUR
        env = math.sin(math.pi * min(1.0, k * 1.1)) ** 0.8
        freq = 650 + 180 * math.sin(2 * math.pi * 3.2 * t) - 120 * k
        tone = math.sin(2 * math.pi * freq * t) * 0.55
        tone += math.sin(2 * math.pi * (freq * 1.7) * t) * 0.22
        noise = random.uniform(-1.0, 1.0) * 0.12
        samples[i] = (tone + noise) * env
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.32
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
