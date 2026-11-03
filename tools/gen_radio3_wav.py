"""Generate a shortwave weather-band radio loop WAV (16-bit mono 44.1 kHz).

Soft static bed with a slow wobble and periodic faint Morse-like beeps, ~12 s.
Writes projects/rv/assets/audio/radio3.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 12.0
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/radio3.wav"


def main() -> None:
    random.seed(77)
    n = int(RATE * DUR)
    samples = [0.0] * n
    for i in range(n):
        t = i / RATE
        # Static bed with slow amplitude wobble.
        bed = (random.uniform(-1.0, 1.0) * 0.22 *
               (0.8 + 0.2 * math.sin(2 * math.pi * 0.17 * t)))
        # Faint low hum.
        hum = 0.05 * math.sin(2 * math.pi * 120 * t)
        samples[i] = bed + hum
    # Periodic short beeps every 2.7 s (weather-band marker).
    for start in (0.6, 3.3, 6.0, 8.7, 11.4):
        s0 = int(start * RATE)
        length = int(0.12 * RATE)
        for k in range(length):
            j = s0 + k
            if j >= n:
                break
            env = math.sin(math.pi * k / length) ** 1.2
            samples[j] += 0.18 * math.sin(2 * math.pi * 880 * k / RATE) * env
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
