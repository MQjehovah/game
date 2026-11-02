#!/usr/bin/env python3
"""Generate rv_clinic.gltf: an enterable roadside clinic.

Keeps the shared 6.2 x 5.2 POI footprint for collision/loot compatibility.
Adds a white/teal clinic with a glass entrance, a red-cross sign, reception
counter, waiting chairs, exam table, medicine shelves, privacy curtain and a
few exterior props (bench, planter).
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_clinic.json"


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
    (WALL, TRIM, ROOF, FLOOR, GLASS, RED, STEEL, WHITE,
     FABRIC, WOOD, TEAL) = range(1, 12)
    H = 3.0

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shell: white clinic, flat roof, teal band ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 3.12, 0, 6.6, 0.20, 5.6)
    b.box(TRIM, 0, 3.28, 0, 6.8, 0.14, 5.8)
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, -1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 1.95, H * 0.5, 2.52, 2.30, H, 0.16)
    b.box(WALL, 0, 2.72, 2.52, 1.60, 0.56, 0.16)
    # Teal wainscot band on the front.
    b.box(TEAL, 0, 0.55, 2.61, 6.0, 0.90, 0.03)
    # Front glass either side of the doorway.
    for sx in (-2.10, 2.10):
        b.box(GLASS, sx, 1.75, 2.61, 1.50, 1.10, 0.04)
    # Door frame + double doors (opened outward).
    b.box(STEEL, -0.86, 1.32, 2.56, 0.16, 2.64, 0.26)
    b.box(STEEL, 0.86, 1.32, 2.56, 0.16, 2.64, 0.26)
    b.box(STEEL, 0, 2.68, 2.56, 1.88, 0.16, 0.26)
    for dx in (-1.18, 1.18):
        b.box(GLASS, dx, 1.30, 2.76, 0.72, 2.30, 0.05)
        b.box(STEEL, dx, 1.30, 2.79, 0.08, 2.34, 0.03)
    # Red cross sign above the entrance.
    b.box(RED, 0, 3.02, 2.64, 1.10, 0.34, 0.06)
    b.box(WHITE, 0, 3.02, 2.68, 0.30, 0.70, 0.02)
    b.box(WHITE, 0, 3.02, 2.69, 0.70, 0.30, 0.02)

    # --- Interior: reception, waiting chairs, exam area ---
    # Reception counter (right).
    b.box(WOOD, 1.55, 0.55, -0.60, 2.10, 1.10, 0.70)
    b.box(WHITE, 1.55, 1.13, -0.60, 2.18, 0.06, 0.78)
    b.box(STEEL, 1.55, 1.42, -0.85, 0.65, 0.50, 0.10)
    # Waiting chairs along the left wall.
    for i in range(3):
        cz = -1.60 + i * 0.95
        b.box(STEEL, -2.30, 0.25, cz, 0.55, 0.10, 0.60)
        b.box(TEAL, -2.30, 0.60, cz + 0.26, 0.55, 0.60, 0.08)
        b.box(STEEL, -2.30, 0.45, cz, 0.08, 0.45, 0.60)
    # Exam table (back-right) with a sheet + pillow.
    b.box(WHITE, 1.90, 0.62, -1.95, 1.20, 0.16, 2.00)
    b.box(STEEL, 1.90, 0.30, -1.95, 1.10, 0.50, 1.85)
    b.box(WHITE, 1.90, 0.74, -1.55, 1.08, 0.10, 1.10)
    b.box(STEEL, 1.90, 0.80, -2.65, 0.95, 0.16, 0.30)
    # Medicine shelves (back-left).
    b.box(STEEL, -1.40, 1.05, -2.32, 2.20, 2.10, 0.26)
    for shelf_y in (0.55, 1.20, 1.85):
        b.box(WHITE, -1.40, shelf_y, -2.16, 2.00, 0.05, 0.34)
    for i in range(9):
        px = -2.15 + (i % 3) * 0.72
        py = 0.66 + (i // 3) * 0.65
        b.box(TEAL if i % 2 == 0 else WHITE, px, py, -2.10, 0.26, 0.30, 0.20)
    # Privacy curtain rail + hanging curtain between exam and waiting area.
    b.box(STEEL, -0.55, 2.78, -0.55, 0.06, 0.06, 3.10)
    b.box(FABRIC, -0.55, 1.80, -1.05, 0.04, 1.90, 0.90)
    # Rug / mat.
    b.box(TEAL, 0, 0.075, 1.30, 1.60, 0.02, 0.90)
    # Interior lining so the room reads white from inside.
    b.box(WHITE, -2.88, 1.45, 0, 0.04, 2.90, 5.00)
    b.box(WHITE, 2.88, 1.45, 0, 0.04, 2.90, 5.00)
    b.box(WHITE, 0, 1.45, -2.40, 6.00, 2.90, 0.04)
    b.box(WHITE, 1.95, 1.45, 2.38, 2.20, 2.90, 0.04)
    b.box(WHITE, -1.95, 1.45, 2.38, 2.20, 2.90, 0.04)
    b.box(WHITE, 0, 2.86, 0, 5.80, 0.04, 4.50)

    # --- Exterior props: bench, planter, wheelchair-ish cart ---
    b.box(WOOD, -1.60, 0.42, 3.35, 1.60, 0.10, 0.50)
    for bx in (-2.30, -0.90):
        b.box(STEEL, bx, 0.20, 3.35, 0.10, 0.40, 0.45)
    for px in (2.10, -2.30):
        b.box(TRIM, px, 0.28, 3.45, 0.70, 0.50, 0.55)
        b.box(TEAL, px, 0.58, 3.45, 0.58, 0.18, 0.45)
    b.box(STEEL, 2.40, 0.28, 4.10, 0.50, 0.06, 0.40)
    b.box(STEEL, 2.40, 0.55, 4.28, 0.50, 0.50, 0.06)
    b.box(STEEL, 2.20, 0.55, 4.10, 0.06, 0.50, 0.40)
    b.box(STEEL, 2.60, 0.55, 4.10, 0.06, 0.50, 0.40)

    materials = [
        {"name": "clinic_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "clinic_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.82, 0.80, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.8}},
        {"name": "clinic_trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.22, 0.23, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.7}},
        {"name": "clinic_roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.26, 0.28, 0.30, 1],
                                  "metallicFactor": 0.08, "roughnessFactor": 0.75}},
        {"name": "clinic_floor",
         "pbrMetallicRoughness": {"baseColorFactor": [0.46, 0.47, 0.45, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.7}},
        {"name": "clinic_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.04, 0.05, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.2}},
        {"name": "clinic_red",
         "pbrMetallicRoughness": {"baseColorFactor": [0.58, 0.06, 0.05, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.55}},
        {"name": "clinic_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.46, 0.48, 0.50, 1],
                                  "metallicFactor": 0.6, "roughnessFactor": 0.35}},
        {"name": "clinic_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.87, 0.84, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.6}},
        {"name": "clinic_fabric",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.42, 0.44, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "clinic_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.40, 0.28, 0.16, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "clinic_teal",
         "pbrMetallicRoughness": {"baseColorFactor": [0.06, 0.34, 0.36, 1],
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
        "asset": {"version": "2.0", "generator": "neon rv clinic generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Clinic", "mesh": 0}],
        "meshes": [{"name": "RV_Clinic", "primitives": primitives}],
        "materials": materials,
        "buffers": [{"uri": "rv_clinic.bin", "byteLength": len(buffer)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (OUT / "rv_clinic.bin").write_bytes(buffer)
    (OUT / "rv_clinic.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_clinic.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(f"clinic bytes={len(buffer)} prims={len(primitives)}")


if __name__ == "__main__":
    main()
