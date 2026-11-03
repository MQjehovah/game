#!/usr/bin/env python3
"""Generate pre-fractured glass shard meshes: rv_glass_frag1..5.gltf.

Each shard is a thin irregular prism lying roughly in the XZ plane (with a
small baked X tilt so settled debris reads as glass chips on the ground rather
than standing panes). The runtime spawns a fixed nine-piece layout per window
pane, so a break always separates along the same crack pattern.

Both primitives use BLEND materials: the hidden material-0 primitive keeps the
importer's default slot, and having it transparent puts the whole entity in the
transparent render pass.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFABS = ROOT / "projects/rv/assets/prefabs"

# Irregular convex polygons in XZ, roughly unit sized.
SHAPES = [
    [(-0.50, -0.30), (0.48, -0.45), (0.10, 0.48)],
    [(-0.48, -0.40), (0.25, -0.48), (0.50, 0.15), (-0.10, 0.46)],
    [(-0.45, -0.20), (0.05, -0.50), (0.50, -0.10), (0.30, 0.42), (-0.28, 0.38)],
    [(-0.50, 0.30), (-0.20, -0.42), (0.50, -0.20), (0.35, 0.45)],
    [(-0.40, 0.35), (0.42, 0.48), (0.05, -0.45)],
]

TILTS = [0.0, 0.20, -0.30, 0.45, -0.55]
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
