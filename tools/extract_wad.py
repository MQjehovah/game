#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract a League WAD3 (.wad.client) into PNG/DDS/etc.

- Parses the WAD3 TOC (count @0x10C, 32-byte entries @0x110).
- Decompresses zstd entries (type & 0xFF == 3).
- Recognises Riot TEX containers (BC1/BC3, 16-byte header) and DDS files and
  writes them out as PNG/DDS named by the WAD path hash.
"""
import os
import re
import struct
import sys

import numpy as np
from PIL import Image
import zstandard

MASK = (1 << 64) - 1
P1 = 11400714785074694791
P2 = 14029467366897019727
P3 = 1609587929392839161
P4 = 9650029242287828579
P5 = 2870177450012600261


def rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & MASK


def rnd(a, i):
    a = (a + i * P2) & MASK
    a = rotl(a, 31)
    return (a * P1) & MASK


def merge(a, v):
    a ^= rnd(0, v)
    return (a * P1 + P4) & MASK


def xxh64(d, seed=0):
    n = len(d)
    i = 0
    if n >= 32:
        v1 = (seed + P1 + P2) & MASK
        v2 = (seed + P2) & MASK
        v3 = seed & MASK
        v4 = (seed - P1) & MASK
        while i + 32 <= n:
            v1 = rnd(v1, int.from_bytes(d[i:i + 8], "little")); i += 8
            v2 = rnd(v2, int.from_bytes(d[i:i + 8], "little")); i += 8
            v3 = rnd(v3, int.from_bytes(d[i:i + 8], "little")); i += 8
            v4 = rnd(v4, int.from_bytes(d[i:i + 8], "little")); i += 8
        h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & MASK
        for v in (v1, v2, v3, v4):
            h = merge(h, v)
    else:
        h = (seed + P5) & MASK
    h = (h + n) & MASK
    while i + 8 <= n:
        h ^= rnd(0, int.from_bytes(d[i:i + 8], "little"))
        h = (rotl(h, 27) * P1 + P4) & MASK
        i += 8
    if i + 4 <= n:
        h ^= (int.from_bytes(d[i:i + 4], "little") * P1) & MASK
        h = (rotl(h, 23) * P2 + P3) & MASK
        i += 4
    while i < n:
        h ^= (d[i] * P5) & MASK
        h = (rotl(h, 11) * P1) & MASK
        i += 1
    h ^= h >> 33
    h = (h * P2) & MASK
    h ^= h >> 29
    h = (h * P3) & MASK
    h ^= h >> 32
    return h


def rgba565(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode_bc1(data, w, h):
    out = np.zeros((h, w, 4), np.uint8)
    bw = max(1, w // 4)
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            if o + 8 > len(data):
                continue
            c0 = int.from_bytes(data[o:o + 2], "little")
            c1 = int.from_bytes(data[o + 2:o + 4], "little")
            bits = int.from_bytes(data[o + 4:o + 8], "little")
            c = [rgba565(c0), rgba565(c1)]
            if c0 > c1:
                c.append(tuple((2 * c[0][i] + c[1][i]) // 3 for i in range(3)))
                c.append(tuple((c[0][i] + 2 * c[1][i]) // 3 for i in range(3)))
            else:
                c.append(tuple((c[0][i] + c[1][i]) // 2 for i in range(3)))
                c.append((0, 0, 0))
            for py in range(4):
                for px in range(4):
                    idx = (bits >> (2 * (py * 4 + px))) & 3
                    yy = by * 4 + py
                    xx = bx * 4 + px
                    if yy < h and xx < w:
                        out[yy, xx] = (*c[idx], 255)
    return out


def decode_bc3(data, w, h):
    out = np.zeros((h, w, 4), np.uint8)
    bw = max(1, w // 4)
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            if o + 16 > len(data):
                continue
            blk = data[o:o + 16]
            a0, a1 = blk[0], blk[1]
            abits = int.from_bytes(blk[2:8], "little")
            c0 = int.from_bytes(blk[8:10], "little")
            c1 = int.from_bytes(blk[10:12], "little")
            bits = int.from_bytes(blk[12:16], "little")
            c = [rgba565(c0), rgba565(c1)]
            if c0 > c1:
                c.append(tuple((2 * c[0][i] + c[1][i]) // 3 for i in range(3)))
                c.append(tuple((c[0][i] + 2 * c[1][i]) // 3 for i in range(3)))
            else:
                c.append(tuple((c[0][i] + c[1][i]) // 2 for i in range(3)))
                c.append((0, 0, 0))
            for py in range(4):
                for px in range(4):
                    idx = py * 4 + px
                    ci = (bits >> (2 * idx)) & 3
                    ai = (abits >> (3 * idx)) & 7
                    if a0 > a1:
                        a = a0 if ai == 0 else a1 if ai == 1 else ((8 - ai) * a0 + (ai - 1) * a1) // 7
                    else:
                        a = (a0 if ai == 0 else a1 if ai == 1
                             else (((6 - ai) * a0 + (ai - 1) * a1) // 5 if ai < 6
                                   else (0 if ai == 6 else 255)))
                    yy = by * 4 + py
                    xx = bx * 4 + px
                    if yy < h and xx < w:
                        out[yy, xx] = (*c[ci], a)
    return out


def bc1_size(w, h):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8


def bc3_size(w, h):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16


def mip_chain(base_w, base_h, size_fn):
    total = 0
    w, h = base_w, base_h
    while True:
        total += size_fn(w, h)
        if w == 1 and h == 1:
            break
        w = max(1, w // 2)
        h = max(1, h // 2)
    return total


def decode_tex(tex):
    """Riot TEX -> (png RGBA array, w, h, fmt)."""
    if tex[:4] != b"TEX\x00":
        raise ValueError("not TEX")
    w = struct.unpack_from("<H", tex, 4)[0]
    h = struct.unpack_from("<H", tex, 6)[0]
    body = tex[16:]
    # Pick the format whose full mip chain best matches the body size.
    cands = [("BC1", bc1_size, decode_bc1), ("BC3", bc3_size, decode_bc3)]
    best = None
    for name, size_fn, dec in cands:
        chain = mip_chain(w, h, size_fn)
        if len(body) >= size_fn(w, h):
            score = abs(chain - len(body))
            if best is None or score < best[0]:
                best = (score, name, dec)
    if best is None:
        raise ValueError("no format fits")
    _, name, dec = best
    img = dec(body, w, h)
    return img, w, h, name


def main():
    if len(sys.argv) < 2:
        print("usage: extract_wad.py <wad> [outdir]")
        return
    wad = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(wad), "..", "extract")
    os.makedirs(out, exist_ok=True)
    zstd = zstandard.ZstdDecompressor()
    with open(wad, "rb") as f:
        f.seek(0x10C)
        count = struct.unpack("<I", f.read(4))[0]
        f.seek(0x110)
        toc = f.read(count * 32)
        entries = []
        for i in range(count):
            h, off, cs, us, t = struct.unpack_from("<QIIII", toc, i * 32)
            entries.append((h, off, cs, us, t))
        kinds = {}
        for idx, (h, off, cs, us, t) in enumerate(entries):
            f.seek(off)
            blob = f.read(cs)
            if (t & 0xFF) == 3:
                try:
                    blob = zstd.decompress(blob, max_output_size=us + 1024)
                except Exception:
                    pass
            magic = blob[:4]
            if magic == b"TEX\x00":
                try:
                    img, w, hh, fmt = decode_tex(blob)
                    Image.fromarray(img, "RGBA").save(os.path.join(out, "%016x.%dx%d.%s.png" % (h, w, hh, fmt)))
                    kinds["TEX/" + fmt] = kinds.get("TEX/" + fmt, 0) + 1
                except Exception as e:
                    kinds["TEX-ERR"] = kinds.get("TEX-ERR", 0) + 1
            elif magic == b"DDS ":
                with open(os.path.join(out, "%016x.dds" % h), "wb") as o:
                    o.write(blob)
                kinds["DDS"] = kinds.get("DDS", 0) + 1
            elif magic[:4] == b"glTF":
                kinds["GLTF"] = kinds.get("GLTF", 0) + 1
            elif blob[:1] in (b"{", b"["):
                kinds["JSON"] = kinds.get("JSON", 0) + 1
                with open(os.path.join(out, "%016x.json" % h), "wb") as o:
                    o.write(blob)
            else:
                k = "other:" + magic.hex()
                kinds[k] = kinds.get(k, 0) + 1
                with open(os.path.join(out, "%016x.bin" % h), "wb") as o:
                    o.write(blob)
        print("entries:", count)
        for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
            print("  %-22s %d" % (k, v))


if __name__ == "__main__":
    main()
