"""Generate a short truck-horn WAV (dual tone, 16-bit mono 44.1 kHz).

The RV project resolves sfx cues from assets/audio/<name>.wav, so this writes
projects/rv/assets/audio/horn.wav. Pure stdlib; deterministic output.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 44100
DUR = 0.55
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/horn.wav"


def main() -> None:
    n = int(RATE * DUR)
    frames = bytearray()
    for i in range(n):
        t = i / RATE
        env = min(1.0, t / 0.03) * min(1.0, (DUR - t) / 0.12)
        # Two close partials + a fifth give a brassy truck-horn timbre.
        s = (0.55 * math.sin(2 * math.pi * 330 * t)
             + 0.35 * math.sin(2 * math.pi * 415 * t)
             + 0.15 * math.sin(2 * math.pi * 495 * t))
        s = math.tanh(1.8 * s) * env * 0.7
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
