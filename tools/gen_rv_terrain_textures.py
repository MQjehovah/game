#!/usr/bin/env python3
"""Generate tileable RV road/terrain surface maps with numpy + PIL.

Outputs (1024x1024, tileable):
  rv_asphalt_albedo.png / _normal.png / _orm.png
  rv_gravel_albedo.png  / _normal.png / _orm.png
and a richer 1024 meadow albedo replacing rv_ground_meadow.png.
"""
from __future__ import annotations

import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
S = 1024
rng = np.random.default_rng(20261104)
rr = random.Random(20261104)


def fft_noise(beta: float, seed: int, shape=(S, S)) -> np.ndarray:
    r = np.random.default_rng(seed)
    h, w = shape
    white = r.standard_normal(shape)
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    filt = 1.0 / (1.0 + (fx * fx + fy * fy) ** (beta * 0.5))
    n = np.fft.ifft2(np.fft.fft2(white) * filt).real
    n -= n.min()
    n /= max(1e-9, n.max())
    return n


def smoothstep(a, b, x):
    t = np.clip((x - a) / max(1e-9, (b - a)), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def save_rgb(name: str, arr: np.ndarray):
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    Image.fromarray(arr, "RGB").save(OUT / name, optimize=True)


def save_rgba(name: str, arr: np.ndarray):
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    Image.fromarray(arr, "RGBA").save(OUT / name, optimize=True)


def normal_from_height(height: np.ndarray, strength=3.0) -> np.ndarray:
    # Periodic Sobel-ish gradient so the normal map tiles like the albedo.
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx /= length
    ny /= length
    nz /= length
    return np.dstack(((nx * 0.5 + 0.5) * 255,
                      (ny * 0.5 + 0.5) * 255,
                      (nz * 0.5 + 0.5) * 255))


def orm_from(ao: np.ndarray, rough: np.ndarray, metal=0.0) -> np.ndarray:
    return np.dstack((np.clip(ao, 0, 255), np.clip(rough, 0, 255),
                      np.full((S, S), metal * 255.0)))


def draw_wrapped_line(draw, points, fill, width=1):
    # Draw the same polyline shifted by -S/0/+S in both axes so cracks cross
    # tile boundaries without a visible seam.
    for ox in (-S, 0, S):
        for oy in (-S, 0, S):
            shifted = [(x + ox, y + oy) for x, y in points]
            draw.line(shifted, fill=fill, width=width)


def asphalt() -> None:
    fine = fft_noise(1.1, 11)
    medium = fft_noise(2.1, 12)
    broad = fft_noise(3.1, 13)
    # Asphalt base with aggregate grain and broad patch tone.
    rgb = np.zeros((S, S, 3), dtype=np.float64)
    gray = 38 + fine * 16 + medium * 8 - broad * 5
    rgb[:] = gray[..., None]
    rgb[:, :, 0] += medium * 2.0
    rgb[:, :, 2] += fine * 1.2

    # Worn tyre bands (u = 0.28 and 0.72), slightly polished and lighter.
    u = np.linspace(0, 1, S, endpoint=False)[None, :]
    band = (np.exp(-((u - 0.27) / 0.055) ** 2) +
            np.exp(-((u - 0.73) / 0.055) ** 2))
    rgb += band[..., None] * np.array([3.5, 3.5, 4.0])

    # Dusty edges where the asphalt meets gravel/soil.
    edge = np.maximum(np.exp(-(u / 0.045) ** 2),
                      np.exp(-((1 - u) / 0.045) ** 2))
    rgb += edge[..., None] * np.array([18.0, 13.0, 7.0])

    # Tar-filled patches: irregular low-frequency darker areas.
    patch = smoothstep(0.58, 0.74, broad)
    rgb = rgb * (1.0 - patch[..., None] * 0.28) + \
          np.array([28.0, 28.0, 30.0]) * (patch[..., None] * 0.28)

    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(img)

    # Cracks: a few wandering dark lines with a lighter lip.
    for _ in range(18):
        x = rr.randrange(S)
        y = rr.randrange(S)
        ang = rr.random() * math.tau
        pts = []
        for _step in range(rr.randint(20, 55)):
            x = (x + math.cos(ang) * rr.uniform(4, 13)) % S
            y = (y + math.sin(ang) * rr.uniform(4, 13)) % S
            ang += rr.uniform(-0.45, 0.45)
            pts.append((x, y))
        draw_wrapped_line(d, pts, (82, 78, 70), 3)
        draw_wrapped_line(d, pts, (18, 18, 20), 1)

    # Pothole-ish dark patches with a slightly raised rim.
    for _ in range(7):
        cx = rr.randrange(S)
        cy = rr.randrange(S)
        rx = rr.randint(8, 22)
        ry = rr.randint(6, 16)
        for ox in (-S, 0, S):
            for oy in (-S, 0, S):
                d.ellipse([cx + ox - rx, cy + oy - ry,
                           cx + ox + rx, cy + oy + ry],
                          fill=(26, 27, 28))
                d.arc([cx + ox - rx - 2, cy + oy - ry - 2,
                       cx + ox + rx + 2, cy + oy + ry + 2],
                      0, 360, fill=(72, 68, 60), width=2)

    img = img.filter(ImageFilter.GaussianBlur(0.35))
    albedo = np.asarray(img, dtype=np.float64)
    # Fine speckle on top.
    spec = rng.integers(-4, 5, (S, S, 1))
    albedo = np.clip(albedo + spec, 0, 255)
    save_rgb("rv_asphalt_albedo.png", albedo)

    # Height for normal: aggregate + cracks + patch edge, then normal map.
    height = fine * 0.55 + medium * 0.25
    # Reuse the albedo's local contrast as a cheap crack/height signal.
    lum = albedo.mean(axis=2) / 255.0
    height = height * 0.65 + (1.0 - lum) * 0.8
    save_rgb("rv_asphalt_normal.png", normal_from_height(height, 4.2))

    ao = 225 + fine * 25
    rough = 205 + medium * 22 + (1.0 - lum) * 30
    save_rgb("rv_asphalt_orm.png", orm_from(ao, rough, 0.0))


def gravel() -> None:
    fine = fft_noise(1.2, 31)
    medium = fft_noise(2.0, 32)
    broad = fft_noise(2.8, 33)
    base = np.zeros((S, S, 3), dtype=np.float64)
    base[:] = np.array([94.0, 83.0, 65.0])
    base += medium[..., None] * np.array([20.0, 18.0, 14.0])
    base += fine[..., None] * np.array([17.0, 16.0, 14.0])
    base -= broad[..., None] * np.array([8.0, 8.0, 7.0])

    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(img)
    # Wrapped pebble stamps.
    for _ in range(2400):
        cx = rr.randrange(S)
        cy = rr.randrange(S)
        rx = rr.randint(1, 5)
        ry = rr.randint(1, 4)
        c = rr.randint(-22, 20)
        col = (int(max(0, min(255, 108 + c))),
               int(max(0, min(255, 96 + c))),
               int(max(0, min(255, 76 + c))))
        for ox in (-S, 0, S):
            for oy in (-S, 0, S):
                d.ellipse([cx + ox - rx, cy + oy - ry,
                           cx + ox + rx, cy + oy + ry], fill=col)
    img = img.filter(ImageFilter.GaussianBlur(0.25))
    albedo = np.asarray(img, dtype=np.float64)
    save_rgb("rv_gravel_albedo.png", albedo)

    height = fine * 0.7 + medium * 0.3
    lum = albedo.mean(axis=2) / 255.0
    height = height * 0.65 + lum * 0.75
    save_rgb("rv_gravel_normal.png", normal_from_height(height, 5.5))
    ao = 210 + fine * 30
    rough = 225 + medium * 18
    save_rgb("rv_gravel_orm.png", orm_from(ao, rough, 0.0))


def meadow() -> None:
    fine = fft_noise(1.1, 51)
    medium = fft_noise(2.0, 52)
    broad = fft_noise(3.2, 53)
    patch = fft_noise(3.8, 54)

    grass = np.array([62.0, 110.0, 46.0])
    dry = np.array([128.0, 125.0, 70.0])
    dirt = np.array([112.0, 86.0, 57.0])
    rock = np.array([102.0, 98.0, 88.0])

    m_dry = smoothstep(0.48, 0.70, medium) * 0.55
    m_dirt = smoothstep(0.58, 0.76, broad) * 0.80
    m_rock = smoothstep(0.72, 0.82, patch) * 0.45

    rgb = np.zeros((S, S, 3), dtype=np.float64)
    rgb[:] = grass
    rgb = rgb * (1 - m_dry[..., None]) + dry * m_dry[..., None]
    rgb = rgb * (1 - m_dirt[..., None]) + dirt * m_dirt[..., None]
    rgb = rgb * (1 - m_rock[..., None]) + rock * m_rock[..., None]
    rgb += fine[..., None] * np.array([13.0, 15.0, 9.0])
    rgb -= broad[..., None] * np.array([7.0, 8.0, 4.0])

    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(img)
    # Small stones and a few flowers; wrap around tile edges.
    for _ in range(900):
        cx = rr.randrange(S)
        cy = rr.randrange(S)
        r = rr.randint(1, 3)
        c = rr.choice([(128, 124, 112), (150, 146, 132), (96, 92, 84)])
        for ox in (-S, 0, S):
            for oy in (-S, 0, S):
                d.ellipse([cx + ox - r, cy + oy - r,
                           cx + ox + r, cy + oy + r], fill=c)
    for _ in range(180):
        cx = rr.randrange(S)
        cy = rr.randrange(S)
        r = 1
        c = rr.choice([(230, 230, 190), (238, 210, 120), (210, 230, 245)])
        for ox in (-S, 0, S):
            for oy in (-S, 0, S):
                d.ellipse([cx + ox - r, cy + oy - r,
                           cx + ox + r, cy + oy + r], fill=c)
    img = img.filter(ImageFilter.GaussianBlur(0.2))
    save_rgb("rv_ground_meadow.png", np.asarray(img, dtype=np.float64))


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    asphalt()
    gravel()
    meadow()
    print("wrote asphalt/gravel/meadow texture sets to", OUT)
