"""Generate a tire-blowout bang WAV (16-bit mono 44.1 kHz).

A sharp noise burst plus a low thump, ~0.45 s. Pure stdlib.
Writes projects/rv/assets/audio/bang.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.45
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/bang.wav"


def main() -> None:
    random.seed(53)
    n = int(RATE * DUR)
    samples = [0.0] * n
    for i in range(n):
        t = i / RATE
        k = t / DUR
        # Sharp attack, fast decay noise.
        env = math.exp(-8.0 * k)
        noise = random.uniform(-1.0, 1.0)
        thump = math.sin(2 * math.pi * 70 * t) * math.exp(-6.0 * k)
        crack = math.sin(2 * math.pi * 1400 * t) * math.exp(-22.0 * k)
        samples[i] = noise * env * 0.9 + thump * 0.6 + crack * 0.3
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.55
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
