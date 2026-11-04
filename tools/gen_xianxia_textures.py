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
base[..., 0] = 56 + field * 30
base[..., 1] = 88 + field * 42
base[..., 2] = 44 + field * 22
base[..., 1] += (np.sin(41.0 * u) * 0.5 + 0.5) * 6
img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")

# Draw blades with wrap-around copies so the tile remains seamless.
draw = ImageDraw.Draw(img)
for _ in range(9000):
    x = rng.uniform(0, SIZE)
    y = rng.uniform(0, SIZE)
    length = rng.uniform(3.0, 10.0)
    lean = rng.uniform(-3.0, 3.0)
    shade = rng.randint(-30, 34)
    col = (max(30, min(130, 58 + shade)), max(55, min(170, 104 + shade)),
           max(25, min(110, 48 + shade)))
    for dx in (-SIZE, 0, SIZE):
        for dy in (-SIZE, 0, SIZE):
            draw.line((x + dx, y + dy, x + dx + lean, y + dy - length),
                      fill=col, width=1)

img = img.filter(ImageFilter.GaussianBlur(0.35))
img.save(OUT / "grass_proc.png")

# Tangent-space normal from the grass luminance (periodic so it tiles).
lum = np.asarray(img.convert("L"), dtype=np.float32) / 255.0
dx = (np.roll(lum, -1, axis=1) - np.roll(lum, 1, axis=1)) * 0.5
dy = (np.roll(lum, -1, axis=0) - np.roll(lum, 1, axis=0)) * 0.5
normal = np.stack([-dx * 3.2, -dy * 3.2, np.ones_like(lum)], axis=-1)
normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
normal_img = Image.fromarray(
    np.clip((normal * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8), "RGB")
normal_img.save(OUT / "grass_nor.png")

# Weathered stone for the path, gate and cliff columns.
stone_field = (
    0.50 * np.sin(2.0 * u + 0.7) * np.cos(3.0 * v + 1.4)
    + 0.24 * np.sin(7.0 * u - 6.0 * v + 0.2)
    + 0.14 * np.sin(19.0 * u + 13.0 * v)
    + 0.08 * np.cos(37.0 * u - 29.0 * v)
)
stone_field = (stone_field - stone_field.min()) / (stone_field.max() - stone_field.min())
g = 122 + stone_field * 38
stone_arr = np.stack([g * 0.98, g * 0.96, g * 0.88], axis=-1)
stone = Image.fromarray(np.clip(stone_arr, 0, 255).astype(np.uint8), "RGB")
sdraw = ImageDraw.Draw(stone)
for _ in range(420):
    x = rng.uniform(0, SIZE)
    y = rng.uniform(0, SIZE)
    length = rng.uniform(8.0, 46.0)
    ang = rng.uniform(-0.45, 0.45)
    col = rng.randint(62, 92)
    for dxs in (-SIZE, 0, SIZE):
        for dys in (-SIZE, 0, SIZE):
            sdraw.line((x + dxs, y + dys,
                        x + dxs + math.cos(ang) * length,
                        y + dys + math.sin(ang) * length),
                       fill=(col, col, col - 4), width=1)
for _ in range(7000):
    x = rng.randint(0, SIZE - 1)
    y = rng.randint(0, SIZE - 1)
    c = rng.randint(88, 138)
    stone.putpixel((x, y), (c, c - 1, c - 5))
stone = stone.filter(ImageFilter.GaussianBlur(0.25))
stone.save(OUT / "stone_proc.png")
slum = np.asarray(stone.convert("L"), dtype=np.float32) / 255.0
sdx = (np.roll(slum, -1, axis=1) - np.roll(slum, 1, axis=1)) * 0.5
sdy = (np.roll(slum, -1, axis=0) - np.roll(slum, 1, axis=0)) * 0.5
snormal = np.stack([-sdx * 4.0, -sdy * 4.0, np.ones_like(slum)], axis=-1)
snormal /= np.linalg.norm(snormal, axis=-1, keepdims=True)
Image.fromarray(
    np.clip((snormal * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8), "RGB"
).save(OUT / "stone_nor.png")

# Soft cloud billboard atlas: a handful of overlapping gaussian lobes with a
# noisy alpha edge, used for the cloud sea and drifting mist.
CW, CH = 512, 256
yy, xx = np.mgrid[0:CH, 0:CW]
nx = (xx + 0.5) / CW
ny = (yy + 0.5) / CH
alpha = np.zeros((CH, CW), dtype=np.float32)
for _ in range(11):
    cx = rng.uniform(0.22, 0.78)
    cy = rng.uniform(0.42, 0.65)
    sx = rng.uniform(0.10, 0.24)
    sy = rng.uniform(0.06, 0.15)
    alpha += np.exp(-(((nx - cx) / sx) ** 2 + ((ny - cy) / sy) ** 2) * 1.7)
alpha = np.clip(alpha * 0.42, 0.0, 1.0)
fade = np.clip((0.5 - np.abs(nx - 0.5)) * 4.0, 0.0, 1.0)
fade *= np.clip((0.5 - np.abs(ny - 0.5)) * 3.6, 0.0, 1.0)
alpha *= fade
edge_noise = 0.82 + 0.18 * np.sin(nx * 61.0 + np.sin(ny * 23.0) * 3.0)
alpha = np.clip(alpha * edge_noise, 0.0, 1.0)
cloud = np.zeros((CH, CW, 4), dtype=np.uint8)
cloud[..., 0] = 238
cloud[..., 1] = 244
cloud[..., 2] = 248
cloud[..., 3] = (alpha * 255.0).astype(np.uint8)
Image.fromarray(cloud, "RGBA").filter(ImageFilter.GaussianBlur(1.2)).save(
    OUT / "cloud_soft.png")

# Tileable cloud-sea albedo: layered periodic billows for the plane below the
# cliff. Keeps the sea from reading as a single flat white sheet.
CS = 512
cy, cx = np.mgrid[0:CS, 0:CS]
u = cx / CS * math.tau
w = cy / CS * math.tau
billow = (
    0.50
    + 0.20 * np.sin(u * 3.0 + np.sin(w * 2.0) * 1.4)
    + 0.13 * np.sin(w * 5.0 + np.sin(u * 3.0) * 1.7)
    + 0.08 * np.sin(u * 8.0 + w * 7.0)
    + 0.05 * np.sin(u * 17.0 - w * 13.0)
)
billow = np.clip(billow, 0.0, 1.0)
cloud_sea = np.zeros((CS, CS, 3), dtype=np.uint8)
cloud_sea[..., 0] = (196 + 42 * billow).astype(np.uint8)
cloud_sea[..., 1] = (207 + 38 * billow).astype(np.uint8)
cloud_sea[..., 2] = (211 + 34 * billow).astype(np.uint8)
Image.fromarray(cloud_sea, "RGB").save(OUT / "cloud_sea.jpg", quality=92)

print(f"wrote {OUT / 'grass_proc.png'}, grass_nor.png, stone_proc.png, "
      "cloud_soft.png and cloud_sea.jpg")

