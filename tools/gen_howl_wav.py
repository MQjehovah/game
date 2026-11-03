"""Generate a distant wolf-howl WAV (16-bit mono 44.1 kHz).

A slow rising-falling tone with vibrato and a little breath noise, ~1.6 s.
Writes projects/rv/assets/audio/howl.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 1.6
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/howl.wav"


def main() -> None:
    random.seed(21)
    n = int(RATE * DUR)
    phase = 0.0
    samples = []
    for i in range(n):
        t = i / RATE
        k = t / DUR
        # Rise to a peak then fall; vibrato gives it a voice-like wobble.
        base = 240 + 260 * math.sin(math.pi * min(1.0, k * 1.15))
        freq = base + 14 * math.sin(2 * math.pi * 5.2 * t)
        phase += 2 * math.pi * freq / RATE
        env = math.sin(math.pi * k) ** 0.8
        breath = 0.08 * random.uniform(-1.0, 1.0) * env
        s = (0.55 * math.sin(phase) +
             0.18 * math.sin(phase * 2.01) + breath) * env
        samples.append(s)
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.50
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
