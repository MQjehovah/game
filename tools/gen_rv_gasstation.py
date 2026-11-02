#!/usr/bin/env python3
"""Generate rv_gasstation.gltf: an enterable roadside gas station.

The shop uses the same solid-wall approach as rv_store (visible from inside and
outside, 1.6 m doorway at +Z) so `collidePoiWalls` and the interior loot slots
work unchanged. A four-post canopy with two pumps sits to the west of the shop.

The shop interior carries higher-fidelity props: a sales counter with a cash
register, a four-shelf product wall with price strips, a glass-front drink
cooler, a coffee machine with cups and condiments, a central snack gondola, a
hanging sign and a wall clock.
"""
import json
import struct
from pathlib import Path

from rv_atlas import atlas_extras, product_material, tile_uvs

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_gasstation.json"


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
    (WALL, TRIM, ROOF, FLOOR, GLASS, RED, WHITE, STEEL,
     WARM, GREEN, BLUE, YELLOW, WOOD, PRODUCT) = range(1, 15)

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shop shell (centred at the prefab origin) ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 2.55, 0, 6.5, 0.18, 5.5)
    b.box(WALL, 0, 1.25, -2.52, 6.2, 2.5, 0.16)
    b.box(WALL, -3.02, 1.25, 0, 0.16, 2.5, 5.2)
    b.box(WALL, 3.02, 1.25, 0, 0.16, 2.5, 5.2)
    # Front wall split around a 1.6 m doorway and two display windows.
    for bay in (-1, 1):
        cx = bay * 1.95
        b.box(WALL, cx, 0.45, 2.52, 2.30, 0.90, 0.16)
        b.box(WALL, cx, 2.25, 2.52, 2.30, 0.50, 0.16)
        b.box(WALL, cx + bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)
        b.box(WALL, cx - bay * 1.00, 1.45, 2.52, 0.30, 1.10, 0.16)
        b.box(TRIM, cx, 1.95, 2.55, 1.78, 0.08, 0.22)
        b.box(TRIM, cx, 0.95, 2.55, 1.78, 0.08, 0.22)
        b.box(TRIM, cx - 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(TRIM, cx + 0.85, 1.45, 2.55, 0.08, 1.10, 0.22)
        b.box(STEEL, cx, 0.92, 2.62, 1.80, 0.05, 0.10)
        # The pane itself is a separate breakable entity (rv_window_glass).
        # Window display: a lit shelf with a few products behind the glass.
        b.box(TRIM, cx, 1.05, 2.30, 1.70, 0.06, 0.42)
        for k, (dx, wy, wh) in enumerate(((-0.55, 1.22, 0.28),
                                          (-0.18, 1.20, 0.24),
                                          (0.20, 1.22, 0.28),
                                          (0.55, 1.18, 0.20))):
            b.box(PRODUCT, cx + dx, wy, 2.30, 0.22, wh, 0.22,
                  uv=tile_uvs((k + (0 if bay < 0 else 4)) % 16))
    b.box(WALL, 0, 2.30, 2.52, 1.60, 0.40, 0.16)
    b.box(TRIM, -0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(TRIM, 0.88, 1.22, 2.56, 0.18, 2.44, 0.26)
    b.box(TRIM, 0, 2.40, 2.56, 1.94, 0.20, 0.26)
    # Roof fascia + red GAS band.
    b.box(TRIM, 0, 2.66, 2.72, 6.6, 0.30, 0.16)
    b.box(RED, 0, 2.72, 2.82, 1.60, 0.34, 0.05)
    b.box(WHITE, 0, 2.72, 2.86, 1.10, 0.16, 0.02)

    # --- Interior fixtures and merchandise ---
    # Sales counter (right) with cash register + receipt roll.
    b.box(TRIM, 1.55, 0.44, -0.80, 1.90, 0.88, 0.70)
    b.box(WHITE, 1.55, 0.90, -0.80, 1.98, 0.06, 0.78)
    b.box(STEEL, 1.55, 0.12, -0.80, 1.82, 0.16, 0.62)
    b.box(STEEL, 1.85, 1.10, -0.80, 0.46, 0.34, 0.40)
    b.box(TRIM, 1.85, 1.13, -0.58, 0.36, 0.20, 0.03)
    b.box(YELLOW, 1.85, 1.10, -0.56, 0.30, 0.10, 0.02)
    b.box(STEEL, 1.85, 1.38, -0.82, 0.10, 0.24, 0.10)
    b.box(TRIM, 1.85, 1.52, -0.80, 0.42, 0.26, 0.04)
    b.box(BLUE, 1.85, 1.52, -0.77, 0.34, 0.18, 0.02)
    b.box(WHITE, 2.00, 1.02, -0.62, 0.12, 0.10, 0.12)

    # Back-wall shelving: dark frame, four cream shelves, product rows.
    b.box(TRIM, -1.50, 1.05, -2.24, 2.70, 2.10, 0.30)
    for k in range(4):
        sy = 0.30 + k * 0.50
        b.box(WHITE, -1.50, sy, -2.06, 2.55, 0.05, 0.38)
        b.box(STEEL, -1.50, sy + 0.04, -1.86, 2.57, 0.04, 0.04)
    for row in range(3):
        sy = 0.30 + row * 0.50 + 0.17
        for i in range(9):
            px = -2.55 + i * 0.26
            b.box(PRODUCT, px, sy, -2.02, 0.16, 0.24, 0.20,
                  uv=tile_uvs((i * 2 + row * 5 + 3) % 16))
    b.box(WHITE, -1.50, 2.10, -2.06, 2.40, 0.10, 0.02)

    # Glass-front drink cooler (left wall, near the door).
    b.box(STEEL, -2.60, 1.20, 0.90, 0.55, 2.20, 1.30)
    b.box(GLASS, -2.32, 1.28, 0.90, 0.03, 1.90, 1.10)
    for row in range(4):
        cy = 0.42 + row * 0.46
        b.box(STEEL, -2.55, cy, 0.90, 0.40, 0.04, 1.18)
        for i in range(5):
            cz = 0.38 + i * 0.22
            b.box(BLUE if (i + row) % 2 == 0 else GREEN,
                  -2.51, cy + 0.14, cz, 0.24, 0.20, 0.16)
    b.box(WHITE, -2.60, 2.24, 0.90, 0.60, 0.20, 0.06)

    # Coffee station (right wall): machine, cup stack and condiment rack.
    b.box(TRIM, 2.55, 0.55, 0.55, 0.55, 1.10, 1.30)
    b.box(WHITE, 2.55, 1.12, 0.55, 0.62, 0.06, 1.38)
    b.box(STEEL, 2.45, 1.30, 0.35, 0.34, 0.46, 0.40)
    b.box(RED, 2.45, 1.34, 0.12, 0.26, 0.18, 0.12)
    b.box(TRIM, 2.45, 1.12, 0.12, 0.30, 0.06, 0.20)
    for k in range(3):
        b.box(WHITE, 2.62, 1.20 + k * 0.10, 0.75, 0.14, 0.09, 0.14)
    b.box(STEEL, 2.55, 1.42, 1.05, 0.40, 0.10, 0.50)
    for i in range(4):
        b.box((RED, WARM, WHITE, YELLOW)[i], 2.45 + (i % 2) * 0.18,
              1.52, 0.92 + (i // 2) * 0.18, 0.10, 0.16, 0.10)

    # Central snack gondola with products on both sides.
    b.box(STEEL, -0.40, 0.55, 0.10, 1.70, 0.06, 0.80)
    b.box(STEEL, -0.40, 1.00, 0.10, 1.70, 0.06, 0.80)
    for ex in (-1.20, 0.40):
        b.box(STEEL, ex, 0.50, 0.10, 0.06, 1.00, 0.76)
    for i in range(6):
        px = -1.05 + i * 0.26
        b.box(PRODUCT, px, 0.66, -0.18, 0.16, 0.20, 0.16,
              uv=tile_uvs((i * 3 + 2) % 16))
        b.box(PRODUCT, px, 0.66, 0.38, 0.16, 0.20, 0.16,
              uv=tile_uvs((i * 3 + 5) % 16))
        b.box(PRODUCT, px, 1.11, -0.18, 0.16, 0.20, 0.16,
              uv=tile_uvs((i * 3 + 8) % 16))
        b.box(PRODUCT, px, 1.11, 0.38, 0.16, 0.20, 0.16,
              uv=tile_uvs((i * 3 + 11) % 16))

    # Hanging price sign over the counter + wall clock.
    b.box(TRIM, 1.55, 2.16, -0.10, 1.10, 0.30, 0.04)
    b.box(WHITE, 1.55, 2.16, -0.07, 0.92, 0.18, 0.02)
    b.box(TRIM, -2.60, 2.10, 1.70, 0.34, 0.34, 0.04)
    b.box(WHITE, -2.60, 2.10, 1.73, 0.26, 0.26, 0.02)

    # Shopping baskets + cardboard box by the door.
    for k in range(3):
        b.box(RED, 2.35, 0.12 + k * 0.14, 1.85, 0.55, 0.12, 0.40)
    b.box(WOOD, -2.20, 0.22, 2.00, 0.55, 0.44, 0.50)

    # Mat and floor strip.
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
        # glTF material index 0 is treated as the no-material default by the
        # engine's importer, so keep a dummy here and start real materials at 1.
        {"name": "gas_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
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
        {"name": "gas_steel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.44, 0.46, 0.49, 1],
                                  "metallicFactor": 0.6, "roughnessFactor": 0.35}},
        {"name": "gas_warm",
         "pbrMetallicRoughness": {"baseColorFactor": [0.74, 0.40, 0.08, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        {"name": "gas_green",
         "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.44, 0.20, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        {"name": "gas_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.24, 0.50, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "gas_yellow",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.66, 0.14, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "gas_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.40, 0.28, 0.16, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        product_material("gas_product"),
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
    gltf.update(atlas_extras())
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
