#!/usr/bin/env python3
"""Generate rv_cabin.gltf: an enterable lakeside/forest cabin.

Keeps the shared 6.2 x 5.2 POI footprint for collision/loot compatibility.
Adds a log cabin with gable roof, covered porch, fireplace, bed, table/chairs,
kitchen counter and shelves.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_cabin.json"


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

    def poly(self, mat, quad):
        if len(quad) not in (3, 4):
            raise ValueError("poly needs 3 or 4 verts")
        ax = [quad[1][i] - quad[0][i] for i in range(3)]
        bx = [quad[2][i] - quad[0][i] for i in range(3)]
        n = (ax[1] * bx[2] - ax[2] * bx[1],
             ax[2] * bx[0] - ax[0] * bx[2],
             ax[0] * bx[1] - ax[1] * bx[0])
        ln = math.sqrt(sum(c * c for c in n))
        if ln < 1e-8:
            return
        n = tuple(c / ln for c in n)
        positions, normals, indices = self._group(mat)
        base = len(positions)
        for q in quad:
            positions.append(q)
            normals.append(n)
        indices.extend([base, base + 1, base + 2])
        if len(quad) == 4:
            indices.extend([base, base + 2, base + 3])


def main():
    b = MeshBuilder()
    (WALL, TRIM, ROOF, FLOOR, GLASS, STONE, WHITE, WOOD,
     BLUE, RED, WARM) = range(1, 12)
    H = 2.85

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shell ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, -1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 0, 2.55, 2.52, 1.60, 0.60, 0.16)
    # Gable roof + gable ends.
    b.poly(ROOF, [(-3.45, 2.85, 3.00), (0, 4.25, 3.00),
                  (0, 4.25, -3.00), (-3.45, 2.85, -3.00)])
    b.poly(ROOF, [(3.45, 2.85, 3.00), (3.45, 2.85, -3.00),
                  (0, 4.25, -3.00), (0, 4.25, 3.00)])
    b.poly(WALL, [(-3.10, 2.85, 2.60), (3.10, 2.85, 2.60), (0, 4.25, 2.60)])
    b.poly(WALL, [(3.10, 2.85, -2.60), (-3.10, 2.85, -2.60), (0, 4.25, -2.60)])
    b.box(TRIM, 0, 4.25, 0, 0.18, 0.14, 6.0)
    # Eave trim.
    b.box(TRIM, -3.43, 2.82, 0, 0.10, 0.12, 6.0)
    b.box(TRIM, 3.43, 2.82, 0, 0.10, 0.12, 6.0)

    # --- Front door, windows, covered porch ---
    b.box(TRIM, -0.88, 1.15, 2.56, 0.18, 2.30, 0.24)
    b.box(TRIM, 0.88, 1.15, 2.56, 0.18, 2.30, 0.24)
    b.box(TRIM, 0, 2.28, 2.56, 1.94, 0.18, 0.24)
    for sx in (-2.10, 2.10):
        b.box(GLASS, sx, 1.55, 2.61, 1.20, 0.90, 0.04)
        b.box(TRIM, sx, 1.00, 2.62, 1.30, 0.08, 0.06)
        b.box(TRIM, sx, 2.10, 2.62, 1.30, 0.08, 0.06)
        b.box(TRIM, sx - 0.62, 1.55, 2.62, 0.08, 1.18, 0.06)
        b.box(TRIM, sx + 0.62, 1.55, 2.62, 0.08, 1.18, 0.06)
    # Porch deck, roof and posts.
    b.box(WOOD, 0, 0.10, 3.35, 5.00, 0.12, 1.60)
    b.box(TRIM, 0, 2.48, 3.55, 5.00, 0.10, 1.80)
    for px in (-2.20, 2.20):
        b.box(TRIM, px, 1.30, 4.20, 0.16, 2.40, 0.16)
    b.box(WOOD, 0, 0.14, 4.35, 1.30, 0.10, 0.55)

    # --- Fireplace / chimney (back-left) ---
    b.box(STONE, -1.55, 1.05, -2.24, 1.90, 2.10, 0.52)
    b.box(TRIM, -1.55, 0.55, -1.94, 1.10, 0.90, 0.10)
    b.box(WARM, -1.55, 0.32, -1.90, 0.72, 0.16, 0.08)
    b.box(STONE, -1.55, 3.30, -2.24, 0.70, 1.00, 0.45)

    # --- Bed, table, chairs, kitchen counter, shelves ---
    b.box(WOOD, 1.95, 0.28, -1.25, 1.30, 0.30, 2.10)
    b.box(WHITE, 1.95, 0.48, -1.25, 1.20, 0.12, 1.90)
    b.box(BLUE, 1.95, 0.57, -1.55, 1.10, 0.08, 1.20)
    b.box(WHITE, 1.95, 0.62, -2.08, 0.80, 0.22, 0.38)
    b.box(WOOD, 1.95, 1.05, -2.22, 1.34, 0.90, 0.08)
    for bx in (1.36, 2.54):
        b.box(WOOD, bx, 0.55, -2.18, 0.10, 1.10, 0.10)
    # Table + chairs (center-left).
    b.box(WOOD, -0.55, 0.72, 0.45, 1.70, 0.10, 1.00)
    for tx in (-1.25, 0.15):
        for tz in (0.05, 0.85):
            b.box(WOOD, tx, 0.36, tz, 0.10, 0.72, 0.10)
    b.box(WOOD, -0.55, 0.45, 1.40, 0.55, 0.08, 0.55)
    b.box(WOOD, -0.55, 0.85, 1.62, 0.55, 0.70, 0.08)
    b.box(WOOD, -0.55, 0.45, -0.55, 0.55, 0.08, 0.55)
    b.box(WOOD, -0.55, 0.85, -0.77, 0.55, 0.70, 0.08)
    # Kitchen counter along the right wall.
    b.box(WOOD, 2.55, 0.45, 1.45, 0.70, 0.90, 1.60)
    b.box(WHITE, 2.55, 0.93, 1.45, 0.78, 0.06, 1.68)
    b.box(TRIM, 2.55, 0.28, 1.45, 0.72, 0.18, 1.62)
    b.box(STONE, 2.55, 1.02, 1.05, 0.50, 0.12, 0.44)
    # Shelves on the left wall.
    for shelf_y in (0.75, 1.35, 1.95):
        b.box(WOOD, -2.72, shelf_y, 1.10, 0.28, 0.07, 2.20)
    for i in range(6):
        b.box((WARM if i % 2 == 0 else RED), -2.66, 0.93 + (i // 3) * 0.60,
              0.35 + (i % 3) * 0.65, 0.18, 0.28, 0.18)
    # Rug.
    b.box(RED, 0.20, 0.075, -1.10, 1.70, 0.02, 1.10)
    # Interior wall lining (light) so the log interior is readable.
    b.box(WHITE, -2.88, 1.42, 0, 0.04, 2.84, 5.00)
    b.box(WHITE, 2.88, 1.42, 0, 0.04, 2.84, 5.00)
    b.box(WHITE, 0, 1.42, -2.40, 6.00, 2.84, 0.04)
    b.box(WHITE, 1.95, 1.42, 2.38, 2.20, 2.84, 0.04)
    b.box(WHITE, -1.95, 1.42, 2.38, 2.20, 2.84, 0.04)
    b.box(WHITE, 0, 2.72, 0, 5.80, 0.04, 4.50)

    # --- Exterior props: wood pile, barrel, fish crate, lantern post ---
    for i in range(4):
        b.box(WOOD, -3.80, 0.18 + i * 0.22, 2.30, 1.10, 0.18, 0.35)
    b.box(TRIM, 3.70, 0.35, 2.30, 0.46, 0.70, 0.46)
    b.box(WOOD, 3.70, 0.72, 2.30, 0.52, 0.06, 0.52)
    b.box(BLUE, -3.80, 0.30, 3.55, 0.80, 0.45, 0.55)
    b.box(TRIM, 3.90, 1.30, 3.70, 0.12, 2.60, 0.12)
    b.box(WARM, 3.90, 2.55, 3.70, 0.28, 0.28, 0.28)

    materials = [
        {"name": "cabin_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "cabin_log",
         "pbrMetallicRoughness": {"baseColorFactor": [0.22, 0.11, 0.05, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "cabin_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.07, 0.05, 0.04, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.85}},
        {"name": "cabin_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.14, 0.12, 0.10, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.9}},
        {"name": "cabin_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.12, 0.06, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "cabin_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.03, 0.04, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.25}},
        {"name": "cabin_stone",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.30, 0.31, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.95}},
        {"name": "cabin_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.72, 0.70, 0.65, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.8}},
        {"name": "cabin_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.18, 0.09, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "cabin_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.18, 0.32, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "cabin_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.38, 0.07, 0.05, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "cabin_warm",
         "pbrMetallicRoughness": {"baseColorFactor": [0.60, 0.34, 0.08, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.7}},
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
        "asset": {"version": "2.0", "generator": "neon rv cabin generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Cabin", "mesh": 0}],
        "meshes": [{"name": "RV_Cabin", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_cabin.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_cabin.bin").write_bytes(buffer)
    (OUT / "rv_cabin.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_cabin.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"cabin bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
