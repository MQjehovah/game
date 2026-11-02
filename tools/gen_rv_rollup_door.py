#!/usr/bin/env python3
"""Generate rv_rollup_door.gltf: a slatted warehouse roll-up shutter.

The mesh is centred on the origin and spans y -1.3..1.3 (2.6 m tall) so the
runtime can roll it up by shrinking scaleY around the fixed top edge
(entity y = topY - height * scaleY / 2).
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_rollup_door.json"


class MeshBuilder:
    def __init__(self):
        self.groups = {}

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
            indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def main():
    b = MeshBuilder()
    DEFAULT, STEEL, DARK, WHITE = range(4)

    # Hidden material-0 box (importer default slot).
    b.box(0, 0, -0.6, 0, 0.02, 0.02, 0.02)

    # 11 horizontal slats, centred on the origin.
    for i in range(11):
        y = -1.20 + i * 0.24
        mat = STEEL if i % 2 == 0 else DARK
        b.box(mat, 0, y, 0, 1.72, 0.20, 0.10)
        b.box(WHITE, 0, y + 0.09, 0.056, 1.74, 0.02, 0.01)
    # Side rails + bottom bar + top roll drum.
    for x in (-0.90, 0.90):
        b.box(STEEL, x, 0, 0, 0.08, 2.60, 0.16)
    b.box(DARK, 0, -1.28, 0, 1.84, 0.12, 0.14)
    b.box(STEEL, 0, 1.32, 0, 1.90, 0.14, 0.20)
    # Warning stripes near the bottom.
    for i in range(6):
        b.box(WHITE, -0.62 + i * 0.25, -1.12, 0.056, 0.12, 0.10, 0.01)

    materials = [
        {"name": "door_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "door_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.46, 0.49, 0.52, 1],
                                  "metallicFactor": 0.55, "roughnessFactor": 0.4}},
        {"name": "door_dark",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.22, 0.24, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.5}},
        {"name": "door_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.85, 0.80, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.65}},
    ]

    buffer = b""
    views = []
    accessors = []
    prims = []
    for mat_index, (positions, normals, indices) in sorted(b.groups.items()):
        for raw in (
                b"".join(struct.pack("<3f", *p) for p in positions),
                b"".join(struct.pack("<3f", *n) for n in normals),
                b"".join(struct.pack("<I", i) for i in indices)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 3
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
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1},
                      "indices": ai + 2, "material": mat_index})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv rollup door generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_RollupDoor", "mesh": 0}],
        "meshes": [{"name": "RV_RollupDoor", "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": "rv_rollup_door.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / "rv_rollup_door.bin").write_bytes(buffer)
    (OUT / "rv_rollup_door.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_rollup_door.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print("rollup door bytes", len(buffer), "prims", len(prims))


if __name__ == "__main__":
    main()
