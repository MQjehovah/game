#!/usr/bin/env python3
"""Generate the small procedural placeholder textures for the xianxia slice."""
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects" / "xianxia" / "assets" / "textures"
OUT.mkdir(parents=True, exist_ok=True)

SIZE = 512
rng = random.Random(31)

# Periodic, seamless low-frequency variation.
yy, xx = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
u = xx / SIZE * math.tau
v = yy / SIZE * math.tau
field = (
    0.45 * np.sin(3.0 * u + 1.1) * np.cos(4.0 * v + 0.4)
    + 0.22 * np.sin(9.0 * u + 7.0 * v + 2.0)
    + 0.12 * np.sin(17.0 * u - 5.0 * v)
    + 0.08 * np.cos(29.0 * u + 23.0 * v)
)
field = (field - field.min()) / (field.max() - field.min())

base = np.zeros((SIZE, SIZE, 3), dtype=np.float32)
base[..., 0] = 38 + field * 25
base[..., 1] = 92 + field * 52
base[..., 2] = 36 + field * 20
base[..., 1] += (np.sin(41.0 * u) * 0.5 + 0.5) * 7
img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")

# Draw blades with wrap-around copies so the tile remains seamless.
draw = ImageDraw.Draw(img)
for _ in range(9000):
    x = rng.uniform(0, SIZE)
    y = rng.uniform(0, SIZE)
    length = rng.uniform(3.0, 10.0)
    lean = rng.uniform(-3.0, 3.0)
    shade = rng.randint(-30, 34)
    col = (max(20, min(120, 42 + shade)), max(60, min(180, 110 + shade)),
           max(20, min(110, 40 + shade)))
    for dx in (-SIZE, 0, SIZE):
        for dy in (-SIZE, 0, SIZE):
            draw.line((x + dx, y + dy, x + dx + lean, y + dy - length),
                      fill=col, width=1)

img = img.filter(ImageFilter.GaussianBlur(0.35))
img.save(OUT / "grass_proc.png")
print(f"wrote {OUT / 'grass_proc.png'}")
