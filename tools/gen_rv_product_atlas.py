#!/usr/bin/env python3
"""Generate rv_products.png: a 4x4 atlas of product-packaging tiles.

The POI product walls map each box onto one tile, so merchandise reads as
packaged goods (brand band, label lines, barcode, accent stripe) instead of
flat colour blocks.

Output: projects/rv/assets/models/rv/rv_products.png (512x512).
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_products.png"

TILES = 4
TILE = 128
PALETTE = [
    (196, 84, 40), (48, 112, 60), (36, 74, 140), (196, 158, 34),
    (120, 48, 120), (36, 132, 132), (176, 60, 70), (70, 90, 110),
    (200, 120, 40), (60, 120, 96), (90, 60, 150), (150, 110, 50),
    (30, 100, 150), (170, 70, 40), (80, 110, 60), (130, 50, 80),
]


def draw_tile(d, ox, oy, idx):
    base = PALETTE[idx % len(PALETTE)]
    # Card body with a darker border.
    d.rectangle([ox, oy, ox + TILE - 1, oy + TILE - 1], fill=base)
    d.rectangle([ox, oy, ox + TILE - 1, oy + TILE - 1],
                outline=(20, 20, 24), width=3)
    # Brand band across the top.
    band = tuple(max(0, c - 40) for c in base)
    d.rectangle([ox + 4, oy + 4, ox + TILE - 5, oy + 30], fill=band)
    d.rectangle([ox + 10, oy + 12, ox + TILE - 10, oy + 22],
                fill=(238, 236, 228))
    # Product label block (white with fake text lines).
    d.rectangle([ox + 10, oy + 40, ox + TILE - 10, oy + 92],
                fill=(236, 234, 226))
    for k in range(4):
        y = oy + 48 + k * 10
        w = TILE - 28 - (k % 2) * 18
        d.rectangle([ox + 14, y, ox + 14 + w, y + 4], fill=(40, 42, 46))
    # Accent stripe.
    accent = tuple(min(255, c + 60) for c in base)
    d.rectangle([ox + 10, oy + 98, ox + TILE - 10, oy + 108], fill=accent)
    # Barcode at the bottom.
    bx = ox + 12
    for k in range(18):
        w = 2 + (k * 7 + idx) % 4
        if bx + w > ox + TILE - 14:
            break
        d.rectangle([bx, oy + 112, bx + w, oy + 122], fill=(24, 24, 26))
        bx += w + 2
    # Corner batch dot.
    d.ellipse([ox + TILE - 26, oy + 110, ox + TILE - 14, oy + 122],
              fill=accent, outline=(20, 20, 24))


def main():
    img = Image.new("RGB", (TILES * TILE, TILES * TILE), (210, 208, 200))
    d = ImageDraw.Draw(img)
    for idx in range(TILES * TILES):
        ox = (idx % TILES) * TILE
        oy = (idx // TILES) * TILE
        draw_tile(d, ox, oy, idx)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT, "PNG")
    print("wrote", OUT, img.size)


if __name__ == "__main__":
    main()
