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

from rv_atlas import atlas_extras, product_material, tile_uvs

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_clinic.json"


class MeshBuilder:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], [], []))

    def box(self, mat, cx, cy, cz, sx, sy, sz, uv=None):
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
        positions, normals, indices, uvs = self._group(mat)
        if uv is not None:
            uvs.extend(uv)
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
     FABRIC, WOOD, TEAL, AMBER, BLUE, GREEN, PRODUCT) = range(1, 16)
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
    # Front wall split around the doorway and two display windows.
    for bay in (-1, 1):
        ccx = bay * 1.95
        b.box(WALL, ccx, 0.45, 2.52, 2.30, 0.90, 0.16)               # sill wall
        b.box(WALL, ccx, 2.50, 2.52, 2.30, 1.00, 0.16)               # header
        b.box(WALL, ccx + bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)  # outer pier
        b.box(WALL, ccx - bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)  # inner pier
        # Window frame (the breakable pane is a dynamic entity).
        b.box(STEEL, ccx, 1.95, 2.55, 1.78, 0.08, 0.22)
        b.box(STEEL, ccx, 0.95, 2.55, 1.78, 0.08, 0.22)
        b.box(STEEL, ccx - 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(STEEL, ccx + 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(STEEL, ccx, 0.92, 2.62, 1.80, 0.05, 0.10)
    b.box(WALL, 0, 2.72, 2.52, 1.60, 0.56, 0.16)
    # Teal wainscot band on the front.
    b.box(TEAL, 0, 0.55, 2.61, 6.0, 0.90, 0.03)
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

    # --- Interior: reception, waiting chairs, exam area, medicine wall ---
    # Reception counter (right) with monitor, keyboard, phone and paperwork.
    b.box(WOOD, 1.55, 0.55, -0.60, 2.10, 1.10, 0.70)
    b.box(WHITE, 1.55, 1.13, -0.60, 2.18, 0.06, 0.78)
    b.box(STEEL, 1.55, 0.10, -0.60, 2.02, 0.16, 0.62)
    b.box(STEEL, 1.55, 1.35, -0.88, 0.10, 0.30, 0.10)
    b.box(TRIM, 1.55, 1.52, -0.80, 0.44, 0.26, 0.04)
    b.box(BLUE, 1.55, 1.52, -0.77, 0.36, 0.18, 0.02)
    b.box(TRIM, 1.55, 1.18, -0.45, 0.44, 0.03, 0.16)
    b.box(TRIM, 1.98, 1.18, -0.58, 0.16, 0.06, 0.22)
    b.box(WHITE, 1.20, 1.16, -0.45, 0.20, 0.02, 0.28)
    b.box(STEEL, 1.55, 0.34, -1.10, 0.40, 0.10, 0.40)
    # Reception sign (teal plate + red cross) on the back wall.
    b.box(TEAL, 1.55, 2.34, -2.44, 1.70, 0.28, 0.04)
    b.box(WHITE, 1.55, 2.34, -2.41, 1.20, 0.14, 0.02)
    b.box(RED, 2.20, 2.34, -2.41, 0.14, 0.20, 0.02)
    b.box(RED, 2.20, 2.34, -2.41, 0.34, 0.08, 0.02)

    # Waiting chairs along the left wall.
    for i in range(3):
        cz = -1.60 + i * 0.95
        b.box(STEEL, -2.30, 0.25, cz, 0.55, 0.10, 0.60)
        b.box(TEAL, -2.30, 0.60, cz + 0.26, 0.55, 0.60, 0.08)
        b.box(STEEL, -2.30, 0.45, cz, 0.08, 0.45, 0.60)

    # Exam table (back-right): padded top, sheet, pillow, paper roll.
    b.box(WHITE, 1.90, 0.62, -1.70, 1.20, 0.16, 1.45)
    b.box(STEEL, 1.90, 0.30, -1.70, 1.10, 0.50, 1.30)
    b.box(WHITE, 1.90, 0.74, -1.35, 1.08, 0.10, 0.80)
    b.box(STEEL, 1.90, 0.80, -2.30, 0.95, 0.16, 0.28)
    b.box(WHITE, 2.36, 0.80, -1.70, 0.10, 0.30, 0.34)

    # Overhead exam lamp on an articulated wall arm.
    b.box(STEEL, 2.60, 2.10, -1.70, 0.10, 0.90, 0.10)
    b.box(STEEL, 2.20, 2.10, -1.70, 0.90, 0.08, 0.08)
    b.box(STEEL, 1.80, 1.95, -1.70, 0.08, 0.40, 0.08)
    b.box(AMBER, 1.80, 1.72, -1.70, 0.34, 0.16, 0.34)
    b.box(WHITE, 1.80, 1.64, -1.70, 0.26, 0.04, 0.26)

    # Vital-signs monitor on a rolling stand.
    b.box(STEEL, 2.62, 0.55, -0.95, 0.06, 1.10, 0.06)
    b.box(STEEL, 2.62, 0.08, -0.95, 0.34, 0.05, 0.34)
    b.box(TRIM, 2.62, 1.20, -0.95, 0.34, 0.28, 0.10)
    b.box(BLUE, 2.62, 1.20, -0.89, 0.26, 0.18, 0.02)
    b.box(GREEN, 2.62, 1.20, -0.88, 0.18, 0.03, 0.01)

    # IV stand + bag, instrument tray on a cart.
    b.box(STEEL, 1.10, 0.70, -2.20, 0.05, 1.40, 0.05)
    b.box(STEEL, 1.10, 0.08, -2.20, 0.30, 0.05, 0.30)
    b.box(WHITE, 1.10, 1.42, -2.20, 0.16, 0.28, 0.10)
    b.box(STEEL, 1.20, 0.55, -1.05, 0.50, 0.06, 0.40)
    b.box(STEEL, 1.20, 0.28, -1.05, 0.06, 0.55, 0.06)
    for tx in (1.14, 1.20, 1.26):
        b.box(WHITE, tx, 0.62, -1.05, 0.03, 0.10, 0.12)

    # Medicine cabinet (back-left): glass-front shelves, labelled boxes.
    b.box(STEEL, -1.40, 1.05, -2.32, 2.20, 2.10, 0.26)
    for shelf_y in (0.55, 1.20, 1.85):
        b.box(WHITE, -1.40, shelf_y, -2.16, 2.00, 0.05, 0.34)
        b.box(WHITE, -1.40, shelf_y + 0.09, -2.03, 1.94, 0.05, 0.02)
    b.box(GLASS, -1.40, 1.30, -2.02, 1.96, 1.80, 0.02)
    med_colors = (TEAL, WHITE, RED, BLUE, GREEN, AMBER)
    for row in range(3):
        sy = 0.55 + row * 0.65 + 0.17
        for i in range(9):
            px = -2.20 + i * 0.22
            b.box(PRODUCT if (i + row) % 3 != 1 else med_colors[(i + row) % 6],
                  px, sy, -2.10, 0.14, 0.22, 0.18,
                  uv=tile_uvs((i * 2 + row * 5 + 1) % 16))
            b.box(WHITE, px, sy, -2.00, 0.12, 0.05, 0.01)
    # Red cross plate on the cabinet header.
    b.box(RED, -1.40, 2.22, -2.16, 0.42, 0.30, 0.03)
    b.box(WHITE, -1.40, 2.22, -2.13, 0.10, 0.26, 0.02)
    b.box(WHITE, -1.40, 2.22, -2.13, 0.26, 0.10, 0.02)

    # Privacy curtain rail + hanging curtain between exam and waiting area.
    b.box(STEEL, -0.55, 2.78, -0.55, 0.06, 0.06, 3.10)
    b.box(FABRIC, -0.55, 1.80, -1.05, 0.04, 1.90, 0.90)
    # "EXAM" sign on the curtain rail.
    b.box(WHITE, -0.55, 2.55, -1.30, 0.02, 0.16, 0.50)
    b.box(TEAL, -0.55, 2.55, -1.30, 0.03, 0.10, 0.38)
    # Wall clock above the waiting chairs.
    b.box(TRIM, -2.86, 2.10, -0.35, 0.04, 0.34, 0.34)
    b.box(WHITE, -2.83, 2.10, -0.35, 0.02, 0.26, 0.26)

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
        {"name": "clinic_amber",
         "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.46, 0.10, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "clinic_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.24, 0.50, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.55}},
        {"name": "clinic_green",
         "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.44, 0.20, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        product_material("clinic_product"),
    ]

    buffer = b""
    buffer_views = []
    accessors = []
    primitives = []
    for mat_index, (positions, normals, indices, uvs) in sorted(b.groups.items()):
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
        idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
        raw_chunks = [pos_bytes, nrm_bytes, idx_bytes]
        has_uv = len(uvs) == len(positions) and len(uvs) > 0
        if has_uv:
            raw_chunks.append(b"".join(struct.pack("<2f", *t) for t in uvs))
        for raw in raw_chunks:
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            buffer_views.append({"buffer": 0, "byteOffset": len(buffer),
                                 "byteLength": len(blob)})
            buffer += blob
        vb = len(buffer_views) - len(raw_chunks)
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
        attrs = {"POSITION": ai, "NORMAL": ai + 1}
        if has_uv:
            accessors.append({"bufferView": vb + 3, "componentType": 5126,
                              "count": len(uvs), "type": "VEC2"})
            attrs["TEXCOORD_0"] = len(accessors) - 1
        primitives.append({"attributes": attrs,
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
    gltf.update(atlas_extras())
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
