#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Convert the DDS textures of the extracted WAD kitpieces to PNG."""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_wad import decode_bc1, decode_bc3  # noqa: E402

SRC = r"F:\assets\fantome\named"
DST = r"F:\assets\fantome\png"


def decode_bc2(data, w, h):
    out = np.zeros((h, w, 4), np.uint8)
    bw = max(1, w // 4)
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            if o + 16 > len(data):
                continue
            blk = data[o:o + 16]
            alpha = int.from_bytes(blk[0:8], "little")
            c0 = int.from_bytes(blk[8:10], "little")
            c1 = int.from_bytes(blk[10:12], "little")
            bits = int.from_bytes(blk[12:16], "little")
            c = [(((c0 >> 11) & 31) * 255 // 31, ((c0 >> 5) & 63) * 255 // 63, (c0 & 31) * 255 // 31),
                 (((c1 >> 11) & 31) * 255 // 31, ((c1 >> 5) & 63) * 255 // 63, (c1 & 31) * 255 // 31)]
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
                    a = ((alpha >> (4 * idx)) & 0xF) * 17
                    yy, xx = by * 4 + py, bx * 4 + px
                    if yy < h and xx < w:
                        out[yy, xx] = (*c[ci], a)
    return out


def parse_dds(path):
    b = open(path, "rb").read()
    if b[:4] != b"DDS ":
        return None
    h = struct.unpack_from("<I", b, 12)[0]
    w = struct.unpack_from("<I", b, 16)[0]
    mip = struct.unpack_from("<I", b, 28)[0]
    pfflags = struct.unpack_from("<I", b, 80)[0]
    fourcc = b[84:88]
    bitcount = struct.unpack_from("<I", b, 88)[0]
    rmask, gmask, bmask, amask = struct.unpack_from("<4I", b, 92)
    body = b[128:]

    def blocks(fn_size):
        return body[:fn_size]

    if fourcc in (b"DXT1", b"DXT2", b"DXT3", b"DXT4", b"DXT5"):
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        if fourcc == b"DXT1":
            img = decode_bc1(body, w, h)
            fmt = "BC1"
        elif fourcc in (b"DXT2", b"DXT3"):
            img = decode_bc2(body, w, h)
            fmt = "BC2"
        else:
            img = decode_bc3(body, w, h)
            fmt = "BC3"
        return img, fmt
    if bitcount == 32 and pfflags & 0x40:  # RGB
        arr = np.frombuffer(body[: w * h * 4], np.uint8).reshape(h, w, 4)
        # DDS 32-bit is usually BGRA; honour the masks when they match BGRA.
        if rmask == 0x00FF0000:
            arr = arr[..., [2, 1, 0, 3]]
        return arr, "RGBA"
    return None, "?"


def main():
    os.makedirs(DST, exist_ok=True)
    n = skip = 0
    for fn in os.listdir(SRC):
        if not fn.lower().endswith(".dds") or "kitpieces" not in fn.lower():
            continue
        img, fmt = parse_dds(os.path.join(SRC, fn))
        if img is None:
            skip += 1
            continue
        Image.fromarray(img, "RGBA").save(os.path.join(DST, fn[:-4] + ".png"))
        n += 1
    print("converted:", n, "skipped:", skip, "->", DST)


if __name__ == "__main__":
    main()
