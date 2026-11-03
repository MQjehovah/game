"""Generate an owl hoot WAV (16-bit mono 44.1 kHz).

Two soft low hoots with a little vibrato, ~1.4 s. Pure stdlib.
Writes projects/rv/assets/audio/owl.wav.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 1.4
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/owl.wav"


def hoot(samples, start, length, f0, f1):
    s0 = int(start * RATE)
    ln = int(length * RATE)
    phase = 0.0
    for k in range(ln):
        j = s0 + k
        if j >= len(samples):
            break
        t = k / ln
        freq = f0 + (f1 - f0) * t + 6 * math.sin(2 * math.pi * 6.0 * k / RATE)
        phase += 2 * math.pi * freq / RATE
        env = math.sin(math.pi * t) ** 1.1
        samples[j] += math.sin(phase) * env * 0.7


def main() -> None:
    random.seed(91)
    n = int(RATE * DUR)
    samples = [0.0] * n
    hoot(samples, 0.05, 0.45, 380, 330)
    hoot(samples, 0.72, 0.50, 360, 310)
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.42
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
