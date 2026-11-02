#!/usr/bin/env python3
"""Generate rv_sliding_door.gltf: a barn-style sliding door panel.

The mesh is centred on the origin, spans x -0.93..0.93 (1.86 m) and
y -1.21..1.21 (2.42 m), and is thin along Z, so the runtime can slide it
horizontally from the bay opening to a parked position over the front wall.

Details: five vertical red planks, two white rails, a centre batten, top roller
brackets, white handle and a small number plate.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_sliding_door.json"


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
    DEFAULT, RED, WHITE, STEEL, DARK = range(5)

    # Hidden material-0 box (importer default slot).
    b.box(DEFAULT, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # Five vertical red planks.
    for i in range(5):
        x = -0.744 + i * 0.372
        b.box(RED, x, 0, 0, 0.352, 2.42, 0.08)
        # Vertical shadow gap between planks.
        if i < 4:
            b.box(DARK, x + 0.186, 0, 0.043, 0.020, 2.38, 0.004)

    # White horizontal rails (top / middle / bottom) on the outer face.
    for y in (-1.02, 0.0, 1.02):
        b.box(WHITE, 0, y, 0.055, 1.80, 0.18, 0.03)
    # Centre batten + white frame edge on the leading side.
    b.box(WHITE, 0, 0, 0.070, 0.16, 2.20, 0.02)
    b.box(WHITE, 0.86, 0, 0.062, 0.10, 2.34, 0.03)

    # Top roller brackets that ride the track.
    for x in (-0.70, 0.70):
        b.box(STEEL, x, 1.17, 0.025, 0.16, 0.12, 0.14)
        b.box(STEEL, x, 1.25, 0.025, 0.10, 0.10, 0.10)

    # Handle on the leading edge + number plate.
    b.box(STEEL, 0.72, 0.10, 0.10, 0.08, 0.44, 0.06)
    b.box(DARK, 0.72, 0.32, 0.13, 0.05, 0.05, 0.05)
    b.box(WHITE, 0.0, 0.55, 0.095, 0.30, 0.22, 0.01)
    b.box(DARK, 0.0, 0.55, 0.101, 0.16, 0.03, 0.005)

    # Small diagonal brace hinted with stepped blocks.
    for i in range(6):
        b.box(WHITE, -0.62 + i * 0.25, -0.42 + i * 0.17, 0.058,
              0.20, 0.12, 0.02)

    materials = [
        {"name": "slidedoor_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "slidedoor_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.13, 0.10, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.8}},
        {"name": "slidedoor_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.87, 0.85, 0.78, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.65}},
        {"name": "slidedoor_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.45, 0.48, 1],
                                  "metallicFactor": 0.5, "roughnessFactor": 0.45}},
        {"name": "slidedoor_dark",
         "pbrMetallicRoughness": {"baseColorFactor": [0.16, 0.13, 0.11, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.85}},
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
        "asset": {"version": "2.0", "generator": "neon rv sliding door generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_SlidingDoor", "mesh": 0}],
        "meshes": [{"name": "RV_SlidingDoor", "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": "rv_sliding_door.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / "rv_sliding_door.bin").write_bytes(buffer)
    (OUT / "rv_sliding_door.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_sliding_door.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print("sliding door bytes", len(buffer), "prims", len(prims))


if __name__ == "__main__":
    main()
