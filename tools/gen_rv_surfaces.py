#!/usr/bin/env python3
"""Generate surface detail textures for the authored RV shell and furniture.

For every material we emit three maps:

  rv_tex_<name>.png       neutral grayscale base-colour detail (grain/weave/...)
  rv_tex_<name>_n.png     tangent-space normal map (Sobel of the height field)
  rv_tex_<name>_orm.png   R=ambient occlusion, G=roughness, B=metallic

glTF materials keep their baseColorFactor, so the base map only modulates the
existing palette. Two extra emissive maps light the head/tail lamps.

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


def wood(base, amp, dark):
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
    return img.filter(ImageFilter.GaussianBlur(0.6))


def brushed(base, amp):
    img = new(base)
    px = img.load()
    rows = [base + rng.randint(-9, 9) for _ in range(S)]
    for y in range(S):
        for x in range(S):
            v = rows[y] + int(5 * math.sin(x * 0.11))
            v += rng.randint(-amp, amp)
            px[x, y] = gray(v)
    return img.filter(ImageFilter.GaussianBlur(0.35))


def fabric(base, accent):
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
    return img.filter(ImageFilter.GaussianBlur(0.4))


def plastic(base):
    img = new(base)
    add_noise(img, 4)
    d = ImageDraw.Draw(img)
    for _ in range(120):
        x, y = rng.randrange(S), rng.randrange(S)
        d.point((x, y), fill=gray(base + 14))
    return img.filter(ImageFilter.GaussianBlur(0.3))


def glass(base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for _ in range(6):
        y0 = rng.randrange(-S, S)
        d.line([(0, y0), (S, y0 + rng.randint(20, 90))],
               fill=gray(base + 10), width=rng.randint(2, 6))
    add_noise(img, 5)
    return img.filter(ImageFilter.GaussianBlur(1.2))


def rubber(base):
    img = new(base)
    add_noise(img, 18)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 20):
        d.line([(0, y), (S, y + 8)], fill=gray(base - 22), width=2)
    return img


def roof(base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 32):
        d.line([(0, y), (S, y)], fill=gray(base - 26), width=2)
        d.line([(0, y + 2), (S, y + 2)], fill=gray(base + 14), width=1)
    add_noise(img, 7)
    return img.filter(ImageFilter.GaussianBlur(0.4))


def body(base):
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
    return img.filter(ImageFilter.GaussianBlur(0.5))


def solar(base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for y in range(0, S, 42):
        d.line([(0, y), (S, y)], fill=gray(base - 55), width=2)
    for x in range(0, S, 42):
        d.line([(x, 0), (x, S)], fill=gray(base - 55), width=2)
    add_noise(img, 6)
    return img.filter(ImageFilter.GaussianBlur(0.3))


def leaf(base):
    img = new(base)
    d = ImageDraw.Draw(img)
    for _ in range(90):
        x, y = rng.randrange(S), rng.randrange(S)
        r = rng.randint(2, 7)
        d.ellipse([x - r, y - r, x + r, y + r],
                  fill=gray(base + rng.randint(-40, 30)))
    add_noise(img, 8)
    return img.filter(ImageFilter.GaussianBlur(0.9))


def lamp():
    img = Image.new("RGB", (S, S), (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([10, 10, S - 11, S - 11], fill=(40, 36, 24))
    for k in range(6):
        y = 30 + k * 34
        d.rectangle([24, y, S - 25, y + 16], fill=(255, 240, 190))
    d.ellipse([70, 70, S - 70, S - 70], fill=(255, 252, 232))
    return img.filter(ImageFilter.GaussianBlur(2.2))


def tail():
    img = Image.new("RGB", (S, S), (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([12, 12, S - 13, S - 13], fill=(60, 4, 3))
    for k in range(5):
        y = 26 + k * 42
        d.rectangle([26, y, S - 27, y + 20], fill=(240, 30, 18))
    return img.filter(ImageFilter.GaussianBlur(2.0))


def normal_map(img, strength):
    """Sobel the height field into a tangent-space normal map."""
    src = img.convert("L")
    px = src.load()
    out = Image.new("RGB", (S, S))
    dst = out.load()
    for y in range(S):
        ym = (y - 1) % S
        yp = (y + 1) % S
        for x in range(S):
            xm = (x - 1) % S
            xp = (x + 1) % S
            dx = (px[xp, ym] + 2 * px[xp, y] + px[xp, yp]) - \
                 (px[xm, ym] + 2 * px[xm, y] + px[xm, yp])
            dy = (px[xm, yp] + 2 * px[x, yp] + px[xp, yp]) - \
                 (px[xm, ym] + 2 * px[x, ym] + px[xp, ym])
            nx = -dx / 1020.0 * strength
            ny = -dy / 1020.0 * strength
            nz = 1.0
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            dst[x, y] = (int((nx / ln * 0.5 + 0.5) * 255),
                         int((ny / ln * 0.5 + 0.5) * 255),
                         int((nz / ln * 0.5 + 0.5) * 255))
    return out


def orm_map(img, rough, metal):
    """R=ambient occlusion (grooves darker), G=roughness, B=metallic."""
    src = img.convert("L")
    px = src.load()
    out = Image.new("RGB", (S, S))
    dst = out.load()
    for y in range(S):
        for x in range(S):
            h = px[x, y]
            r = 255 - int(max(0, 128 - h) * 1.4)
            g = int(max(8, min(255, rough * 255 + (h - 128) * 0.22)))
            dst[x, y] = (r, g, int(metal * 255))
    return out


PARAMS = {
    # name: (roughness, metallic, normal-strength)
    "wood_light": (0.78, 0.0, 1.6),
    "wood_dark": (0.85, 0.0, 1.6),
    "metal": (0.32, 1.0, 0.7),
    "metal_dark": (0.46, 0.9, 0.7),
    "fabric": (0.95, 0.0, 2.0),
    "plastic": (0.55, 0.0, 0.5),
    "glass": (0.15, 0.5, 0.35),
    "rubber": (0.95, 0.0, 2.2),
    "roof": (0.60, 0.1, 1.2),
    "body": (0.55, 0.05, 1.3),
    "solar": (0.25, 0.4, 0.5),
    "leaf": (0.90, 0.0, 1.4),
}


def emit(name, img):
    rough, metal, nstrength = PARAMS[name]
    img.save(OUT / ("rv_tex_%s.png" % name), "PNG")
    normal_map(img, nstrength).save(OUT / ("rv_tex_%s_n.png" % name), "PNG")
    orm_map(img, rough, metal).save(OUT / ("rv_tex_%s_orm.png" % name), "PNG")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    emit("wood_light", wood(218, 9, 158))
    emit("wood_dark", wood(158, 8, 108))
    emit("metal", brushed(212, 11))
    emit("metal_dark", brushed(118, 9))
    emit("fabric", fabric(216, 194))
    emit("plastic", plastic(238))
    emit("glass", glass(205))
    emit("rubber", rubber(74))
    emit("roof", roof(222))
    emit("body", body(230))
    emit("solar", solar(206))
    emit("leaf", leaf(198))
    lamp().save(OUT / "rv_tex_lamp.png", "PNG")
    tail().save(OUT / "rv_tex_tail.png", "PNG")
    print("wrote", len(list(OUT.glob("rv_tex_*.png"))), "textures to", OUT)


if __name__ == "__main__":
    main()
