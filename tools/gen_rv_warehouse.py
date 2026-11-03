#!/usr/bin/env python3
"""Generate rv_warehouse.gltf: an enterable logistics warehouse.

The shell keeps the shared 6.2 x 5.2 POI footprint so `collidePoiWalls` and the
interior loot slots work unchanged. It adds a taller shed, a wide loading-bay
door with an exterior apron/stripes/bollards, steel racks + pallets inside, and
higher-fidelity depot props: labelled pallet stacks with tape-striped cargo
boxes, a forklift, rack signage, a shipping desk with monitor/label printer, a
dock leveler and painted floor lanes.
"""
import json
import struct
from pathlib import Path

from rv_atlas import atlas_extras, product_material, tile_uvs

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_warehouse.json"


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
    (WALL, TRIM, ROOF, FLOOR, GLASS, YELLOW, STEEL, WOOD,
     RED, WHITE, BLUE, GREEN, ORANGE, PRODUCT) = range(1, 15)
    H = 3.6

    # Hidden material-0 primitive (the importer's first primitive uses the
    # default material; keep that slot invisible).
    b.box(0, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # --- Shed shell, wide loading-bay doorway at +Z ---
    b.box(FLOOR, 0, 0.03, 0, 6.2, 0.06, 5.2)
    b.box(ROOF, 0, 3.72, 0, 6.6, 0.22, 5.6)
    b.box(TRIM, 0, 3.90, 0, 6.8, 0.14, 5.8)   # parapet/edge
    b.box(WALL, 0, H * 0.5, -2.52, 6.2, H, 0.16)
    b.box(WALL, -3.02, H * 0.5, 0, 0.16, H, 5.2)
    b.box(WALL, 3.02, H * 0.5, 0, 0.16, H, 5.2)
    # Front wall split around the bay door and two display windows.
    for bay in (-1, 1):
        ccx = bay * 1.95
        b.box(WALL, ccx, 0.60, 2.52, 2.30, 1.20, 0.16)               # sill wall
        b.box(WALL, ccx, 3.10, 2.52, 2.30, 1.00, 0.16)               # header
        b.box(WALL, ccx + bay * 1.00, 1.90, 2.52, 0.30, 1.40, 0.16)  # outer pier
        b.box(WALL, ccx - bay * 1.00, 1.90, 2.52, 0.30, 1.40, 0.16)  # inner pier
        # Window frame (the breakable pane is a dynamic entity).
        b.box(STEEL, ccx, 2.55, 2.55, 1.78, 0.08, 0.22)
        b.box(STEEL, ccx, 1.25, 2.55, 1.78, 0.08, 0.22)
        b.box(STEEL, ccx - 0.85, 1.90, 2.55, 0.08, 1.30, 0.22)
        b.box(STEEL, ccx + 0.85, 1.90, 2.55, 0.08, 1.30, 0.22)
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
    # Dock leveler plate just inside the bay.
    b.box(STEEL, 0, 0.09, 2.28, 1.62, 0.05, 0.86)
    # Painted floor lanes + hazard stripes.
    b.box(YELLOW, 0, 0.066, 2.02, 3.6, 0.02, 0.10)
    b.box(YELLOW, 0, 0.066, 1.90, 3.6, 0.02, 0.10)
    for k in range(5):
        b.box(YELLOW, 0, 0.066, -1.80 + k * 0.80, 0.10, 0.02, 0.40)

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

    # Rack signage: yellow boards with white labels on the back and sides.
    b.box(YELLOW, 0, 2.42, -2.12, 2.60, 0.34, 0.04)
    b.box(WHITE, 0, 2.42, -2.09, 2.20, 0.18, 0.02)
    for sx in (-1, 1):
        b.box(YELLOW, sx * 2.30, 2.30, 1.00, 0.04, 0.34, 0.80)
        b.box(WHITE, sx * 2.27, 2.30, 1.00, 0.02, 0.20, 0.60)

    # --- Labelled pallet stacks with tape-striped cargo ---
    stacks = [(-2.15, 1.35, 3), (1.95, 1.45, 2), (-1.95, -1.15, 3),
              (0.55, -1.35, 2)]
    for si, (px, pz, levels) in enumerate(stacks):
        b.box(WOOD, px, 0.08, pz, 0.90, 0.12, 0.75)
        for i in range(levels):
            cy = 0.14 + 0.18 + i * 0.34
            b.box(PRODUCT, px, cy, pz, 0.70, 0.30, 0.58,
                  uv=tile_uvs((si * 3 + i * 2) % 16))
            b.box(WHITE, px, cy + 0.15, pz, 0.72, 0.04, 0.10)
        top = 0.14 + 0.18 + levels * 0.34
        # Shipping label facing the aisle (+Z).
        b.box(WHITE, px, top - 0.12, pz + 0.30, 0.34, 0.16, 0.02)
        b.box(TRIM, px, top - 0.15, pz + 0.31, 0.20, 0.03, 0.01)

    # --- Forklift (facing +Z, parked by the right rack) ---
    fx, fz = 1.70, -1.40
    b.box(YELLOW, fx, 0.55, fz, 0.90, 0.60, 1.20)          # body
    b.box(TRIM, fx, 0.55, fz - 0.60, 0.80, 0.70, 0.30)     # counterweight
    b.box(TRIM, fx, 0.98, fz - 0.10, 0.42, 0.12, 0.42)     # seat
    b.box(TRIM, fx, 1.20, fz - 0.28, 0.42, 0.42, 0.10)     # seat back
    for gx in (-0.36, 0.36):
        for gz in (-0.45, 0.45):
            b.box(STEEL, fx + gx, 1.32, fz + gz, 0.06, 0.90, 0.06)
    b.box(STEEL, fx, 1.80, fz, 0.90, 0.06, 1.10)           # overhead guard
    for wx in (-0.45, 0.45):
        for wz in (-0.38, 0.38):
            b.box(TRIM, fx + wx, 0.18, fz + wz, 0.16, 0.36, 0.36)
    for mx in (-0.34, 0.34):
        b.box(STEEL, fx + mx, 1.25, fz + 0.66, 0.08, 2.10, 0.10)
    b.box(STEEL, fx, 2.28, fz + 0.66, 0.80, 0.08, 0.10)    # mast crossbar
    for ox in (-0.26, 0.26):
        b.box(STEEL, fx + ox, 0.16, fz + 1.15, 0.14, 0.06, 0.95)  # forks
    # Small orange beacon + white model plate.
    b.box(ORANGE, fx, 1.92, fz - 0.30, 0.14, 0.14, 0.14)
    b.box(WHITE, fx, 0.72, fz + 0.61, 0.30, 0.12, 0.02)

    # --- Shipping desk with monitor, label printer and stool ---
    dx, dz = 2.30, 1.92
    b.box(TRIM, dx, 0.50, dz, 1.30, 1.00, 0.60)
    b.box(WHITE, dx, 1.02, dz, 1.38, 0.06, 0.68)
    b.box(STEEL, dx - 0.34, 1.22, dz - 0.10, 0.42, 0.30, 0.06)
    b.box(BLUE, dx - 0.34, 1.24, dz - 0.13, 0.34, 0.20, 0.02)
    b.box(STEEL, dx + 0.30, 1.16, dz, 0.30, 0.22, 0.30)
    b.box(WHITE, dx + 0.30, 1.28, dz, 0.24, 0.04, 0.24)
    b.box(STEEL, dx + 0.02, 1.06, dz + 0.20, 0.28, 0.04, 0.18)
    b.box(WOOD, dx, 0.42, dz + 0.58, 0.55, 0.10, 0.55)
    b.box(STEEL, dx, 0.20, dz + 0.58, 0.40, 0.40, 0.10)

    # --- Loose cargo boxes by the bay ---
    loose = (
            (-2.30, 2.10, 0.80, 0.55, 0.60, 1),
            (1.55, 2.15, 0.70, 0.65, 0.55, 5),
            (-2.55, 2.10, 0.60, 0.30, 0.45, 9))
    for bx, bz, sx, sy, sz, tile in loose:
        b.box(PRODUCT, bx, sy * 0.5 + 0.06, bz, sx, sy, sz,
              uv=tile_uvs(tile))
        b.box(WHITE, bx, sy * 0.5 + 0.10, bz + sz * 0.5 + 0.01, sx * 0.7, 0.05, 0.02)

    materials = [
        # glTF material index 0 is treated as the no-material default by the
        # engine's importer, so keep a dummy here and start real materials at 1.
        {"name": "wh_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
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
        {"name": "wh_white",
         "pbrMetallicRoughness": {"baseColorFactor": [0.88, 0.87, 0.82, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "wh_blue",
         "pbrMetallicRoughness": {"baseColorFactor": [0.10, 0.24, 0.50, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.6}},
        {"name": "wh_green",
         "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.44, 0.20, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        {"name": "wh_orange",
         "pbrMetallicRoughness": {"baseColorFactor": [0.78, 0.40, 0.10, 1],
                                  "metallicFactor": 0.03, "roughnessFactor": 0.65}},
        product_material("wh_product"),
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
    gltf.update(atlas_extras())
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
