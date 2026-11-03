"""Generate a thunder rumble WAV (16-bit mono 44.1 kHz).

Low-frequency sine rumble plus filtered noise, with a sharp initial crack and
a long decaying tail. Writes projects/rv/assets/audio/thunder.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 2.2
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/thunder.wav"


def main() -> None:
    random.seed(3)
    n = int(RATE * DUR)
    samples = []
    low = 0.0
    for i in range(n):
        t = i / RATE
        # Long exponential tail with a short bright crack at the start.
        env = math.exp(-t / 0.55) + 0.35 * math.exp(-t / 0.08)
        noise = random.uniform(-1.0, 1.0)
        low = low * 0.985 + noise * 0.015
        rumble = 0.55 * math.sin(2 * math.pi * 42 * t) * math.exp(-t / 0.7)
        s = (low * 1.8 + rumble + 0.25 * noise * math.exp(-t / 0.06)) * env
        samples.append(s)
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.72
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
