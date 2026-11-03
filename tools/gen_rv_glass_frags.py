#!/usr/bin/env python3
"""Generate pre-fractured glass shard meshes: rv_glass_frag1..12.gltf.

Each shard is a thin irregular prism lying roughly in the XZ plane (with a
small baked X tilt so settled debris reads as glass chips on the ground rather
than standing panes). The shapes are convex hulls of random point clouds, so
they read like Voronoi cells; at runtime the script computes a fresh Voronoi
layout per break from the server seed and picks a mesh from this pool for each
cell, so the crack pattern changes every time.

Both primitives use BLEND materials: the hidden material-0 primitive keeps the
importer's default slot, and having it transparent puts the whole entity in the
transparent render pass.
"""
import json
import math
import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFABS = ROOT / "projects/rv/assets/prefabs"


def convex_hull(points):
    """Monotone-chain convex hull; returns counter-clockwise points."""
    pts = sorted(set(points))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def _shard_shape(rng, target_n):
    """Irregular convex polygon roughly unit-sized, from a random point cloud."""
    while True:
        cloud = [(rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5))
                 for _ in range(target_n * 3)]
        hull = convex_hull(cloud)
        if 3 <= len(hull) <= target_n + 3:
            # Normalise so the largest extent fits in [-0.5, 0.5].
            xs = [p[0] for p in hull]
            zs = [p[1] for p in hull]
            span = max(max(xs) - min(xs), max(zs) - min(zs), 1e-3)
            k = 0.96 / span
            cx = (max(xs) + min(xs)) * 0.5
            cz = (max(zs) + min(zs)) * 0.5
            return [((p[0] - cx) * k, (p[1] - cz) * k) for p in hull]


_rng = random.Random(20261103)
SHAPES = [_shard_shape(_rng, _rng.choice((3, 4, 4, 5, 5, 6, 6, 7)))
          for _ in range(12)]
TILTS = [_rng.uniform(-0.6, 0.6) for _ in range(len(SHAPES))]
THICK = 0.035


def rot_x(p, a):
    c, s = math.cos(a), math.sin(a)
    return (p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c)


def norm(v):
    ln = math.sqrt(sum(c * c for c in v))
    if ln < 1e-9:
        return (0.0, 1.0, 0.0)
    return (v[0] / ln, v[1] / ln, v[2] / ln)


def add_face(pos, nrm, ind, verts, normal):
    base = len(pos)
    for v in verts:
        pos.append(v)
        nrm.append(normal)
    for i in range(1, len(verts) - 1):
        ind.extend([base, base + i, base + i + 1])


def build_prism(pos, nrm, ind, pts, tilt):
    h = THICK * 0.5
    top = [(x, h, z) for x, z in pts]
    bot = [(x, -h, z) for x, z in pts]
    add_face(pos, nrm, ind, top, (0.0, 1.0, 0.0))
    add_face(pos, nrm, ind, list(reversed(bot)), (0.0, -1.0, 0.0))
    n = len(pts)
    for i in range(n):
        x0, z0 = pts[i]
        x1, z1 = pts[(i + 1) % n]
        side = norm((z1 - z0, 0.0, -(x1 - x0)))
        quad = [(x0, h, z0), (x1, h, z1), (x1, -h, z1), (x0, -h, z0)]
        add_face(pos, nrm, ind, quad, side)
    # Bake a small tilt so settled shards are not perfectly flat/standing.
    for i in range(len(pos)):
        pos[i] = rot_x(pos[i], tilt)
        nrm[i] = norm(rot_x(nrm[i], tilt))


def write_shard(index):
    positions, normals, indices = [], [], []
    # Hidden material-0 triangle (importer default slot).
    add_face(positions, normals, indices,
             [(0.0, -0.6, 0.0), (0.02, -0.6, 0.0), (0.0, -0.58, 0.0)],
             (0.0, 0.0, 1.0))
    base = len(positions)
    build_prism(positions, normals, indices, SHAPES[index], TILTS[index])
    # The prism helper appended to the shared lists after the dummy, so only
    # the dummy occupies the first 3 vertices; split indices accordingly.
    dummy_ind = indices[:3]
    body_ind = [i - base for i in indices[3:]]
    body_pos = positions[base:]
    body_nrm = normals[base:]

    buffer = b""
    views = []
    accessors = []
    prims = []
    for (p, n, ix), mat in (((positions[:3], normals[:3], dummy_ind), 0),
                            ((body_pos, body_nrm, body_ind), 1)):
        for raw in (
                b"".join(struct.pack("<3f", *v) for v in p),
                b"".join(struct.pack("<3f", *v) for v in n),
                b"".join(struct.pack("<I", i) for i in ix)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 3
        mn = [min(v[i] for v in p) for i in range(3)]
        mx = [max(v[i] for v in p) for i in range(3)]
        accessors.extend([
            {"bufferView": vb, "componentType": 5126, "count": len(p),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": vb + 1, "componentType": 5126, "count": len(n),
             "type": "VEC3"},
            {"bufferView": vb + 2, "componentType": 5125, "count": len(ix),
             "type": "SCALAR"},
        ])
        ai = len(accessors) - 3
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1},
                      "indices": ai + 2, "material": mat})

    materials = [
        {"name": "frag_default",
         "alphaMode": "BLEND",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 0.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "frag_glass",
         "alphaMode": "BLEND",
         "doubleSided": True,
         "pbrMetallicRoughness": {"baseColorFactor": [0.52, 0.74, 0.90, 0.85],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.15}},
    ]
    name = "rv_glass_frag%d" % (index + 1)
    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv glass frag generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_GlassFrag%d" % (index + 1), "mesh": 0}],
        "meshes": [{"name": "RV_GlassFrag%d" % (index + 1), "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": name + ".bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / (name + ".bin")).write_bytes(buffer)
    (OUT / (name + ".gltf")).write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    (PREFABS / (name + ".json")).write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/" + name + ".gltf",
                     "castShadow": False, "receiveShadow": False},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(name, "verts", len(body_pos), "tris", len(body_ind) // 3)


def main():
    for i in range(len(SHAPES)):
        write_shard(i)


if __name__ == "__main__":
    main()
