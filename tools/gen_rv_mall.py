#!/usr/bin/env python3
"""Generate rv_mall.gltf: an enterable abandoned mall store unit.

Uses the shared 6.2 x 5.2 POI footprint so the wall collision and interior loot
slots work unchanged. The storefront is a tall glass facade with a red/blue
sign, checkout counters, product shelves, clothing racks, mannequins and a few
exterior props (planters, bench, cart).
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_mall.json"


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
    (WALL, TRIM, ROOF, FLOOR, GLASS, RED, BLUE, STEEL,
     WARM, COOL, CLOTH, WHITE, DARK) = range(1, 14)
    H = 4.0

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shell: tall store unit, glass storefront, flat roof ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 4.12, 0, 6.6, 0.20, 5.6)
    b.box(TRIM, 0, 4.28, 0, 6.8, 0.14, 5.8)
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, -1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 0, 3.30, 2.52, 1.60, 1.40, 0.16)
    # Storefront glass: two large panes either side of the doorway.
    for sx in (-2.10, 2.10):
        b.box(GLASS, sx, 1.75, 2.61, 1.55, 2.10, 0.04)
        b.box(STEEL, sx + 0.82, 1.75, 2.62, 0.08, 2.14, 0.05)
        b.box(STEEL, sx - 0.82, 1.75, 2.62, 0.08, 2.14, 0.05)
    b.box(STEEL, -0.86, 1.35, 2.56, 0.18, 2.70, 0.28)
    b.box(STEEL, 0.86, 1.35, 2.56, 0.18, 2.70, 0.28)
    b.box(STEEL, 0, 2.75, 2.56, 1.90, 0.18, 0.28)
    # Red/blue sign band + MALL letter blocks.
    b.box(RED, 0, 3.55, 2.62, 3.60, 0.62, 0.10)
    b.box(BLUE, 0, 3.55, 2.67, 2.90, 0.16, 0.03)
    for i, ch in enumerate("MALL"):
        b.box(WHITE, -1.35 + i * 0.90, 3.55, 2.69, 0.52, 0.30, 0.03)
    # Awning, side high windows, roof AC units.
    b.box(TRIM, 0, 2.92, 3.02, 2.60, 0.08, 0.95)
    b.box(GLASS, -3.10, 3.20, 0, 0.04, 0.55, 3.8)
    b.box(GLASS, 3.10, 3.20, 0, 0.04, 0.55, 3.8)
    for ox, oz in ((-1.6, -1.2), (1.5, 0.9)):
        b.box(STEEL, ox, 4.34, oz, 1.10, 0.40, 0.80)
        b.box(TRIM, ox, 4.56, oz, 0.90, 0.06, 0.62)

    # --- Interior shop fit-out ---
    # Checkout counters.
    b.box(TRIM, 1.60, 0.45, -1.10, 2.20, 0.90, 0.70)
    b.box(WHITE, 1.60, 0.93, -1.10, 2.28, 0.06, 0.78)
    b.box(TRIM, -1.65, 0.45, 0.50, 1.80, 0.90, 0.70)
    b.box(WHITE, -1.65, 0.93, 0.50, 1.88, 0.06, 0.78)
    # Back-wall shelving with products.
    b.box(STEEL, 0, 1.10, -2.30, 4.20, 2.20, 0.18)
    for shelf_y in (0.45, 1.10, 1.75):
        b.box(WHITE, 0, shelf_y, -2.16, 4.00, 0.05, 0.34)
    cols = (WARM, COOL, CLOTH, BLUE, RED, WHITE)
    for i in range(10):
        px = -1.75 + (i % 5) * 0.88
        py = 0.56 + (i // 5) * 0.65
        b.box(cols[i % len(cols)], px, py, -2.08, 0.34, 0.30, 0.20)
    # Center display tables + products.
    for tx, tz in ((-0.35, -0.30), (1.75, 0.85)):
        b.box(WHITE, tx, 0.38, tz, 1.30, 0.10, 0.85)
        for lx in (-0.42, 0.0, 0.42):
            b.box(STEEL, tx + lx, 0.19, tz, 0.06, 0.38, 0.06)
        b.box(WARM, tx - 0.32, 0.53, tz, 0.28, 0.20, 0.28)
        b.box(COOL, tx + 0.34, 0.53, tz, 0.30, 0.22, 0.30)
    # Clothing rack along the left wall.
    for rz in (-1.40, 0.40):
        b.box(STEEL, -2.55, 0.90, rz, 0.06, 1.80, 0.06)
    b.box(STEEL, -2.55, 1.80, -0.50, 0.06, 0.06, 2.20)
    for i in range(5):
        b.box((CLOTH if i % 2 == 0 else BLUE), -2.55, 1.30, -1.30 + i * 0.40,
              0.08, 0.80, 0.30)
    # Mannequins near the storefront.
    for mx in (-2.15, 2.15):
        b.box(DARK, mx, 0.95, 1.90, 0.30, 0.90, 0.24)
        b.box(WHITE, mx, 1.55, 1.90, 0.22, 0.24, 0.20)
        b.box(DARK, mx, 0.35, 1.90, 0.12, 0.40, 0.12)
        b.box(WHITE, mx, 0.12, 1.90, 0.45, 0.06, 0.45)
    # Entrance mat.
    b.box(DARK, 0, 0.075, 2.05, 1.70, 0.02, 0.95)

    # --- Exterior props: planters, bench, shopping cart ---
    for px in (-2.30, 2.30):
        b.box(TRIM, px, 0.30, 3.55, 0.75, 0.55, 0.60)
        b.box(COOL, px, 0.62, 3.55, 0.62, 0.20, 0.48)
    b.box(WARM, 0.0, 0.42, 3.55, 1.50, 0.10, 0.55)
    for bx in (-0.60, 0.60):
        b.box(TRIM, bx, 0.20, 3.55, 0.10, 0.40, 0.45)
    b.box(STEEL, 1.75, 0.25, 3.35, 0.55, 0.06, 0.40)
    b.box(STEEL, 1.75, 0.55, 3.35, 0.55, 0.55, 0.06)
    b.box(STEEL, 1.50, 0.55, 3.35, 0.06, 0.55, 0.40)
    b.box(STEEL, 2.00, 0.55, 3.35, 0.06, 0.55, 0.40)

    materials = [
        {"name": "mall_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "mall_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.34, 0.34, 0.36, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.82}},
        {"name": "mall_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.13, 0.15, 1],
                                  "metallicFactor": 0.25, "roughnessFactor": 0.5}},
        {"name": "mall_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.26, 0.27, 0.29, 1],
                                  "metallicFactor": 0.08, "roughnessFactor": 0.75}},
        {"name": "mall_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.52, 0.50, 0.47, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.7}},
        {"name": "mall_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.03, 0.04, 1],
                                  "metallicFactor": 0.45, "roughnessFactor": 0.2}},
        {"name": "mall_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.07, 0.05, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.5}},
        {"name": "mall_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.06, 0.15, 0.42, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.5}},
        {"name": "mall_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.44, 0.47, 1],
                                  "metallicFactor": 0.6, "roughnessFactor": 0.35}},
        {"name": "mall_warm",
         "pbrMetallicRoughness": {"baseColorFactor": [0.72, 0.42, 0.08, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.6}},
        {"name": "mall_cool",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.42, 0.38, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.6}},
        {"name": "mall_cloth",
         "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.10, 0.34, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "mall_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.82, 0.81, 0.77, 1],
                                  "metallicFactor": 0.04, "roughnessFactor": 0.6}},
        {"name": "mall_dark",
         "pbrMetallicRoughness": {"baseColorFactor": [0.09, 0.09, 0.10, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.85}},
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
        "asset": {"version": "2.0", "generator": "neon rv mall generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Mall", "mesh": 0}],
        "meshes": [{"name": "RV_Mall", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_mall.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_mall.bin").write_bytes(buffer)
    (OUT / "rv_mall.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_mall.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"mall bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
