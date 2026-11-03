"""Generate a campfire crackle WAV (16-bit mono 44.1 kHz).

Random sparse pops over a soft low hiss, ~2.2 s. Writes
projects/rv/assets/audio/fire.wav. Pure stdlib and deterministic.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 2.2
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/fire.wav"


def main() -> None:
    random.seed(11)
    n = int(RATE * DUR)
    samples = [0.0] * n
    # Soft bed: low-passed noise with gentle flicker.
    low = 0.0
    for i in range(n):
        t = i / RATE
        low = low * 0.94 + random.uniform(-1.0, 1.0) * 0.06
        flicker = 0.75 + 0.25 * math.sin(2 * math.pi * 1.7 * t) * math.sin(2 * math.pi * 0.6 * t)
        samples[i] += low * 0.35 * flicker
    # Sparse crackles: short decaying noise bursts.
    for _ in range(34):
        start = random.randint(0, n - 1)
        length = random.randint(int(RATE * 0.008), int(RATE * 0.035))
        amp = random.uniform(0.25, 0.85)
        for k in range(length):
            j = start + k
            if j >= n:
                break
            env = math.exp(-k / (length * 0.35))
            samples[j] += random.uniform(-1.0, 1.0) * amp * env
    # One-pole low-pass smooths the crackles a little.
    prev = 0.0
    for i in range(n):
        prev = prev * 0.25 + samples[i] * 0.75
        samples[i] = prev
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.62
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
