"""Generate a looping snowy-wind ambience WAV (16-bit mono 44.1 kHz).

Low-passed noise with slow gusts, ~12 s loop. Pure stdlib.
Writes projects/rv/assets/audio/snowwind.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 12.0
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/snowwind.wav"


def main() -> None:
    random.seed(409)
    n = int(RATE * DUR)
    samples = [0.0] * n
    low = 0.0
    low2 = 0.0
    for i in range(n):
        t = i / RATE
        # Two-stage one-pole low-pass to make wind rather than white noise.
        white = random.uniform(-1.0, 1.0)
        low += (white - low) * 0.015
        low2 += (low - low2) * 0.03
        gust = 0.55 + 0.45 * (0.5 + 0.5 * math.sin(2 * math.pi * 0.07 * t))
        gust *= 0.75 + 0.25 * (0.5 + 0.5 * math.sin(2 * math.pi * 0.19 * t + 1.3))
        tone = 0.10 * math.sin(2 * math.pi * 140 * t) * math.sin(
            2 * math.pi * 0.05 * t)
        samples[i] = (low2 * 6.0 + tone) * gust
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.30
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
