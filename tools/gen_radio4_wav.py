"""Generate an eerie numbers-station radio loop WAV (16-bit mono 44.1 kHz).

Static bed + low drone + slow Morse-like beeps, ~12 s loopable.
Writes projects/rv/assets/audio/radio4.wav. Pure stdlib.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 12.0
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/radio4.wav"


def main() -> None:
    random.seed(211)
    n = int(RATE * DUR)
    samples = [0.0] * n
    for i in range(n):
        t = i / RATE
        static = random.uniform(-1.0, 1.0) * 0.16
        drone = (0.05 * math.sin(2 * math.pi * 90 * t) +
                 0.03 * math.sin(2 * math.pi * 137 * t))
        samples[i] = static + drone
    # Slow beep pattern (Morse-ish), repeated every 3 s.
    pattern = [0.2, 0.35, 0.2, 0.35, 0.6]
    for start in (0.4, 3.4, 6.4, 9.4):
        t0 = start
        for dur in pattern:
            s0 = int(t0 * RATE)
            ln = int(dur * RATE)
            for k in range(ln):
                j = s0 + k
                if j >= n:
                    break
                env = math.sin(math.pi * k / ln) ** 1.3
                samples[j] += 0.16 * math.sin(2 * math.pi * 620 * k / RATE) * env
            t0 += dur + 0.12
    peak = max(1e-6, max(abs(v) for v in samples))
    frames = bytearray()
    for v in samples:
        s = (v / peak) * 0.28
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
