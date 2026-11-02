#!/usr/bin/env python3
"""Generate rv_barn.gltf: an enterable farm barn.

Keeps the shared 6.2 x 5.2 POI footprint for collision/loot compatibility, adds
a gabled red barn, sliding doors, hayloft opening, stalls, hay bales, feed
trough and shelves. Roof planes/gable ends use custom triangles so the barn
reads as a real farm building instead of a scaled cube.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_barn.json"


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
        """Convex polygon (3 or 4 verts) with computed outward normal."""
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
    WALL, TRIM, ROOF, FLOOR, WOOD, HAY, DARK = range(1, 8)
    H = 3.15

    # Hidden dummy geometry using material 0. The engine's glTF importer
    # renders the first primitive with its default material, so keep that slot
    # occupied by something invisible and start visible parts at material 1.
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shell: red walls, floor, sliding bay, gable roof ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    # Front wall split around a 1.8 m doorway.
    b.box(WALL, -2.00, H * 0.5, 2.52, 2.10, H, 0.16)
    b.box(WALL, 2.00, H * 0.5, 2.52, 2.10, H, 0.16)
    b.box(WALL, 0, 2.83, 2.52, 1.90, 0.64, 0.16)
    # White door frame + hayloft opening.
    b.box(TRIM, -0.95, 1.30, 2.56, 0.18, 2.60, 0.22)
    b.box(TRIM, 0.95, 1.30, 2.56, 0.18, 2.60, 0.22)
    b.box(TRIM, 0, 2.62, 2.56, 2.08, 0.18, 0.22)
    b.box(DARK, 0, 2.92, 2.48, 1.30, 0.42, 0.06)
    # Sliding doors parked beside the opening (open state).
    for sx in (-2.15, 2.15):
        b.box(WALL, sx, 1.25, 2.66, 1.55, 2.35, 0.08)
        b.box(TRIM, sx, 0.10, 2.70, 1.62, 0.10, 0.06)
        b.box(TRIM, sx, 2.42, 2.70, 1.62, 0.10, 0.06)

    # Whitewashed interior lining (keeps the red exterior while lighting the
    # interior like a real barn).
    b.box(TRIM, -2.88, 1.55, 0, 0.04, 3.10, 5.00)
    b.box(TRIM, 2.88, 1.55, 0, 0.04, 3.10, 5.00)
    b.box(TRIM, 0, 1.55, -2.40, 6.00, 3.10, 0.04)
    b.box(TRIM, -1.95, 1.55, 2.38, 2.20, 3.10, 0.04)
    b.box(TRIM, 1.95, 1.55, 2.38, 2.20, 3.10, 0.04)
    b.box(TRIM, 0, 3.08, 0, 5.80, 0.04, 4.60)  # white ceiling liner

    # Gable roof: two planes + two gable-end triangles.
    b.poly(ROOF, [(-3.50, 3.15, 3.00), (0, 5.00, 3.00),
                  (0, 5.00, -3.00), (-3.50, 3.15, -3.00)])
    b.poly(ROOF, [(3.50, 3.15, 3.00), (3.50, 3.15, -3.00),
                  (0, 5.00, -3.00), (0, 5.00, 3.00)])
    b.poly(WALL, [(-3.10, 3.15, 2.60), (3.10, 3.15, 2.60), (0, 5.00, 2.60)])
    b.poly(WALL, [(3.10, 3.15, -2.60), (-3.10, 3.15, -2.60), (0, 5.00, -2.60)])
    # Ridge beam and white eave trim.
    b.box(TRIM, 0, 5.02, 0, 0.22, 0.16, 6.0)
    b.box(TRIM, -3.48, 3.10, 0, 0.10, 0.14, 6.0)
    b.box(TRIM, 3.48, 3.10, 0, 0.10, 0.14, 6.0)

    # --- Stalls (left/right) ---
    for sx in (-1.0, 1.0):
        for z in (-2.20, -0.10):
            b.box(WOOD, sx * 2.10, 0.70, z, 0.14, 1.40, 0.14)
            b.box(WOOD, sx * 1.10, 0.70, z, 0.14, 1.40, 0.14)
        b.box(WOOD, sx * 1.60, 1.12, -2.20, 1.10, 0.12, 0.14)
        b.box(WOOD, sx * 1.60, 1.12, -0.10, 1.10, 0.12, 0.14)
        b.box(WOOD, sx * 1.60, 0.60, -0.10, 1.10, 0.10, 0.10)
        b.box(WOOD, sx * 2.06, 0.55, -1.15, 0.10, 1.10, 2.10)
        b.box(HAY, sx * 1.60, 0.18, -1.20, 1.00, 0.30, 1.60)

    # --- Hay bales, trough, workbench, shelves, barrels ---
    b.box(HAY, 2.05, 0.24, 1.55, 1.00, 0.46, 0.66)
    b.box(HAY, 2.05, 0.70, 1.55, 0.92, 0.42, 0.60)
    b.box(HAY, 2.05, 0.24, 0.80, 1.00, 0.46, 0.66)
    b.box(DARK, 0.20, 0.22, 1.55, 1.50, 0.36, 0.52)
    b.box(WOOD, -1.55, 0.42, 1.35, 1.90, 0.16, 0.70)
    for lx in (-2.40, -0.70):
        b.box(WOOD, lx, 0.21, 1.35, 0.12, 0.42, 0.62)
    for shelf_y in (0.55, 1.15, 1.75):
        b.box(WOOD, 1.45, shelf_y, -2.32, 2.60, 0.08, 0.32)
    b.box(WOOD, 0.20, 0.42, -2.30, 0.55, 0.80, 0.50)
    b.box(WOOD, 2.55, 0.42, -1.95, 0.55, 0.80, 0.50)
    b.box(DARK, -2.55, 0.32, 2.05, 0.46, 0.62, 0.46)
    b.box(DARK, 2.55, 0.32, 2.05, 0.46, 0.62, 0.46)

    # --- Exterior: hay stack, trough, fence ---
    b.box(HAY, -3.85, 0.26, 3.55, 1.10, 0.50, 0.70)
    b.box(HAY, 3.80, 0.26, 3.50, 1.10, 0.50, 0.70)
    b.box(WOOD, -4.30, 0.25, 1.60, 1.40, 0.36, 0.55)
    for i in range(4):
        b.box(TRIM, -2.60 - i * 1.30, 0.65, 4.55, 0.12, 1.30, 0.12)
    b.box(TRIM, -4.55, 0.85, 4.55, 4.20, 0.10, 0.10)
    b.box(TRIM, -4.55, 0.42, 4.55, 4.20, 0.10, 0.10)

    materials = [
        # glTF material index 0 is treated as the no-material default by the
        # engine's importer, so keep a dummy here and start real materials at 1.
        {"name": "barn_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "barn_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.045, 0.03, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.9}},
        {"name": "barn_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.84, 0.81, 0.72, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.8}},
        {"name": "barn_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.09, 0.08, 1],
                                  "metallicFactor": 0.08, "roughnessFactor": 0.85}},
        {"name": "barn_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.27, 0.24, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.95}},
        {"name": "barn_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.30, 0.19, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "barn_hay",
         "pbrMetallicRoughness": {"baseColorFactor": [0.72, 0.62, 0.26, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "barn_dark",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.09, 0.08, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.9}},
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
        "asset": {"version": "2.0", "generator": "neon rv barn generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Barn", "mesh": 0}],
        "meshes": [{"name": "RV_Barn", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_barn.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_barn.bin").write_bytes(buffer)
    (OUT / "rv_barn.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_barn.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"barn bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
