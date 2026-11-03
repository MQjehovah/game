#!/usr/bin/env python3
"""Generate neutral detail textures for the authored RV shell and furniture.

Each map is a 256x256 grayscale pattern (wood grain, brushed metal, fabric
weave, rubber stipple, panel seams, solar grid, leaf mottling...). The glTF
materials keep their existing baseColorFactor, so the texture multiplies the
palette colour and adds surface detail without changing the art direction.

Output: projects/rv/assets/models/rv/rv_tex_*.png
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"

S = 256
rng = random.Random(20261103)


def gray(v):
    v = max(0, min(255, int(v)))
    return (v, v, v)


def new(base):
    return Image.new("RGB", (S, S), gray(base))


def add_noise(img, amp):
    px = img.load()
    for y in range(S):
        for x in range(S):
            v = px[x, y][0] + rng.randint(-amp, amp)
            px[x, y] = gray(v)


def wood(name, base, amp, dark):
    img = new(base)
    px = img.load()
    cols = [base + rng.randint(-10, 10) for _ in range(S)]
    for x in range(S):
        for y in range(S):
            v = cols[x] + int(7 * math.sin(y * 0.045 + x * 0.7))
            v += rng.randint(-amp, amp)
            px[x, y] = gray(v)
    d = ImageDraw.Draw(img)
    for _ in range(20):
        x0 = rng.randrange(S)
        w = rng.randint(1, 3)
        pts = []
        for y in range(0, S + 8, 8):
            pts.append((x0 + int(4 * math.sin(y * 0.05 + rng.random() * 6)), y))
        d.line(pts, fill=gray(dark), width=w)
    img = img.filter(ImageFilter.GaussianBlur(0.6))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def brushed(name, base, amp):
    img = new(base)
    px = img.load()
    rows = [base + rng.randint(-9, 9) for _ in range(S)]
    for y in range(S):
        for x in range(S):
            v = rows[y] + int(5 * math.sin(x * 0.11))
            v += rng.randint(-amp, amp)
            px[x, y] = gray(v)
    img = img.filter(ImageFilter.GaussianBlur(0.35))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def fabric(name, base, accent):
    img = new(base)
    d = ImageDraw.Draw(img)
    t = 4
    for y in range(0, S, t * 2):
        for x in range(0, S, t * 2):
            d.rectangle([x, y, x + t - 1, y + t - 1], fill=gray(base + 10))
            d.rectangle([x + t, y + t, x + t * 2 - 1, y + t * 2 - 1],
                        fill=gray(base + 10))
            d.rectangle([x + t, y, x + t * 2 - 1, y + t - 1], fill=gray(accent))
            d.rectangle([x, y + t, x + t - 1, y + t * 2 - 1], fill=gray(accent))
    add_noise(img, 5)
    img = img.filter(ImageFilter.GaussianBlur(0.4))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def plastic(name, base):
    img = new(base)
    add_noise(img, 4)
    d = ImageDraw.Draw(img)
    for _ in range(120):
        x, y = rng.randrange(S), rng.randrange(S)
        d.point((x, y), fill=gray(base + 14))
    img = img.filter(ImageFilter.GaussianBlur(0.3))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def glass(name, base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for i in range(6):
        y0 = rng.randrange(-S, S)
        d.line([(0, y0), (S, y0 + rng.randint(20, 90))],
               fill=gray(base + 10), width=rng.randint(2, 6))
    add_noise(img, 5)
    img = img.filter(ImageFilter.GaussianBlur(1.2))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def rubber(name, base):
    img = new(base)
    add_noise(img, 18)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 20):
        d.line([(0, y), (S, y + 8)], fill=gray(base - 22), width=2)
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def roof(name, base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 32):
        d.line([(0, y), (S, y)], fill=gray(base - 26), width=2)
        d.line([(0, y + 2), (S, y + 2)], fill=gray(base + 14), width=1)
    add_noise(img, 7)
    img = img.filter(ImageFilter.GaussianBlur(0.4))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def body(name, base):
    img = new(base)
    px = img.load()
    for y in range(S):
        phase = y % 16
        rib = 20 if phase < 3 else (-15 if 6 <= phase < 9 else 0)
        for x in range(S):
            v = base + rib + rng.randint(-4, 4)
            if x % 64 == 0:
                v -= 26
            px[x, y] = gray(v)
    img = img.filter(ImageFilter.GaussianBlur(0.5))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def solar(name, base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 42):
        d.line([(0, y), (S, y)], fill=gray(base - 55), width=2)
    for x in range(0, S, 42):
        d.line([(x, 0), (x, S)], fill=gray(base - 55), width=2)
    add_noise(img, 6)
    img = img.filter(ImageFilter.GaussianBlur(0.3))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def leaf(name, base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for _ in range(90):
        x, y = rng.randrange(S), rng.randrange(S)
        r = rng.randint(2, 7)
        d.ellipse([x - r, y - r, x + r, y + r], fill=gray(base + rng.randint(-40, 30)))
    add_noise(img, 8)
    img = img.filter(ImageFilter.GaussianBlur(0.9))
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    wood("wood_light", 218, 9, 158)
    wood("wood_dark", 158, 8, 108)
    brushed("metal", 212, 11)
    brushed("metal_dark", 118, 9)
    fabric("fabric", 216, 194)
    plastic("plastic", 238)
    glass("glass", 205)
    rubber("rubber", 74)
    roof("roof", 222)
    body("body", 230)
    solar("solar", 206)
    leaf("leaf", 198)
    print("wrote", len(list(OUT.glob("rv_tex_*.png"))), "textures to", OUT)


if __name__ == "__main__":
    main()
