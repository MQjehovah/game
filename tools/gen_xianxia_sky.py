#!/usr/bin/env python3
"""Generate a painted equirectangular twilight sky for the immortal path."""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects" / "xianxia" / "assets" / "sky" / "xianxia_sky.jpg"

W, H = 2048, 1024
u = (np.arange(W, dtype=np.float32) + 0.5) / W
v = (np.arange(H, dtype=np.float32) + 0.5) / H
uu, vv = np.meshgrid(u, v)
az = u * math.tau
az2 = uu * math.tau

# Sky gradient: cool slate zenith -> warm pale horizon.
top = np.array([54.0, 88.0, 122.0], dtype=np.float32)
horizon = np.array([218.0, 213.0, 199.0], dtype=np.float32)
t = np.clip(vv / 0.50, 0.0, 1.0)[..., None]
sky = top * (1.0 - t) ** 1.25 + horizon * t ** 1.65

# Broad cloud bands, periodic around the full 360 degrees.
cloud_noise = (
    0.46
    + 0.22 * np.sin(az2 * 3.0 + 0.4)
    + 0.13 * np.sin(az2 * 7.0 + 2.1)
    + 0.08 * np.sin(az2 * 13.0 + 5.0)
    + 0.05 * np.sin(az2 * 23.0 + 1.3)
)
cloud_band = np.exp(-((vv - 0.385) / 0.075) ** 2) * 0.75
cloud_band += np.exp(-((vv - 0.455) / 0.035) ** 2) * 0.55
cloud_mask = np.clip((cloud_noise - 0.30) * 2.6, 0.0, 1.0) * cloud_band
cloud_mask = np.clip(cloud_mask * 1.4, 0.0, 0.92)
sky = sky * (1.0 - cloud_mask[..., None]) + np.array(
    [232.0, 235.0, 232.0]) * cloud_mask[..., None]

# Soft sun glow behind thin cloud.
sun_u, sun_v = 0.74, 0.34
dist = np.sqrt(((uu - sun_u) * 1.0) ** 2 + ((vv - sun_v) * 1.15) ** 2)
glow = np.clip(1.0 - dist / 0.24, 0.0, 1.0) ** 2.4
sky = sky + np.array([88.0, 61.0, 30.0]) * glow[..., None]
sky = np.clip(sky, 0.0, 255.0)
img = Image.fromarray(sky.astype(np.uint8), "RGB")
draw = ImageDraw.Draw(img, "RGBA")


def ridge_height(base_y, amp, phase):
    h = (
        0.55 * np.sin(az * 2.0 + phase)
        + 0.28 * np.sin(az * 5.0 + phase * 1.7 + 0.7)
        + 0.13 * np.sin(az * 11.0 + phase * 2.3)
        + 0.07 * np.sin(az * 19.0 + phase * 0.4)
    )
    return base_y - amp * (0.55 + 0.45 * (h * 0.5 + 0.5))


layers = [
    (0.500, 0.190, (157.0, 174.0, 184.0), 0.32),
    (0.532, 0.155, (116.0, 139.0, 150.0), 0.22),
    (0.568, 0.125, (83.0, 108.0, 119.0), 0.12),
]
for li, (base_y, amp, col, haze) in enumerate(layers):
    ys = ridge_height(base_y * H, amp * H, li * 2.1 + 0.6)
    poly = [(0, H)] + [(int(x), int(round(float(ys[x])))) for x in range(W)] + [
        (W, H)]
    draw.polygon(poly, fill=(int(col[0]), int(col[1]), int(col[2]), 255))
    rim = [(int(x), int(round(float(ys[x])))) for x in range(W)]
    draw.line(rim, fill=(int(min(col[0] + 26, 255)), int(min(col[1] + 24, 255)),
                         int(min(col[2] + 22, 255)), 110), width=2)
    veil = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    vd = ImageDraw.Draw(veil)
    vd.rectangle((0, int(base_y * H), W, H),
                 fill=(int(col[0] + 32), int(col[1] + 34), int(col[2] + 34),
                       int(255 * haze)))
    img = Image.alpha_composite(img.convert("RGBA"), veil).convert("RGB")
    draw = ImageDraw.Draw(img, "RGBA")

# Sea of cloud at the bottom.
cloud = np.zeros((H, W, 3), dtype=np.float32)
cloud[..., 0] = 211.0
cloud[..., 1] = 221.0
cloud[..., 2] = 221.0
bank = (
    0.5
    + 0.2 * np.sin(az2 * 4.0 + 1.2)
    + 0.13 * np.sin(az2 * 9.0 + 3.4)
    + 0.07 * np.sin(az2 * 17.0 + 0.6)
)
bank = np.clip((vv - 0.55) * 3.2, 0.0, 1.0) * (0.5 + 0.5 * bank)
bank = np.clip(bank, 0.0, 0.82)
arr = np.asarray(img, dtype=np.float32)
arr = arr * (1.0 - bank[..., None]) + cloud * bank[..., None]
img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")
img = img.filter(ImageFilter.GaussianBlur(0.45))
OUT.parent.mkdir(parents=True, exist_ok=True)
img.save(OUT, quality=93)
print(f"wrote {OUT} ({W}x{H})")



