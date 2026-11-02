#!/usr/bin/env python3
"""Generate rv_store.gltf: an enterable roadside store.

The mesh is built from solid boxes (walls have thickness, so they are visible
from both outside and inside) around a 1.6 m front doorway. It replaces the
cube-scaled ruin shell for the "废宅/商店" POI so the building reads as real
architecture instead of a scaled-cube prototype.

The interior now carries higher-fidelity props: a cash register + receipt roll
on the sales counter, a four-shelf merchandise wall with rows of coloured
product boxes and price strips, a glass-front drink cooler, a produce bin, a
central gondola, a wall clock, a hanging sign and a stack of shopping baskets.
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
    (CREAM, DARK, ROOF, FLOOR, RED, STEEL, GLASS, WHITE,
     WARM, GREEN, BLUE, YELLOW, WOOD) = range(1, 14)

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # Shell.
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 2.55, 0, 6.5, 0.18, 5.5)
    b.box(CREAM, 0, 1.25, -2.52, 6.2, 2.5, 0.16)
    b.box(CREAM, -3.02, 1.25, 0, 0.16, 2.5, 5.2)
    b.box(CREAM, 3.02, 1.25, 0, 0.16, 2.5, 5.2)
    # Front wall with a 1.6 m doorway and two large display windows. The wall is
    # split around each window so the shop can be seen from the street (the
    # display panes are broken, matching the abandoned theme).
    # Left bay: x -3.1..-0.8. Right bay: x 0.8..3.1.
    for bay in (-1, 1):
        cx = bay * 1.95
        b.box(CREAM, cx, 0.45, 2.52, 2.30, 0.90, 0.16)   # sill wall
        b.box(CREAM, cx, 2.25, 2.52, 2.30, 0.50, 0.16)   # header
        b.box(CREAM, cx + bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)  # outer pier
        b.box(CREAM, cx - bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)  # inner pier
        # Window frame + sill + a couple of glass shards.
        b.box(DARK, cx, 1.95, 2.55, 1.78, 0.08, 0.22)
        b.box(DARK, cx, 0.95, 2.55, 1.78, 0.08, 0.22)
        b.box(DARK, cx - 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(DARK, cx + 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(STEEL, cx, 0.92, 2.62, 1.80, 0.05, 0.10)
        for k in range(3):
            sx = cx - 0.60 + k * 0.60
            b.box(GLASS, sx, 1.50 + (k % 2) * 0.10, 2.52, 0.30, 0.70, 0.02)
        # Window display: a lit shelf with a few products behind the glass.
        b.box(DARK, cx, 1.05, 2.30, 1.70, 0.06, 0.42)
        b.box(WARM, cx - 0.55, 1.22, 2.30, 0.22, 0.28, 0.22)
        b.box(GREEN, cx - 0.18, 1.20, 2.30, 0.22, 0.24, 0.22)
        b.box(BLUE, cx + 0.20, 1.22, 2.30, 0.22, 0.28, 0.22)
        b.box(YELLOW, cx + 0.55, 1.18, 2.30, 0.22, 0.20, 0.22)
    b.box(CREAM, 0, 2.30, 2.52, 1.60, 0.40, 0.16)

    # Door frame + header + sign/awning.
    b.box(DARK, -0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(DARK, 0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(DARK, 0, 2.40, 2.56, 1.94, 0.20, 0.26)
    b.box(DARK, 0, 2.68, 3.00, 2.40, 0.08, 0.92)
    b.box(RED, 0, 2.88, 2.63, 2.40, 0.42, 0.08)
    b.box(RED, -1.55, 2.72, 3.00, 0.10, 0.24, 0.86)
    b.box(RED, 1.55, 2.72, 3.00, 0.10, 0.24, 0.86)

    # ---- Interior fixtures and merchandise ----
    # Sales counter (right) with a cash register and receipt roll.
    b.box(DARK, 1.50, 0.44, -0.80, 2.00, 0.88, 0.70)
    b.box(CREAM, 1.50, 0.90, -0.80, 2.08, 0.06, 0.78)
    b.box(STEEL, 1.50, 0.12, -0.80, 1.92, 0.16, 0.62)
    b.box(STEEL, 1.85, 1.10, -0.80, 0.46, 0.34, 0.40)
    b.box(DARK, 1.85, 1.13, -0.58, 0.36, 0.20, 0.03)
    b.box(YELLOW, 1.85, 1.10, -0.56, 0.30, 0.10, 0.02)
    b.box(STEEL, 1.85, 1.38, -0.82, 0.10, 0.24, 0.10)
    b.box(DARK, 1.85, 1.52, -0.80, 0.42, 0.26, 0.04)
    b.box(BLUE, 1.85, 1.52, -0.77, 0.34, 0.18, 0.02)
    b.box(WHITE, 2.00, 1.02, -0.62, 0.12, 0.10, 0.12)

    # Back-wall shelving: dark frame, four cream shelves, product rows.
    b.box(DARK, -1.50, 1.05, -2.24, 2.70, 2.10, 0.30)
    for k in range(4):
        sy = 0.30 + k * 0.50
        b.box(CREAM, -1.50, sy, -2.06, 2.55, 0.05, 0.38)
        b.box(STEEL, -1.50, sy + 0.04, -1.86, 2.57, 0.04, 0.04)
    colors = (WARM, GREEN, BLUE, YELLOW, WHITE, RED)
    for row in range(3):
        sy = 0.30 + row * 0.50 + 0.17
        for i in range(9):
            px = -2.55 + i * 0.26
            c = colors[(i + row) % len(colors)]
            b.box(c, px, sy, -2.02, 0.16, 0.24, 0.20)
    # Price-tag strip along the top shelf.
    b.box(WHITE, -1.50, 2.10, -2.06, 2.40, 0.10, 0.02)

    # Tall glass-front drink cooler (left back corner).
    b.box(STEEL, -2.55, 1.20, -1.10, 0.55, 2.20, 1.30)
    b.box(GLASS, -2.27, 1.28, -1.10, 0.03, 1.90, 1.10)
    for row in range(4):
        cy = 0.42 + row * 0.46
        b.box(STEEL, -2.50, cy, -1.10, 0.40, 0.04, 1.18)
        for i in range(5):
            cz = -1.62 + i * 0.22
            b.box(BLUE if (i + row) % 2 == 0 else GREEN,
                  -2.46, cy + 0.14, cz, 0.24, 0.20, 0.16)
    b.box(WHITE, -2.55, 2.24, -1.10, 0.60, 0.20, 0.06)

    # Produce bin (front left): wood crate + green/orange goods.
    b.box(WOOD, -2.35, 0.26, 0.75, 0.90, 0.50, 0.80)
    b.box(GREEN, -2.35, 0.52, 0.60, 0.80, 0.18, 0.34)
    b.box(WARM, -2.35, 0.52, 0.95, 0.80, 0.16, 0.30)

    # Central low gondola shelf with products on both sides.
    b.box(STEEL, -0.55, 0.55, 0.10, 1.70, 0.06, 0.80)
    b.box(STEEL, -0.55, 1.00, 0.10, 1.70, 0.06, 0.80)
    for ex in (-1.35, 0.25):
        b.box(STEEL, ex, 0.50, 0.10, 0.06, 1.00, 0.76)
    for i in range(6):
        px = -1.20 + i * 0.26
        b.box(colors[i % len(colors)], px, 0.66, -0.18, 0.16, 0.20, 0.16)
        b.box(colors[(i + 3) % len(colors)], px, 0.66, 0.38, 0.16, 0.20, 0.16)
        b.box(colors[(i + 1) % len(colors)], px, 1.11, -0.18, 0.16, 0.20, 0.16)
        b.box(colors[(i + 4) % len(colors)], px, 1.11, 0.38, 0.16, 0.20, 0.16)

    # Hanging price sign over the counter + wall clock.
    b.box(DARK, 1.50, 2.16, -0.20, 1.10, 0.30, 0.04)
    b.box(WHITE, 1.50, 2.16, -0.17, 0.92, 0.18, 0.02)
    b.box(DARK, -2.60, 2.10, 1.10, 0.34, 0.34, 0.04)
    b.box(WHITE, -2.60, 2.10, 1.13, 0.26, 0.26, 0.02)

    # Shopping baskets stacked by the door + a cardboard box.
    for k in range(3):
        b.box(RED, 2.35, 0.12 + k * 0.14, 1.85, 0.55, 0.12, 0.40)
    b.box(WOOD, -2.20, 0.22, 2.00, 0.55, 0.44, 0.50)

    # Rug + door mat.
    b.box(CREAM, 0, 0.075, 1.72, 1.90, 0.02, 1.10)
    b.box(DARK, 0, 0.085, 2.86, 1.70, 0.02, 0.60)

    materials = [
        # glTF material index 0 is treated as the no-material default by the
        # engine's importer, so keep a dummy here and start real materials at 1.
        {"name": "store_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
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
        {"name": "store_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.44, 0.46, 0.49, 1],
                                  "metallicFactor": 0.6, "roughnessFactor": 0.35}},
        {"name": "store_glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.03, 0.06, 0.08, 1],
                                  "metallicFactor": 0.45, "roughnessFactor": 0.15}},
        {"name": "store_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.85, 0.80, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "store_warm",
         "pbrMetallicRoughness": {"baseColorFactor": [0.74, 0.40, 0.08, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        {"name": "store_green",
         "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.44, 0.20, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        {"name": "store_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.24, 0.50, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "store_yellow",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.66, 0.14, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "store_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.40, 0.28, 0.16, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
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
