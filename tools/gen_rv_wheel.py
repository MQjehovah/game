#!/usr/bin/env python3
"""Generate rv_wheel.gltf: one standalone wheel for per-wheel suspension.

The body mesh used to bake all four wheels into rv_body.gltf. They are now
separate entities so each one can follow its own terrain contact (suspension
travel) and the front pair can steer. This mesh is authored around the origin
with its axle along X and the tyre radius 0.40, matching the old wheel wells
(wheel centres +-1.26 m across, +-2.92 m along, centre height 0.38).
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_wheel.json"

R, HALF = 0.40, 0.12
RIM = 0.24
SEG = 16

DEFAULT, TIRE, HUB = 0, 1, 2


class Mesh:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], [], []))

    def quad(self, mat, a, b, c, d, normal=None):
        u = [b[i] - a[i] for i in range(3)]
        v = [d[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0]]
        ln = math.sqrt(sum(k * k for k in n)) or 1.0
        n = [k / ln for k in n]
        if normal is None:
            normal = n
        elif n[0] * normal[0] + n[1] * normal[1] + n[2] * normal[2] < 0:
            b, d = d, b
        pos, nrm, idx, uvs = self._group(mat)
        base = len(pos)
        for p in (a, b, c, d):
            pos.append(p)
            nrm.append(normal)
            uvs.append((p[2] * 0.8 + 0.5, p[1] * 0.8 + 0.5))
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def disc(self, mat, x, cy, cz, radius, facing):
        for i in range(SEG):
            a0 = 2 * math.pi * i / SEG
            a1 = 2 * math.pi * (i + 1) / SEG
            p0 = (x, cy + math.cos(a0) * radius, cz + math.sin(a0) * radius)
            p1 = (x, cy + math.cos(a1) * radius, cz + math.sin(a1) * radius)
            self.quad(mat, (x, cy, cz), p0, p1, (x, cy, cz), (facing, 0, 0))

    def hidden(self):
        self.quad(DEFAULT, (0, -2, 0), (0, -1.98, 0), (0.02, -1.98, 0),
                  (0.02, -2, 0), (0, 1, 0))


def build(m):
    m.hidden()
    for i in range(SEG):
        a0 = 2 * math.pi * i / SEG
        a1 = 2 * math.pi * (i + 1) / SEG
        y0, z0 = math.cos(a0) * R, math.sin(a0) * R
        y1, z1 = math.cos(a1) * R, math.sin(a1) * R
        mid = (a0 + a1) * 0.5
        n = (0.0, math.cos(mid), math.sin(mid))
        # tread band
        m.quad(TIRE, (-HALF, y0, z0), (HALF, y0, z0),
               (HALF, y1, z1), (-HALF, y1, z1), n)
        # sidewalls (annulus down to the rim)
        for xs, facing in ((-HALF, -1.0), (HALF, 1.0)):
            m.quad(TIRE, (xs, y0, z0), (xs, y1, z1),
                   (xs, y1 * RIM / R, z1 * RIM / R),
                   (xs, y0 * RIM / R, z0 * RIM / R), (facing, 0, 0))
    # rim discs on both faces
    for xs, facing in ((-HALF - 0.005, -1.0), (HALF + 0.005, 1.0)):
        m.disc(HUB, xs, 0, 0, RIM, facing)
        # five dark lug holes make the spin readable without a texture
        for k in range(5):
            a = 2 * math.pi * k / 5
            cy, cz = math.cos(a) * 0.155, math.sin(a) * 0.155
            m.disc(TIRE, xs + facing * 0.004, cy, cz, 0.045, facing)
        m.disc(HUB, xs + facing * 0.008, 0, 0, 0.065, facing)


def main():
    m = Mesh()
    build(m)

    materials = [
        {"name": "wheel_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "wheel_tire",
         "pbrMetallicRoughness": {"baseColorFactor": [0.07, 0.07, 0.08, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "wheel_hub",
         "pbrMetallicRoughness": {"baseColorFactor": [0.62, 0.64, 0.66, 1],
                                  "metallicFactor": 0.65, "roughnessFactor": 0.35}},
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
        "asset": {"version": "2.0", "generator": "neon rv wheel generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Wheel", "mesh": 0}],
        "meshes": [{"name": "RV_Wheel", "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": "rv_wheel.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / "rv_wheel.bin").write_bytes(buffer)
    (OUT / "rv_wheel.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_wheel.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print("rv_wheel bytes", len(buffer), "prims", len(prims),
          "verts", sum(len(v[0]) for v in m.groups.values()))


if __name__ == "__main__":
    main()
