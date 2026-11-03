"""Generate a short reverse-warning beep (16-bit mono 44.1 kHz).

Writes projects/rv/assets/audio/beep.wav.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.13
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/beep.wav"


def main() -> None:
    n = int(RATE * DUR)
    frames = bytearray()
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.008) * min(1.0, (DUR - t) / 0.03)
        s = 0.7 * math.sin(2 * math.pi * 880 * t) * env
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
