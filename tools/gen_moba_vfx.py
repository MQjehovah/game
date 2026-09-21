#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate NeonMOBA ground-VFX decal textures (soft ring / disc / chevron).

Pure stdlib (struct + zlib): no Pillow dependency. White RGB + alpha mask so the
runtime can tint the decal per team/skill.

Usage: python tools/gen_moba_vfx.py
Output: projects/moba/assets/sprites/decal_*.png
"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "projects", "moba", "assets", "sprites")


def write_png(path, w, h, pixels):
    """pixels: list of (r,g,b,a) bytes, length w*h."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        for x in range(w):
            raw.extend(pixels[y * w + x])
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def smoothstep(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def ring(size=128):
    px = []
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c) / c  # 0 center .. 1 edge
            # soft annulus around r=0.82
            band = 1.0 - smoothstep(0.0, 1.0, abs(d - 0.82) / 0.14)
            inner = 1.0 - smoothstep(0.0, 1.0, d / 0.62)  # faint inner glow
            a = max(band, inner * 0.18)
            a = int(max(0.0, min(1.0, a)) * 255)
            px.append((255, 255, 255, a))
    return px


def disc(size=128):
    px = []
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c) / c
            a = 1.0 - smoothstep(0.0, 1.0, d)
            a = int(max(0.0, min(1.0, a * a)) * 235)
            px.append((255, 255, 255, a))
    return px


def chevron(size=128):
    """Down-pointing arrow-ish chevron for ground direction cues."""
    px = []
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            nx, ny = (x - c) / c, (y - c) / c
            # two diagonal bands forming a V, thick
            a = 0.0
            if ny > -0.1:
                for sign in (1.0, -1.0):
                    # distance to line ny = -0.55 + sign*0.95*nx
                    num = abs(ny + 0.55 - sign * 0.95 * nx)
                    a = max(a, 1.0 - smoothstep(0.0, 0.22, num))
            if ny < -0.55 or ny > 0.75 or abs(nx) > 0.95:
                a = 0.0
            px.append((255, 255, 255, int(max(0.0, min(1.0, a)) * 255)))
    return px


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, pixels in (("decal_ring.png", ring()),
                         ("decal_disc.png", disc()),
                         ("decal_chevron.png", chevron())):
        path = os.path.join(OUT, name)
        write_png(path, 128, 128, pixels)
        print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
