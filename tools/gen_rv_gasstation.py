#!/usr/bin/env python3
"""Generate rv_gasstation.gltf: an enterable roadside gas station.

The shop uses the same solid-wall approach as rv_store (visible from inside and
outside, 1.6 m doorway at +Z) so `collidePoiWalls` and the interior loot slots
work unchanged. A four-post canopy with two pumps sits to the west of the shop.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_gasstation.json"


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
            indices.extend([base, base + 1, base + 2,
                            base, base + 2, base + 3])


def main():
    b = MeshBuilder()
    WALL, TRIM, ROOF, FLOOR, GLASS, RED, WHITE = range(7)

    # --- Shop shell (centred at the prefab origin) ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 2.55, 0, 6.5, 0.18, 5.5)
    b.box(WALL, 0, 1.25, -2.52, 6.2, 2.5, 0.16)
    b.box(WALL, -3.02, 1.25, 0, 0.16, 2.5, 5.2)
    b.box(WALL, 3.02, 1.25, 0, 0.16, 2.5, 5.2)
    b.box(WALL, -1.95, 1.25, 2.52, 2.30, 2.5, 0.16)
    b.box(WALL, 1.95, 1.25, 2.52, 2.30, 2.5, 0.16)
    b.box(WALL, 0, 2.30, 2.52, 1.60, 0.40, 0.16)
    b.box(TRIM, -0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(TRIM, 0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(TRIM, 0, 2.40, 2.56, 1.94, 0.20, 0.26)
    # Shop windows (dark glass overlays on the front wall).
    b.box(GLASS, -2.10, 1.62, 2.61, 1.30, 0.95, 0.04)
    b.box(GLASS, 2.10, 1.62, 2.61, 1.30, 0.95, 0.04)
    # Roof fascia + red GAS band.
    b.box(TRIM, 0, 2.66, 2.72, 6.6, 0.30, 0.16)
    b.box(RED, 0, 2.72, 2.82, 1.60, 0.34, 0.05)
    b.box(WHITE, 0, 2.72, 2.86, 1.10, 0.16, 0.02)

    # --- Interior: counter, shelves, coffee machine, mat ---
    b.box(TRIM, 1.55, 0.44, -0.80, 1.90, 0.88, 0.70)
    b.box(WHITE, 1.55, 0.90, -0.80, 1.98, 0.06, 0.78)
    b.box(TRIM, -1.50, 1.05, -2.24, 2.60, 2.10, 0.30)
    for k in range(3):
        b.box(WHITE, -1.50, 0.36 + k * 0.52, -2.06, 2.40, 0.05, 0.34)
    b.box(TRIM, 2.35, 1.04, -2.30, 0.55, 1.90, 0.45)
    b.box(RED, 2.35, 1.72, -2.04, 0.34, 0.30, 0.28)
    b.box(WHITE, 0, 0.075, 1.72, 1.90, 0.02, 1.10)
    b.box(TRIM, 0, 0.085, 2.86, 1.70, 0.02, 0.60)

    # --- Canopy and pumps (west of the shop) ---
    CANOPY_X, CANOPY_Z = -6.40, -1.20
    for ox in (-2.15, 2.15):
        for oz in (-1.55, 1.55):
            b.box(TRIM, CANOPY_X + ox, 1.60, CANOPY_Z + oz, 0.22, 3.20, 0.22)
    b.box(ROOF, CANOPY_X, 3.28, CANOPY_Z, 5.20, 0.22, 3.90)
    b.box(RED, CANOPY_X, 3.42, CANOPY_Z + 1.95, 5.20, 0.34, 0.10)
    for ox in (-1.25, 1.25):
        b.box(WHITE, CANOPY_X + ox, 0.12, CANOPY_Z, 1.30, 0.18, 0.80)
        b.box(TRIM, CANOPY_X + ox, 0.78, CANOPY_Z, 0.46, 1.20, 0.42)
        b.box(RED, CANOPY_X + ox, 1.46, CANOPY_Z, 0.50, 0.16, 0.46)
        b.box(WHITE, CANOPY_X + ox, 1.58, CANOPY_Z, 0.54, 0.10, 0.50)

    materials = [
        {"name": "gas_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.77, 0.68, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.85}},
        {"name": "gas_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.15, 0.16, 0.18, 1],
                                  "metallicFactor": 0.22, "roughnessFactor": 0.5}},
        {"name": "gas_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.32, 0.35, 1],
                                  "metallicFactor": 0.12, "roughnessFactor": 0.65}},
        {"name": "gas_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.34, 0.35, 0.37, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.9}},
        {"name": "gas_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.03, 0.04, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.25}},
        {"name": "gas_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.66, 0.16, 0.11, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.55}},
        {"name": "gas_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.86, 0.82, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.6}},
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
        "asset": {"version": "2.0", "generator": "neon rv gas station generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_GasStation", "mesh": 0}],
        "meshes": [{"name": "RV_GasStation", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_gasstation.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_gasstation.bin").write_bytes(buffer)
    (OUT / "rv_gasstation.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_gasstation.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"gas station bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
