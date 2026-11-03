#!/usr/bin/env python3
"""Generate rv_ground_meadow.png: a stylised grass + dirt ground texture.

The reference art direction is a lush meadow with a dirt trail, so the world
ground switches from flat sand to a tiling grass map with dirt blotches,
mown-grass streaks and tiny wildflower dots. Elements are drawn with wrap-around
copies so the 512x512 tile is seamless.

Output: projects/rv/assets/models/rv/rv_ground_meadow.png
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_ground_meadow.png"

S = 512
rng = random.Random(20261104)


def wrapped(fn, x, y):
    """Call fn(dx, dy) for the 3x3 wrapped copies of (x, y)."""
    for ox in (-S, 0, S):
        for oy in (-S, 0, S):
            fn(x + ox, y + oy)


def main():
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        for x in range(S):
            v = 0.5 + 0.26 * math.sin(x * 0.012) * math.cos(y * 0.011)
            v += 0.10 * math.sin((x + y) * 0.038)
            n = rng.randint(-8, 8)
            px[x, y] = (max(0, min(255, int(54 + 34 * v) + n)),
                        max(0, min(255, int(98 + 48 * v) + n)),
                        max(0, min(255, int(36 + 20 * v) + n)))
    d = ImageDraw.Draw(img)

    # Dirt blotches (kept sparse so the world still reads as meadow).
    for _ in range(10):
        cx, cy = rng.randrange(S), rng.randrange(S)
        r = rng.randint(34, 96)
        def draw_dirt(x, y, cx=cx, cy=cy, r=r):
            d.ellipse([cx - r, cy - r * 0.7, cx + r, cy + r * 0.7],
                      fill=(104, 82, 56))
            d.ellipse([cx - r * 0.7, cy - r * 0.45, cx + r * 0.7, cy + r * 0.45],
                      fill=(120, 94, 63))
        wrapped(draw_dirt, cx, cy)

    # Grass streaks: light/dark strokes give the mown-meadow texture.
    for _ in range(4200):
        x, y = rng.randrange(S), rng.randrange(S)
        ln = rng.randint(4, 14)
        ang = rng.uniform(-0.5, 0.5)
        dx, dy = math.cos(ang) * ln, math.sin(ang) * ln
        light = rng.random() < 0.5
        col = (138, 182, 84) if light else (46, 84, 30)
        d.line([(x, y), (x + dx, y + dy)], fill=col,
               width=1 if rng.random() < 0.6 else 2)

    # Darker grass clumps break up the low-frequency colour bands.
    for _ in range(220):
        cx, cy = rng.randrange(S), rng.randrange(S)
        r = rng.randint(3, 10)
        def draw_clump(x, y, cx=cx, cy=cy, r=r):
            d.ellipse([cx - r, cy - r * 0.7, cx + r, cy + r * 0.7],
                      fill=(52, 92, 34))
        wrapped(draw_clump, cx, cy)

    # Wildflower dots in small clusters.
    flower_cols = [(240, 240, 244), (96, 150, 240), (246, 208, 74), (226, 98, 104)]
    for _ in range(26):
        cx, cy = rng.randrange(S), rng.randrange(S)
        col = flower_cols[rng.randrange(len(flower_cols))]
        for _ in range(rng.randint(3, 8)):
            x = cx + rng.randint(-14, 14)
            y = cy + rng.randint(-14, 14)
            d.ellipse([x - 1, y - 1, x + 1, y + 1], fill=col)
            d.point((x, y), fill=(250, 250, 200))

    img = img.filter(ImageFilter.GaussianBlur(0.4))
    img.save(OUT, "PNG")
    print("wrote", OUT, img.size)


if __name__ == "__main__":
    main()
