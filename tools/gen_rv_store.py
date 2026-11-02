#!/usr/bin/env python3
"""Generate rv_store.gltf: an enterable roadside store.

The mesh is built from solid boxes (walls have thickness, so they are visible
from both outside and inside) around a 1.6 m front doorway. It replaces the
cube-scaled ruin shell for the "废宅/商店" POI so the building reads as real
architecture instead of a scaled-cube prototype.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_store.json"


class MeshBuilder:
    def __init__(self):
        self.groups = {}  # material -> (positions, normals, indices)

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], []))

    def box(self, mat, cx, cy, cz, sx, sy, sz):
        hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
        x0, x1 = cx - hx, cx + hx
        y0, y1 = cy - hy, cy + hy
        z0, z1 = cz - hz, cz + hz
        faces = [
            ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
            ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
            ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
            ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
            ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
            ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
        ]
        positions, normals, indices = self._group(mat)
        for n, quad in faces:
            base = len(positions)
            for q in quad:
                positions.append(q)
                normals.append(n)
            indices.extend([base, base + 1, base + 2,
                            base, base + 2, base + 3])


def main():
    b = MeshBuilder()
    CREAM = 0
    DARK = 1
    ROOF = 2
    FLOOR = 3
    RED = 4

    # Shell.
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 2.55, 0, 6.5, 0.18, 5.5)
    b.box(CREAM, 0, 1.25, -2.52, 6.2, 2.5, 0.16)
    b.box(CREAM, -3.02, 1.25, 0, 0.16, 2.5, 5.2)
    b.box(CREAM, 3.02, 1.25, 0, 0.16, 2.5, 5.2)
    # Front wall with a 1.6 m doorway: segments from x=+-0.8 to +-3.1.
    b.box(CREAM, -1.95, 1.25, 2.52, 2.30, 2.5, 0.16)
    b.box(CREAM, 1.95, 1.25, 2.52, 2.30, 2.5, 0.16)
    b.box(CREAM, 0, 2.30, 2.52, 1.60, 0.40, 0.16)

    # Door frame + header + sign/awning.
    b.box(DARK, -0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(DARK, 0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(DARK, 0, 2.40, 2.56, 1.94, 0.20, 0.26)
    b.box(DARK, 0, 2.68, 3.00, 2.40, 0.08, 0.92)
    b.box(RED, 0, 2.88, 2.63, 2.40, 0.42, 0.08)
    b.box(RED, -1.55, 2.72, 3.00, 0.10, 0.24, 0.86)
    b.box(RED, 1.55, 2.72, 3.00, 0.10, 0.24, 0.86)

    # Interior: counter (right), shelves (back-left), rug, door mat.
    b.box(DARK, 1.50, 0.44, -0.80, 2.00, 0.88, 0.70)
    b.box(CREAM, 1.50, 0.90, -0.80, 2.08, 0.06, 0.78)
    b.box(DARK, -1.50, 1.05, -2.24, 2.60, 2.10, 0.30)
    for k in range(3):
        b.box(CREAM, -1.50, 0.36 + k * 0.52, -2.06, 2.40, 0.05, 0.34)
    b.box(CREAM, 0, 0.075, 1.72, 1.90, 0.02, 1.10)
    b.box(DARK, 0, 0.085, 2.86, 1.70, 0.02, 0.60)

    materials = [
        {"name": "store_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.74, 0.66, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.85}},
        {"name": "store_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.14, 0.15, 0.17, 1],
                                  "metallicFactor": 0.2, "roughnessFactor": 0.55}},
        {"name": "store_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.27, 0.29, 0.32, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.7}},
        {"name": "store_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.33, 0.34, 0.36, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.9}},
        {"name": "store_sign",
         "pbrMetallicRoughness": {"baseColorFactor": [0.62, 0.18, 0.13, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.6}},
    ]

    buffer = b""
    buffer_views = []
    accessors = []
    primitives = []
    for mat_index, (positions, normals, indices) in sorted(b.groups.items()):
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
        for raw in (pos_bytes, nrm_bytes, idx_bytes):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            buffer_views.append({"buffer": 0, "byteOffset": len(buffer),
                                 "byteLength": len(blob)})
            buffer += blob
        vb = len(buffer_views) - 3
        mn = [min(p[i] for p in positions) for i in range(3)]
        mx = [max(p[i] for p in positions) for i in range(3)]
        accessors.extend([
            {"bufferView": vb, "componentType": 5126, "count": len(positions),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": vb + 1, "componentType": 5126, "count": len(normals),
             "type": "VEC3"},
            {"bufferView": vb + 2, "componentType": 5125, "count": len(indices),
             "type": "SCALAR"},
        ])
        ai = len(accessors) - 3
        primitives.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1},
                           "indices": ai + 2, "material": mat_index})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv store generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Store", "mesh": 0}],
        "meshes": [{"name": "RV_Store", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_store.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_store.bin").write_bytes(buffer)
    (OUT / "rv_store.gltf").write_text(json.dumps(gltf, separators=(",", ":")),
                                      encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_store.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"store verts={len(buffer_views)} views, bytes={len(buffer)}, "
          f"prims={len(primitives)}")


if __name__ == "__main__":
    main()
