#!/usr/bin/env python3
"""Generate rv_warehouse.gltf: an enterable logistics warehouse.

The shell keeps the shared 6.2 x 5.2 POI footprint so `collidePoiWalls` and the
interior loot slots work unchanged. It adds a taller shed, a wide loading-bay
door with an exterior apron/stripes/bollards, and steel racks + pallets inside.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_warehouse.json"


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
    WALL, TRIM, ROOF, FLOOR, GLASS, YELLOW, STEEL, WOOD, RED = range(9)
    H = 3.6

    # --- Shed shell, wide loading-bay doorway at +Z ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 3.72, 0, 6.6, 0.22, 5.6)
    b.box(TRIM, 0, 3.90, 0, 6.8, 0.14, 5.8)   # parapet/edge
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, -1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 0, 3.10, 2.52, 1.60, 1.00, 0.16)  # header over the bay
    # High clerestory windows on the sides and back.
    b.box(GLASS, -3.10, 3.00, 0, 0.04, 0.55, 3.6)
    b.box(GLASS, 3.10, 3.00, 0, 0.04, 0.55, 3.6)
    b.box(GLASS, 0, 3.00, -2.61, 4.2, 0.55, 0.04)

    # --- Loading bay frame, roll-up drum, apron, stripes, bollards ---
    b.box(TRIM, -0.90, 1.35, 2.56, 0.20, 2.70, 0.30)
    b.box(TRIM, 0.90, 1.35, 2.56, 0.20, 2.70, 0.30)
    b.box(TRIM, 0, 2.72, 2.56, 2.00, 0.22, 0.30)
    b.box(STEEL, 0, 2.95, 2.56, 1.80, 0.34, 0.22)  # roll-up drum
    for i in range(4):
        b.box(YELLOW, 0, 2.62 - i * 0.09, 2.48, 1.62, 0.06, 0.04)
    b.box(TRIM, 0, 0.07, 3.65, 4.40, 0.10, 2.10)   # concrete apron
    for ox in (-1.35, 0.0, 1.35):
        b.box(YELLOW, ox, 0.13, 4.55, 0.95, 0.02, 0.16)
    for ox in (-2.05, 2.05):
        b.box(RED, ox, 0.45, 4.35, 0.14, 0.80, 0.14)
        b.box(STEEL, ox, 0.86, 4.35, 0.18, 0.06, 0.18)
    b.box(RED, 0, 3.30, 2.64, 2.20, 0.40, 0.06)
    b.box(YELLOW, 0, 3.30, 2.68, 1.40, 0.16, 0.02)

    # --- Interior racks: left, right and back ---
    for rack_x in (-2.55, 2.55):
        for rack_z in (-1.85, 1.85):
            b.box(STEEL, rack_x, 1.25, rack_z, 0.12, 2.50, 0.12)
        for shelf_y in (0.55, 1.30, 2.05):
            b.box(STEEL, rack_x, shelf_y, 0.0, 0.12, 0.08, 3.70)
            b.box(WOOD, rack_x, shelf_y + 0.06, 0.0, 0.55, 0.04, 3.55)
    for rack_x in (-1.55, 1.55):
        b.box(STEEL, rack_x, 1.30, -2.30, 0.12, 2.60, 0.12)
    for shelf_y in (0.55, 1.35, 2.15):
        b.box(STEEL, 0, shelf_y, -2.30, 3.10, 0.08, 0.12)
        b.box(WOOD, 0, shelf_y + 0.06, -2.30, 3.00, 0.04, 0.55)

    # --- Pallets and crates inside ---
    pallets = [(-1.20, 0.22, 1.30), (0.55, 0.22, 1.55), (1.85, 0.22, -1.15),
               (-1.95, 0.22, -1.10)]
    for i, (px, py, pz) in enumerate(pallets):
        b.box(WOOD, px, py - 0.14, pz, 0.90, 0.10, 0.75)
        b.box(WOOD if i % 2 == 0 else STEEL, px, py + 0.18, pz, 0.72, 0.55, 0.58)

    materials = [
        {"name": "wh_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.52, 0.55, 0.58, 1],
                                  "metallicFactor": 0.12, "roughnessFactor": 0.8}},
        {"name": "wh_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.13, 0.14, 0.16, 1],
                                  "metallicFactor": 0.3, "roughnessFactor": 0.45}},
        {"name": "wh_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.32, 0.35, 1],
                                  "metallicFactor": 0.15, "roughnessFactor": 0.6}},
        {"name": "wh_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.28, 0.29, 0.31, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.95}},
        {"name": "wh_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.03, 0.04, 0.05, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.3}},
        {"name": "wh_yellow",
         "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.62, 0.12, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.65}},
        {"name": "wh_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.44, 0.47, 0.50, 1],
                                  "metallicFactor": 0.55, "roughnessFactor": 0.4}},
        {"name": "wh_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.48, 0.36, 0.24, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "wh_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.62, 0.16, 0.12, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.55}},
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
        "asset": {"version": "2.0", "generator": "neon rv warehouse generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Warehouse", "mesh": 0}],
        "meshes": [{"name": "RV_Warehouse", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_warehouse.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_warehouse.bin").write_bytes(buffer)
    (OUT / "rv_warehouse.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_warehouse.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"warehouse bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
