#!/usr/bin/env python3
"""Generate authored multi-part RV furniture: rv_furn_*.gltf.

Replaces the Kenney low-poly furniture used by the placeable catalog while
keeping the same prefab names, footprints and interaction hooks:

  bedSingle / bedDouble  -> rv_furn_bed_single / rv_furn_bed_double
  kitchenStove           -> rv_furn_stove
  kitchenFridge          -> rv_furn_fridge
  kitchenSink            -> rv_furn_sink
  desk (workbench)       -> rv_furn_workbench
  cardboardBoxClosed     -> rv_furn_storage

Every model is centred on the origin with its base at y=0, matching how
lc_build.place() positions modelScale furniture (cell centre, FLOOR_Y+0.01).
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFABS = ROOT / "projects/rv/assets/prefabs"

# material slots (0 = hidden importer default)
(DEFAULT, WOOD, WOOD2, METAL, DARK, WHITE, FABRIC, BLUE, RED, GLASS,
 GREY, GREEN) = range(12)

MATS = [
    ("furn_default", [0.5, 0.5, 0.5], 0.0, 0.9),
    ("furn_wood", [0.46, 0.31, 0.17], 0.02, 0.8),
    ("furn_wood2", [0.30, 0.19, 0.10], 0.02, 0.85),
    ("furn_metal", [0.55, 0.57, 0.60], 0.65, 0.35),
    ("furn_dark", [0.12, 0.13, 0.15], 0.25, 0.5),
    ("furn_white", [0.87, 0.87, 0.84], 0.05, 0.55),
    ("furn_fabric", [0.72, 0.70, 0.64], 0.0, 0.95),
    ("furn_blue", [0.20, 0.34, 0.52], 0.0, 0.9),
    ("furn_red", [0.55, 0.16, 0.13], 0.02, 0.75),
    ("furn_glass", [0.08, 0.10, 0.12], 0.5, 0.15),
    ("furn_grey", [0.42, 0.44, 0.46], 0.15, 0.6),
    ("furn_green", [0.18, 0.44, 0.20], 0.02, 0.7),
]


class Mesh:
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
            indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def ring(self, mat, cx, cy, cz, r0, r1, seg=12):
        """Flat annulus on the XZ plane (normal +Y)."""
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            p = [(cx + math.cos(a0) * r0, cy, cz + math.sin(a0) * r0),
                 (cx + math.cos(a0) * r1, cy, cz + math.sin(a0) * r1),
                 (cx + math.cos(a1) * r1, cy, cz + math.sin(a1) * r1),
                 (cx + math.cos(a1) * r0, cy, cz + math.sin(a1) * r0)]
            pos, nrm, idx = self._group(mat)
            base = len(pos)
            for q in p:
                pos.append(q)
                nrm.append((0.0, 1.0, 0.0))
            idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def hidden(m):
    m.box(DEFAULT, 0, -0.60, 0, 0.02, 0.02, 0.02)


def bed(m, half_w, half_l, blanket_mat):
    hw, hl = half_w, half_l
    # frame rails + slats + legs
    m.box(WOOD2, 0, 0.14, 0, hw * 2, 0.16, hl * 2)
    for dz in (-hl + 0.06, 0, hl - 0.06):
        m.box(WOOD, 0, 0.24, dz, hw * 1.9, 0.06, 0.10)
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box(WOOD2, sx * (hw - 0.05), 0.10, sz * (hl - 0.05), 0.08, 0.20, 0.08)
    # mattress + blanket + fold
    m.box(FABRIC, 0, 0.33, 0, hw * 1.92, 0.16, hl * 1.94)
    m.box(blanket_mat, 0, 0.43, 0.10, hw * 1.86, 0.10, hl * 1.55)
    m.box(blanket_mat, 0, 0.50, -hl * 0.78, hw * 1.80, 0.05, 0.14)
    # pillows
    m.box(WHITE, 0, 0.47, -hl * 0.80, hw * 0.9, 0.12, 0.30)
    if half_w > 0.6:
        m.box(WHITE, -hw * 0.48, 0.47, -hl * 0.80, hw * 0.42, 0.12, 0.30)
        m.box(WHITE, hw * 0.48, 0.47, -hl * 0.80, hw * 0.42, 0.12, 0.30)
    # headboard + footboard
    m.box(WOOD, 0, 0.42, -hl + 0.03, hw * 2, 0.60, 0.06)
    m.box(WOOD, 0, 0.32, hl - 0.03, hw * 2, 0.40, 0.06)


def stove(m):
    # body + steel kick
    m.box(METAL, 0, 0.40, 0, 0.84, 0.64, 0.56)
    m.box(DARK, 0, 0.06, 0, 0.80, 0.10, 0.52)
    # oven door, window, handle, knobs
    m.box(WHITE, 0, 0.34, 0.292, 0.76, 0.40, 0.02)
    m.box(GLASS, 0, 0.36, 0.304, 0.52, 0.24, 0.01)
    m.box(METAL, 0, 0.55, 0.305, 0.62, 0.04, 0.05)
    for i in range(3):
        m.box(DARK, -0.26 + i * 0.26, 0.70, 0.305, 0.09, 0.05, 0.04)
    # cooktop + burners + back panel
    m.box(GREY, 0, 0.745, 0, 0.86, 0.05, 0.58)
    m.ring(DARK, -0.22, 0.775, 0.02, 0.09, 0.14)
    m.ring(DARK, 0.22, 0.775, 0.02, 0.09, 0.14)
    m.ring(METAL, -0.22, 0.78, 0.02, 0.05, 0.09)
    m.ring(METAL, 0.22, 0.78, 0.02, 0.05, 0.09)
    m.box(METAL, 0, 0.86, -0.26, 0.84, 0.24, 0.04)
    m.box(DARK, 0, 0.92, -0.238, 0.58, 0.08, 0.01)


def fridge(m):
    m.box(WHITE, 0, 0.57, 0, 0.50, 1.04, 0.54)
    m.box(GREY, 0, 0.06, 0, 0.46, 0.08, 0.50)
    # split doors + handles + vents
    m.box(WHITE, 0, 0.78, 0.272, 0.48, 0.54, 0.02)
    m.box(WHITE, 0, 0.26, 0.272, 0.48, 0.50, 0.02)
    m.box(DARK, 0, 0.51, 0.276, 0.48, 0.02, 0.01)
    m.box(METAL, 0.17, 0.82, 0.29, 0.03, 0.32, 0.04)
    m.box(METAL, 0.17, 0.30, 0.29, 0.03, 0.26, 0.04)
    for i in range(3):
        m.box(DARK, 0, 0.14 - i * 0.03, 0.272, 0.30, 0.012, 0.01)


def sink(m):
    m.box(WOOD, 0, 0.40, 0, 0.48, 0.80, 0.48)
    m.box(WOOD2, 0, 0.10, 0.0, 0.44, 0.10, 0.44)
    m.box(WHITE, 0, 0.83, 0, 0.52, 0.06, 0.52)
    # basin: four walls + bottom
    m.box(METAL, 0, 0.755, 0, 0.38, 0.02, 0.36)
    m.box(METAL, 0, 0.80, 0.17, 0.38, 0.08, 0.02)
    m.box(METAL, 0, 0.80, -0.17, 0.38, 0.08, 0.02)
    m.box(METAL, 0.17, 0.80, 0, 0.02, 0.08, 0.34)
    m.box(METAL, -0.17, 0.80, 0, 0.02, 0.08, 0.34)
    # faucet + knobs
    m.box(METAL, 0, 0.92, -0.18, 0.04, 0.24, 0.04)
    m.box(METAL, 0, 1.04, -0.13, 0.04, 0.04, 0.14)
    m.box(METAL, 0, 1.00, -0.06, 0.035, 0.08, 0.035)
    for sx in (-1, 1):
        m.box(GREY, sx * 0.11, 0.88, -0.17, 0.05, 0.04, 0.05)
    # cabinet door + handle
    m.box(WOOD2, 0, 0.42, 0.245, 0.40, 0.62, 0.02)
    m.box(METAL, 0.14, 0.44, 0.26, 0.03, 0.16, 0.03)


def workbench(m):
    # top + legs + lower shelf + drawer
    m.box(WOOD, 0, 0.88, 0, 0.80, 0.08, 0.58)
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box(WOOD2, sx * 0.35, 0.44, sz * 0.24, 0.07, 0.88, 0.07)
    m.box(WOOD2, 0, 0.24, 0, 0.72, 0.05, 0.48)
    m.box(WOOD2, 0, 0.62, 0.24, 0.36, 0.16, 0.10)
    m.box(METAL, 0, 0.62, 0.295, 0.12, 0.03, 0.02)
    # pegboard + tools
    m.box(WOOD2, 0, 1.10, -0.26, 0.76, 0.40, 0.04)
    for i in range(4):
        m.box(METAL, -0.24 + i * 0.16, 1.16, -0.235, 0.02, 0.16, 0.02)
    m.box(METAL, -0.20, 1.10, -0.23, 0.05, 0.24, 0.03)
    m.box(METAL, 0.08, 1.10, -0.23, 0.18, 0.05, 0.03)
    m.box(DARK, 0.24, 1.06, -0.23, 0.09, 0.09, 0.04)
    # vise + toolbox
    m.box(METAL, 0.32, 0.98, 0.10, 0.10, 0.12, 0.12)
    m.box(RED, -0.22, 0.96, 0.10, 0.24, 0.10, 0.18)
    m.box(DARK, -0.22, 1.00, 0.10, 0.18, 0.02, 0.04)


def storage(m):
    # plank crate + metal corners + lid + handles + label
    for i in range(3):
        m.box(WOOD, 0, 0.10 + i * 0.15, 0, 0.50, 0.13, 0.48)
    for i in range(3):
        m.box(WOOD2, 0, 0.10 + i * 0.15, 0, 0.02, 0.13, 0.50)
    m.box(WOOD, 0, 0.52, 0, 0.52, 0.06, 0.50)
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box(METAL, sx * 0.24, 0.24, sz * 0.23, 0.02, 0.34, 0.02)
            m.box(METAL, sx * 0.24, 0.50, sz * 0.23, 0.03, 0.03, 0.03)
    m.box(METAL, 0, 0.52, -0.24, 0.10, 0.03, 0.03)
    m.box(METAL, 0, 0.40, 0.25, 0.14, 0.04, 0.03)
    m.box(WHITE, 0, 0.30, 0.252, 0.22, 0.10, 0.01)
    m.box(DARK, 0, 0.30, 0.257, 0.14, 0.02, 0.005)


def tv(m):
    # cabinet + doors + TV set + antenna
    m.box(WOOD, 0, 0.38, 0, 1.00, 0.76, 0.46)
    m.box(WOOD2, 0, 0.05, 0, 0.96, 0.10, 0.42)
    m.box(WOOD2, 0, 0.42, 0.235, 0.46, 0.58, 0.02)
    m.box(WOOD2, 0, 0.42, 0.235, 0.02, 0.58, 0.02)
    m.box(METAL, 0.20, 0.46, 0.25, 0.03, 0.14, 0.03)
    m.box(WOOD, 0, 0.78, 0, 1.02, 0.05, 0.48)
    # TV body + bezel + screen + stand + antenna
    m.box(DARK, 0, 0.99, -0.02, 0.66, 0.40, 0.30)
    m.box(GREY, 0, 0.99, 0.135, 0.60, 0.34, 0.02)
    m.box(GLASS, 0, 0.99, 0.147, 0.54, 0.28, 0.01)
    m.box(WOOD2, 0, 0.80, -0.02, 0.30, 0.04, 0.22)
    m.box(METAL, 0.18, 1.24, -0.02, 0.02, 0.22, 0.02)
    m.box(METAL, -0.10, 1.33, -0.02, 0.30, 0.02, 0.02)
    # speaker slats + DVD box
    for i in range(3):
        m.box(DARK, -0.36, 0.98 + i * 0.05, 0.137, 0.12, 0.02, 0.01)
    m.box(BLUE, 0.28, 0.86, 0.10, 0.18, 0.08, 0.14)


def bathroom(m):
    # wet-bath: tray, back/side walls, toilet, sink, mirror, shower head
    hw, hd = 0.50, 0.70
    m.box(WHITE, 0, 0.06, 0, hw * 2, 0.12, hd * 2)
    m.box(WHITE, 0, 1.00, -hd + 0.03, hw * 2, 1.90, 0.06)
    m.box(WHITE, -hw + 0.03, 1.00, 0, 0.06, 1.90, hd * 2)
    m.box(GREY, hw - 0.03, 1.00, 0.0, 0.05, 1.90, hd * 2)
    m.box(DARK, 0, 1.96, 0, hw * 2, 0.06, hd * 2)
    # toilet (back-left): base, bowl, seat, lid, tank
    m.box(WHITE, -0.26, 0.28, -0.42, 0.36, 0.34, 0.46)
    m.box(WHITE, -0.26, 0.47, -0.42, 0.40, 0.06, 0.50)
    m.box(GREY, -0.26, 0.51, -0.42, 0.38, 0.02, 0.48)
    m.box(WHITE, -0.26, 0.62, -0.62, 0.40, 0.42, 0.18)
    m.box(METAL, -0.26, 0.86, -0.54, 0.06, 0.04, 0.04)
    # sink (front-right): cabinet, basin, faucet
    m.box(WOOD, 0.28, 0.42, 0.44, 0.40, 0.80, 0.40)
    m.box(WHITE, 0.28, 0.85, 0.44, 0.44, 0.06, 0.44)
    m.box(METAL, 0.28, 0.82, 0.44, 0.30, 0.02, 0.28)
    m.box(METAL, 0.28, 0.93, 0.32, 0.03, 0.14, 0.03)
    m.box(METAL, 0.28, 1.00, 0.37, 0.03, 0.03, 0.10)
    # mirror + shower head + curtain rail
    m.box(GLASS, 0.0, 1.42, -hd + 0.07, 0.52, 0.40, 0.02)
    m.box(METAL, hw - 0.10, 1.70, -0.28, 0.03, 0.26, 0.03)
    m.box(METAL, hw - 0.20, 1.82, -0.28, 0.18, 0.03, 0.03)
    m.box(GREY, hw - 0.30, 1.78, -0.28, 0.10, 0.04, 0.10)
    m.box(METAL, 0, 1.86, 0.55, hw * 2 - 0.06, 0.02, 0.02)


def med_cabinet(m):
    # white cabinet, glass door, shelves + medicine boxes, red cross
    m.box(WHITE, 0, 0.38, 0, 0.50, 0.72, 0.30)
    m.box(GREY, 0, 0.05, 0, 0.46, 0.06, 0.26)
    for y in (0.24, 0.44):
        m.box(WHITE, 0, y, 0.02, 0.42, 0.03, 0.24)
    for i, c in enumerate((RED, BLUE, GREEN, WHITE, GREY)):
        m.box(c, -0.15 + (i % 3) * 0.15, 0.30 + (i // 3) * 0.20, 0.06,
              0.11, 0.14, 0.10)
    m.box(GLASS, 0, 0.38, 0.16, 0.42, 0.62, 0.02)
    m.box(METAL, 0.18, 0.40, 0.175, 0.03, 0.14, 0.02)
    m.box(RED, 0, 0.80, 0.05, 0.22, 0.22, 0.03)
    m.box(WHITE, 0, 0.80, 0.07, 0.06, 0.18, 0.01)
    m.box(WHITE, 0, 0.80, 0.07, 0.18, 0.06, 0.01)


def gun_rack(m):
    # wooden rack: backboard, posts, shelf, two rifles, ammo boxes
    m.box(WOOD, 0, 0.62, -0.08, 0.98, 1.20, 0.05)
    for sx in (-1, 1):
        m.box(WOOD2, sx * 0.47, 0.62, -0.08, 0.07, 1.24, 0.10)
    m.box(WOOD2, 0, 0.05, -0.05, 0.94, 0.10, 0.28)
    m.box(WOOD2, 0, 0.36, -0.02, 0.90, 0.06, 0.20)
    # rifles: body, barrel, stock, magazine (two upright)
    for i, sx in enumerate((-0.24, 0.24)):
        m.box(DARK, sx, 0.72, 0.02, 0.07, 0.72, 0.07)
        m.box(GREY, sx, 0.50, 0.02, 0.05, 0.28, 0.05)
        m.box(METAL, sx, 1.10, 0.02, 0.03, 0.30, 0.03)
        m.box(WOOD2, sx, 0.48, 0.02, 0.12, 0.20, 0.05)
        m.box(DARK, sx, 0.70, 0.08, 0.05, 0.10, 0.06)
    # ammo boxes + padlock
    m.box(GREEN, -0.30, 0.14, 0.06, 0.26, 0.16, 0.18)
    m.box(GREEN, 0.02, 0.14, 0.06, 0.26, 0.16, 0.18)
    m.box(METAL, 0.34, 0.18, 0.08, 0.10, 0.06, 0.06)


def heater(m):
    # floor heater: body, front grille, top vents, knob, feet, glow
    m.box(GREY, 0, 0.30, 0, 0.46, 0.50, 0.30)
    m.box(DARK, 0, 0.30, 0.152, 0.36, 0.34, 0.02)
    for i in range(5):
        m.box(METAL, 0, 0.17 + i * 0.065, 0.163, 0.30, 0.02, 0.01)
    m.box(DARK, 0, 0.56, 0, 0.40, 0.03, 0.24)
    for i in range(4):
        m.box(METAL, -0.15 + i * 0.10, 0.575, 0, 0.03, 0.01, 0.20)
    m.box(METAL, 0.16, 0.46, 0.152, 0.06, 0.06, 0.03)
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.box(DARK, sx * 0.18, 0.03, sz * 0.10, 0.06, 0.06, 0.06)
    m.box(RED, 0.02, 0.30, 0.166, 0.12, 0.04, 0.01)


MODELS = {
    "rv_furn_bed_single": ("RV_Furn_BedSingle", lambda m: bed(m, 0.475, 0.72, BLUE)),
    "rv_furn_bed_double": ("RV_Furn_BedDouble", lambda m: bed(m, 0.72, 0.96, RED)),
    "rv_furn_stove": ("RV_Furn_Stove", stove),
    "rv_furn_fridge": ("RV_Furn_Fridge", fridge),
    "rv_furn_sink": ("RV_Furn_Sink", sink),
    "rv_furn_workbench": ("RV_Furn_Workbench", workbench),
    "rv_furn_storage": ("RV_Furn_Storage", storage),
    "rv_furn_tv": ("RV_Furn_TV", tv),
    "rv_furn_bathroom": ("RV_Furn_Bathroom", bathroom),
    "rv_furn_med_cabinet": ("RV_Furn_MedCabinet", med_cabinet),
    "rv_furn_gun_rack": ("RV_Furn_GunRack", gun_rack),
    "rv_furn_heater": ("RV_Furn_Heater", heater),
}


def write_model(key, node, build):
    m = Mesh()
    hidden(m)
    build(m)
    buffer = b""
    views = []
    accessors = []
    prims = []
    for mat_index, (positions, normals, indices) in sorted(m.groups.items()):
        for raw in (
                b"".join(struct.pack("<3f", *p) for p in positions),
                b"".join(struct.pack("<3f", *n) for n in normals),
                b"".join(struct.pack("<I", i) for i in indices)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 3
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
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1},
                      "indices": ai + 2, "material": mat_index})

    materials = []
    for name, color, metal, rough in MATS:
        materials.append({
            "name": name.replace("furn_", "furn_%s_" % key.split("_")[-1]),
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1],
                "metallicFactor": metal, "roughnessFactor": rough},
        })
    # material 0 must stay the importer default; restore its neutral grey
    materials[0]["name"] = "furn_default"

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv furniture generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": node, "mesh": 0}],
        "meshes": [{"name": node, "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": key + ".bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / (key + ".bin")).write_bytes(buffer)
    (OUT / (key + ".gltf")).write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    (PREFABS / (key + ".json")).write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/" + key + ".gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(key, "bytes", len(buffer), "prims", len(prims),
          "verts", sum(len(v[0]) for v in m.groups.values()))


def main():
    for key, (node, build) in MODELS.items():
        write_model(key, node, build)


if __name__ == "__main__":
    main()
