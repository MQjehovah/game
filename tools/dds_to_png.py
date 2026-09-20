#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Decode the LoL DXT5 (.dds) map textures to RGBA PNGs, preserving alpha.

The previous conversion flattened alpha to 255, which broke foliage cut-outs
(bushes/trees rendered as solid quads).
"""
import glob
import os
import struct

import numpy as np
from PIL import Image

SRC = r"F:\assets\sr\textures"
DST = r"F:\assets\sr\textures\png"


def decode_dxt5(data, w, h):
    out = np.zeros((h, w, 4), np.uint8)
    bw = w // 4

    def rgb565(c):
        return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

    for by in range(h // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            blk = data[o:o + 16]
            a0, a1 = blk[0], blk[1]
            abits = int.from_bytes(blk[2:8], "little")
            c0 = int.from_bytes(blk[8:10], "little")
            c1 = int.from_bytes(blk[10:12], "little")
            cbits = int.from_bytes(blk[12:16], "little")
            cols = [rgb565(c0), rgb565(c1)]
            if c0 > c1:
                cols.append(tuple((2 * cols[0][i] + cols[1][i]) // 3 for i in range(3)))
                cols.append(tuple((cols[0][i] + 2 * cols[1][i]) // 3 for i in range(3)))
            else:
                cols.append(tuple((cols[0][i] + cols[1][i]) // 2 for i in range(3)))
                cols.append((0, 0, 0))
            for py in range(4):
                for px in range(4):
                    i = py * 4 + px
                    ci = (cbits >> (2 * i)) & 3
                    ai = (abits >> (3 * i)) & 7
                    if a0 > a1:
                        a = a0 if ai == 0 else a1 if ai == 1 else ((8 - ai) * a0 + (ai - 1) * a1) // 7
                    else:
                        a = (a0 if ai == 0 else a1 if ai == 1
                             else (((6 - ai) * a0 + (ai - 1) * a1) // 5 if ai < 6
                                   else (0 if ai == 6 else 255)))
                    out[by * 4 + py, bx * 4 + px] = (*cols[ci], a)
    return out


def main():
    os.makedirs(DST, exist_ok=True)
    for p in sorted(glob.glob(os.path.join(SRC, "*.dds"))):
        b = open(p, "rb").read()
        assert b[:4] == b"DDS "
        h = struct.unpack_from("<I", b, 12)[0]
        w = struct.unpack_from("<I", b, 16)[0]
        fourcc = b[84:88]
        if fourcc != b"DXT5":
            print("skip (not DXT5):", os.path.basename(p), fourcc)
            continue
        img = decode_dxt5(b[128:], w, h)

        # Faithful DXT5 -> RGBA conversion: colours are kept exactly as stored
        # in the source .dds (only alpha is preserved for foliage cut-outs).
        name = os.path.splitext(os.path.basename(p))[0]
        out = os.path.join(DST, name + ".png")
        Image.fromarray(img, "RGBA").save(out)
        a = img[..., 3]
        print("%-52s -> %dx%d alpha mean=%.0f <128=%.0f%%" % (
            os.path.basename(p), w, h, a.mean(), 100 * (a < 128).mean()))


if __name__ == "__main__":
    main()
