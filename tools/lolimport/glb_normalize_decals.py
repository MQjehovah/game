#!/usr/bin/env python3
"""Fix Summoner's Rift ground-decal UVs so each placed stamp maps the texture once.

The `*_decalVersion3_*` materials are single-stamp ground decals (a tower-base
ring, a flower cluster, a chasm blob, a mossy rock patch) whose textures have a
soft alpha border.  The source FBX gives them a shared world-projected UV field,
so every stamp tiles 2-8x across its patch - the ground renders as a chaotic
mosaic and, e.g., the tower-base ring (`NVRMaterial_lanetowerdcl_...`) smears
everywhere.  The game itself re-projects these decals with shader/decal data the
static glb does not carry.

This tool rebuilds the intended placement from the geometry: it splits each
decal primitive's triangle soup into connected components (one component = one
placed decal instance) and remaps that component's TEXCOORD_0 bounding box onto
[0,1].  Geometry (POSITION / NORMAL / indices) is untouched; only the decal
primitives' TEXCOORD_0 accessors are rewritten in place, so the output is the
same size as the input.

Usage::

    python tools/lolimport/glb_normalize_decals.py \
        --in  projects/moba/assets/models/sr/sr_map.glb \
        --out build-msvc/sr_map_decals.glb
"""
import argparse
import json
import os
import struct

import numpy as np

DECAL_TAG = "decalVersion3"

CT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
CT_DTYPE = {5120: np.int8, 5121: np.uint8, 5122: np.int16,
            5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_glb(path):
    data = open(path, "rb").read()
    magic, ver, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise SystemExit("%s is not a .glb" % path)
    off, js, binc = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            binc = bytearray(chunk)
    return js, binc


def write_glb(path, js, binc):
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(jb) % 4:
        jb += b" "
    while len(binc) % 4:
        binc.append(0)
    total = 12 + 8 + len(jb) + 8 + len(binc)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, total))
        fh.write(struct.pack("<I4s", len(jb), b"JSON"))
        fh.write(jb)
        fh.write(struct.pack("<I4s", len(binc), b"BIN\x00"))
        fh.write(bytes(binc))


def acc_info(js, i):
    a = js["accessors"][i]
    bv = js["bufferViews"][a["bufferView"]]
    n = NCOMP[a["type"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride") or (CT_SIZE[a["componentType"]] * n)
    return a, bv, CT_DTYPE[a["componentType"]], n, a["count"], off, stride


def read_acc(js, binc, i):
    a, bv, dt, n, count, off, stride = acc_info(js, i)
    itemsize = CT_SIZE[a["componentType"]] * n
    if stride == itemsize:
        return np.frombuffer(bytes(binc), dtype=dt, count=count * n,
                             offset=off).reshape(count, n)
    out = np.empty((count, n), dtype=dt)
    for r in range(count):
        out[r] = np.frombuffer(bytes(binc), dtype=dt, count=n,
                               offset=off + r * stride)
    return out


def write_acc(js, binc, i, data):
    a, bv, dt, n, count, off, stride = acc_info(js, i)
    itemsize = CT_SIZE[a["componentType"]] * n
    if stride == itemsize:
        binc[off:off + count * itemsize] = \
            np.ascontiguousarray(data, dtype=dt).tobytes()
        return
    for r in range(count):
        binc[off + r * stride: off + r * stride + itemsize] = \
            np.ascontiguousarray(data[r], dtype=dt).tobytes()


def connected_components(nverts, idx):
    parent = np.arange(nverts)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in idx.reshape(-1, 3):
        r0, r1, r2 = find(int(t[0])), find(int(t[1])), find(int(t[2]))
        if r0 != r1:
            parent[r1] = r0
        if r0 != r2:
            parent[r2] = r0
    return np.array([find(i) for i in range(nverts)])


def normalize_prim(js, binc, prim, mode):
    attrs = prim["attributes"]
    pos = read_acc(js, binc, attrs["POSITION"]).astype(np.float64)
    uv = read_acc(js, binc, attrs["TEXCOORD_0"]).astype(np.float64)
    idx = read_acc(js, binc, prim["indices"]).astype(np.int64)
    roots = connected_components(len(pos), idx)
    newuv = uv.copy()
    ncomp = 0
    for r in np.unique(roots):
        sel = roots == r
        ncomp += 1
        if mode == "world":
            u = np.column_stack([pos[sel, 0], pos[sel, 2]])
        else:
            u = uv[sel]
        umin, umax = u.min(axis=0), u.max(axis=0)
        span = umax - umin
        if mode == "square":
            c = (umin + umax) * 0.5
            s = span.max()
            newuv[sel] = (u - c) / s + 0.5 if s > 1e-9 else 0.5
        else:
            safe = np.where(span > 1e-9, span, 1.0)
            newuv[sel] = np.where(span > 1e-9, (u - umin) / safe, 0.5)
    write_acc(js, binc, attrs["TEXCOORD_0"], newuv)
    return ncomp


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default=DECAL_TAG,
                    help="material-name substring marking decals (default %r)" % DECAL_TAG)
    ap.add_argument("--mode", choices=["fit", "square", "world"], default="fit",
                    help="fit: uv bbox -> [0,1] (default); square: centre with a "
                         "uniform square; world: world-XZ bbox -> [0,1]")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change without writing")
    args = ap.parse_args()

    js, binc = read_glb(args.inp)
    mats = js.get("materials", [])
    total_prim = total_comp = 0
    for mesh in js["meshes"]:
        for prim in mesh["primitives"]:
            mi = prim.get("material", -1)
            if mi < 0 or args.tag not in mats[mi].get("name", ""):
                continue
            if "TEXCOORD_0" not in prim["attributes"]:
                continue
            total_prim += 1
            total_comp += normalize_prim(js, binc, prim, args.mode)
    if args.dry_run:
        print("%d decal primitives, %d components (dry-run, nothing written)"
              % (total_prim, total_comp))
        return
    write_glb(args.out, js, binc)
    print("normalized %d decal primitives (%d placed instances) -> %s (%.1f MB)"
          % (total_prim, total_comp, args.out, os.path.getsize(args.out) / 1e6))


if __name__ == "__main__":
    main()
