"""Generate a night-cricket chirp WAV (16-bit mono 44.1 kHz).

A few short high-frequency pulses with a soft envelope, ~0.45 s.
Writes projects/rv/assets/audio/cricket.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.45
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/cricket.wav"


def main() -> None:
    random.seed(7)
    n = int(RATE * DUR)
    samples = [0.0] * n
    # Three quick chirp bursts around 4.2 kHz.
    for start in (0.02, 0.16, 0.30):
        s0 = int(start * RATE)
        length = int(0.055 * RATE)
        for k in range(length):
            j = s0 + k
            if j >= n:
                break
            t = k / RATE
            env = math.sin(math.pi * (k / length)) ** 1.5
            tone = (0.6 * math.sin(2 * math.pi * 4200 * t) +
                    0.25 * math.sin(2 * math.pi * 5600 * t))
            samples[j] += tone * env * 0.7
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
