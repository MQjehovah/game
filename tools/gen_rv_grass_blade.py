#!/usr/bin/env python3
"""Generate a small alpha-cut grass blade card for the procedural scatter pass."""
from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/textures/grass_blade.png"
S = 256
rng = random.Random(20261104)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

for i in range(14):
    base_x = S * (0.18 + 0.64 * rng.random())
    height = S * (0.48 + 0.45 * rng.random())
    width = S * (0.022 + 0.022 * rng.random())
    lean = (rng.random() - 0.5) * S * 0.22
    dark = rng.uniform(0.70, 1.0)
    green = (int(54 * dark), int(128 * dark), int(40 * dark), 255)
    light = (int(86 * dark), int(166 * dark), int(58 * dark), 255)

    pts = []
    steps = 9
    for j in range(steps + 1):
        t = j / steps
        x = base_x + lean * t * t
        y = S - 8 - height * t
        w = width * (1.0 - 0.92 * t)
        pts.append((x - w, y))
    for j in range(steps, -1, -1):
        t = j / steps
        x = base_x + lean * t * t
        y = S - 8 - height * t
        w = width * (1.0 - 0.92 * t)
        pts.append((x + w, y))
    d.polygon(pts, fill=light if i % 3 == 0 else green)

# Slight darker root band so the tuft sits into the terrain.
for i in range(8):
    x = S * (0.25 + 0.5 * rng.random())
    d.line([(x, S - 10), (x + rng.uniform(-6, 6), S - 20)],
           fill=(28, 74, 24, 255), width=3)

img = img.resize((64, 64), Image.LANCZOS)
OUT.parent.mkdir(parents=True, exist_ok=True)
img.save(OUT, optimize=True)
print("wrote", OUT)
