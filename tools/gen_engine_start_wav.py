"""Generate an engine-start WAV (starter crank + brief rev, 16-bit mono 44.1k).

Writes projects/rv/assets/audio/engine_start.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 1.05
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/engine_start.wav"


def main() -> None:
    random.seed(5)
    n = int(RATE * DUR)
    samples = []
    for i in range(n):
        t = i / RATE
        # Starter motor: ~9 Hz crank pulses with a whiny overtone.
        if t < 0.62:
            pulse = 0.5 + 0.5 * math.sin(2 * math.pi * 9.0 * t)
            s = (0.30 * math.sin(2 * math.pi * 210 * t) +
                 0.18 * random.uniform(-1.0, 1.0)) * pulse
        else:
            # Engine catches: rev up with a low rumble.
            k = (t - 0.62) / (DUR - 0.62)
            freq = 70 + 120 * k
            env = math.sin(math.pi * min(1.0, k * 1.2))
            s = (0.55 * math.sin(2 * math.pi * freq * t) +
                 0.22 * math.sin(2 * math.pi * freq * 2.02 * t) +
                 0.10 * random.uniform(-1.0, 1.0)) * env
        samples.append(s)
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.65
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
