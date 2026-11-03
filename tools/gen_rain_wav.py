"""Generate a seamless rain ambience loop (16-bit mono 44.1 kHz).

The RV project resolves sfx cues from assets/audio/<name>.wav and the player
loops the music bus, so this writes projects/rv/assets/audio/rain.wav.
The tail is cross-faded into the head to avoid a click at the loop point.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
SECONDS = 6.0
CROSS = 0.35
OUT = Path(__file__).resolve().parent.parent / "projects/rv/assets/audio/rain.wav"


def main() -> None:
    random.seed(7)
    loop_n = int(RATE * SECONDS)
    cross_n = int(RATE * CROSS)
    total = loop_n + cross_n
    # White noise through a one-pole low-pass gives a soft rain hiss.
    raw = []
    prev = 0.0
    for _ in range(total):
        white = random.uniform(-1.0, 1.0)
        prev = prev * 0.72 + white * 0.28
        raw.append(prev)
    # Slow amplitude drift so the bed breathes instead of sounding flat.
    for i in range(total):
        t = i / RATE
        drift = 0.82 + 0.18 * math.sin(2 * math.pi * t / SECONDS)
        raw[i] *= drift

    out = [0.0] * loop_n
    for i in range(cross_n):
        w = i / cross_n
        out[i] = raw[i] * w + raw[loop_n + i] * (1.0 - w)
    for i in range(cross_n, loop_n):
        out[i] = raw[i]

    peak = max(1e-6, max(abs(v) for v in out))
    frames = bytearray()
    for v in out:
        s = (v / peak) * 0.55
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
