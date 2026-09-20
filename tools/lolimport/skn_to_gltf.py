#!/usr/bin/env python3
"""Convert League of Legends ``.skn`` (Simple Skin mesh) files to glTF/GLB.

Format (Riot "SknFile"; see Pupix/lol-skn-parser):

    int32  magic (0x00112233)
    uint16 version
    uint16 numObjects
    int32  materialCount
    material[materialCount]: char name[64], int32 startVertex, numVertices,
                             startIndex, numIndices
    (v4 only) int32 unknown
    int32  indexCount
    int32  vertexCount
    (v4 only) uint16 pad[24]
    int16  indices[indexCount]
    vertex[vertexCount]: float pos[3], uint8 joints[4], float weights[4],
                         float normal[3], float uv[2]

Vertices are emitted in bind pose (no skeleton applied), which is what a
static placement needs. One glTF primitive is produced per SKN material.

Usage::

    python tools/lolimport/skn_to_gltf.py --skn X.skn --texture X.png \
        --out Y.glb --name Minion
"""

import argparse
import json
import os
import struct
import numpy as np

SKN_MAGIC = 0x00112233


def parse_skn(path):
    b = open(path, "rb").read()
    off = 0

    def u32():
        nonlocal off
        v = struct.unpack_from("<I", b, off)[0]
        off += 4
        return v

    def i32():
        nonlocal off
        v = struct.unpack_from("<i", b, off)[0]
        off += 4
        return v

    def u16():
        nonlocal off
        v = struct.unpack_from("<H", b, off)[0]
        off += 2
        return v

    magic = u32()
    if magic != SKN_MAGIC:
        raise ValueError("bad skn magic 0x%08x" % magic)
    version = u16()
    _num_objects = u16()

    mat_count = i32()
    materials = []
    for _ in range(mat_count):
        name = b[off:off + 64].split(b"\x00", 1)[0].decode("latin-1")
        off += 64
        sv, nv, si, ni = i32(), i32(), i32(), i32()
        materials.append({"name": name, "start_vertex": sv, "num_vertices": nv,
                          "start_index": si, "num_indices": ni})

    if version >= 4:
        _unk = i32()
    index_count = i32()
    vertex_count = i32()
    if version >= 4:
        off += 48  # uint16 pad[24]

    indices = np.frombuffer(b, dtype="<i2", count=index_count, offset=off).astype(np.int32)
    off += index_count * 2

    vtx = np.frombuffer(b, dtype=np.uint8, count=vertex_count * 52,
                        offset=off).reshape(vertex_count, 52)
    pos = vtx[:, 0:12].copy().view("<f4").reshape(vertex_count, 3)
    joints = vtx[:, 12:16]
    weights = vtx[:, 16:32].copy().view("<f4").reshape(vertex_count, 4)
    nrm = vtx[:, 32:44].copy().view("<f4").reshape(vertex_count, 3)
    uv = vtx[:, 44:52].copy().view("<f4").reshape(vertex_count, 2)
    return version, materials, indices, pos, nrm, uv, joints, weights


class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []

    def add(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        o = len(self.bin)
        self.bin.extend(data)
        v = {"buffer": 0, "byteOffset": o, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def acc(self, view, ct, at, n, mn=None, mx=None):
        a = {"bufferView": view, "componentType": ct, "type": at, "count": n}
        if mn is not None:
            a["min"] = mn
            a["max"] = mx
        self.accessors.append(a)
        return len(self.accessors) - 1


def convert(skn_path, tex_png, out_path, name):
    version, materials, indices, pos, nrm, uv, joints, weights = parse_skn(skn_path)
    print("skn v%d: %d materials, %d verts, %d indices" %
          (version, len(materials), len(pos), len(indices)))

    g = Glb()
    prims = []
    gltf_mats = []
    images, textures = [], []
    tex_index = None
    if tex_png and os.path.isfile(tex_png):
        png = open(tex_png, "rb").read()
        v = g.add(png)
        images.append({"bufferView": v, "mimeType": "image/png"})
        textures.append({"source": 0, "sampler": 0})
        tex_index = 0

    for m in materials:
        sv, nv, si, ni = m["start_vertex"], m["num_vertices"], m["start_index"], m["num_indices"]
        if nv <= 0 or ni <= 0:
            continue
        idx = indices[si:si + ni].copy()
        # keep only triangles fully inside this vertex range
        if idx.min() < sv or idx.max() >= sv + nv:
            mask = (idx >= sv) & (idx < sv + nv)
            # drop whole triangles with out-of-range verts
            tri = idx.reshape(-1, 3)
            good = np.all((tri >= sv) & (tri < sv + nv), axis=1)
            idx = tri[good].reshape(-1)
            if idx.size == 0:
                continue
        idx = (idx - sv).astype(np.uint32)
        p = pos[sv:sv + nv].astype(np.float32)
        n = nrm[sv:sv + nv].astype(np.float32)
        t = uv[sv:sv + nv].astype(np.float32)

        # Drop degenerate "spike" triangles (a few stray vertices whose
        # position is in a different space tear long thin triangles across the
        # scene). Threshold = the model's bounding-box diagonal.
        tri = idx.reshape(-1, 3).astype(np.int64)
        mn = p.min(axis=0)
        mx = p.max(axis=0)
        diag = float(np.linalg.norm(mx - mn)) or 1.0
        thr = min(diag, 1000.0)
        a = p[tri[:, 0]]
        b = p[tri[:, 1]]
        c = p[tri[:, 2]]
        e = np.maximum.reduce([np.linalg.norm(a - b, axis=1),
                               np.linalg.norm(b - c, axis=1),
                               np.linalg.norm(c - a, axis=1)])
        keep = e <= thr
        if not keep.all():
            print("       dropped %d spike tris (thr=%.0f)" % ((~keep).sum(), thr))
            tri = tri[keep]
        idx = tri.reshape(-1).astype(np.uint32)
        if idx.size == 0:
            continue

        pv = g.add(p.tobytes(), 34962)
        pacc = g.acc(pv, 5126, "VEC3", len(p), p.min(0).tolist(), p.max(0).tolist())
        nv_v = g.add(n.tobytes(), 34962)
        nacc = g.acc(nv_v, 5126, "VEC3", len(n))
        tv = g.add(t.tobytes(), 34962)
        tacc = g.acc(tv, 5126, "VEC2", len(t))
        iv = g.add(idx.tobytes(), 34963)
        iacc = g.acc(iv, 5125, "SCALAR", len(idx))

        mat = {"name": m["name"] or (name + "_mat"), "doubleSided": True,
               "alphaMode": "MASK", "alphaCutoffFactor": 0.35,
               "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                        "metallicFactor": 0.0, "roughnessFactor": 0.8}}
        if tex_index is not None:
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": tex_index}
        gltf_mats.append(mat)
        prims.append({"attributes": {"POSITION": pacc, "NORMAL": nacc, "TEXCOORD_0": tacc},
                      "indices": iacc, "material": len(gltf_mats) - 1, "mode": 4})

    if not prims:
        raise ValueError("no primitives")

    gltf = {"asset": {"version": "2.0", "generator": "neon skn_to_gltf"},
            "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "name": name}],
            "meshes": [{"name": name, "primitives": prims}],
            "materials": gltf_mats, "buffers": [{"byteLength": 0}],
            "bufferViews": g.views, "accessors": g.accessors}
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]

    while len(g.bin) % 4:
        g.bin.append(0)
    gltf["buffers"][0]["byteLength"] = len(g.bin)
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(g.bin)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, total))
        fh.write(struct.pack("<I4s", len(js), b"JSON"))
        fh.write(js)
        fh.write(struct.pack("<I4s", len(g.bin), b"BIN\x00"))
        fh.write(bytes(g.bin))
    print("wrote %s (%.1f KB, %d primitives)" %
          (out_path, os.path.getsize(out_path) / 1e3, len(prims)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skn", required=True)
    ap.add_argument("--texture", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="model")
    a = ap.parse_args()
    convert(a.skn, a.texture, a.out, a.name)
