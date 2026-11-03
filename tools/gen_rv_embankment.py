#!/usr/bin/env python3
"""Generate rv_embankment.gltf: a grassy dirt ridge beside the main road.

The ridge runs along +Z next to the camp road (x 5.5..38), matching the
reference art direction: a dirt slope rising from the verge with a grassy top.
Its height undulates along Z and tapers at both ends. Quads on the road-facing
side use the dirt map, the crest/far side uses the meadow grass map.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_embankment.json"

XS = [5.0, 8.0, 11.5, 15.0, 19.0, 23.5, 28.0, 33.0, 38.0]
YS = [0.0, 0.70, 1.90, 3.00, 3.50, 3.55, 3.00, 1.50, 0.0]
Z0, Z1, STEPS = -24.0, 46.0, 36
DIRT_STRIPS = 3  # strips 0..DIRT_STRIPS-1 are dirt, the rest grass

# material slots (0 = hidden importer default)
DEFAULT, DIRT, GRASS = 0, 1, 2


def height(x, z):
    if x <= XS[0] or x >= XS[-1]:
        return 0.0
    base = 0.0
    for i in range(len(XS) - 1):
        if XS[i] <= x <= XS[i + 1]:
            t = (x - XS[i]) / (XS[i + 1] - XS[i])
            base = YS[i] + (YS[i + 1] - YS[i]) * t
            break
    base += 0.28 * math.sin(z * 0.23 + x * 0.35) + 0.12 * math.sin(z * 0.61)
    taper = min(1.0,
                max(0.0, (z - Z0) / 6.0),
                max(0.0, (Z1 - z) / 6.0))
    return base * taper


class Mesh:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], [], []))

    def quad(self, mat, a, b, c, d):
        u = [b[i] - a[i] for i in range(3)]
        v = [d[i] - a[i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(k * k for k in n)) or 1.0
        n = (n[0] / ln, n[1] / ln, n[2] / ln)
        if n[1] < 0:
            b, d = d, b
            n = (-n[0], -n[1], -n[2])
        pos, nrm, idx, uvs = self._group(mat)
        base = len(pos)
        for p in (a, b, c, d):
            pos.append(p)
            nrm.append(n)
            uvs.append((p[0] * 0.30, p[2] * 0.30))
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def box(self, mat, cx, cy, cz, sx, sy, sz):
        hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
        x0, x1 = cx - hx, cx + hx
        y0, y1 = cy - hy, cy + hy
        z0, z1 = cz - hz, cz + hz
        for n, quad in (
                ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
                ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
                ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
                ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
                ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
                ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)])):
            self.quad(mat, quad[0], quad[1], quad[2], quad[3])

    def hidden(self):
        self.box(DEFAULT, 0, -0.6, 0, 0.02, 0.02, 0.02)


def main():
    m = Mesh()
    m.hidden()
    dz = (Z1 - Z0) / STEPS
    rows = []
    for j in range(STEPS + 1):
        z = Z0 + dz * j
        shift = 0.8 * math.sin(z * 0.13)
        row = []
        for i, x in enumerate(XS):
            xx = x + shift
            row.append((xx, height(xx, z), z))
        rows.append(row)
    for j in range(STEPS):
        for i in range(len(XS) - 1):
            mat = DIRT if i < DIRT_STRIPS else GRASS
            a = rows[j][i]
            b = rows[j][i + 1]
            c = rows[j + 1][i + 1]
            d = rows[j + 1][i]
            m.quad(mat, a, b, c, d)

    materials = [
        {"name": "emb_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "emb_dirt",
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95,
                                  "baseColorTexture": {"index": 0}}},
        {"name": "emb_grass",
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9,
                                  "baseColorTexture": {"index": 1}}},
    ]

    buffer = b""
    views = []
    accessors = []
    prims = []
    for mat_index, (positions, normals, indices, uvs) in sorted(m.groups.items()):
        for raw in (
                b"".join(struct.pack("<3f", *p) for p in positions),
                b"".join(struct.pack("<3f", *n) for n in normals),
                b"".join(struct.pack("<I", i) for i in indices),
                b"".join(struct.pack("<2f", *t) for t in uvs)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 4
        mn = [min(p[i] for p in positions) for i in range(3)]
        mx = [max(p[i] for p in positions) for i in range(3)]
        accessors.extend([
            {"bufferView": vb, "componentType": 5126, "count": len(positions),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": vb + 1, "componentType": 5126, "count": len(normals),
             "type": "VEC3"},
            {"bufferView": vb + 2, "componentType": 5125, "count": len(indices),
             "type": "SCALAR"},
            {"bufferView": vb + 3, "componentType": 5126, "count": len(uvs),
             "type": "VEC2"},
        ])
        ai = len(accessors) - 4
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1,
                                     "TEXCOORD_0": ai + 3},
                      "indices": ai + 2, "material": mat_index})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv embankment generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Embankment", "mesh": 0}],
        "meshes": [{"name": "RV_Embankment", "primitives": prims}],
        "materials": materials,
        "images": [{"uri": "rv_dirt.png"}, {"uri": "rv_ground_meadow.png"}],
        "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
        "buffers": [{"uri": "rv_embankment.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / "rv_embankment.bin").write_bytes(buffer)
    (OUT / "rv_embankment.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_embankment.gltf",
                     "castShadow": False, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print("embankment bytes", len(buffer), "prims", len(prims),
          "verts", sum(len(v[0]) for v in m.groups.values()))


if __name__ == "__main__":
    main()
