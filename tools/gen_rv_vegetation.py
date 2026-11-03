#!/usr/bin/env python3
"""Generate stylised meadow vegetation: grass tufts, wildflowers, ferns, bushes.

Matches the reference art direction (soft, saturated greens with colourful
flower dots). Every model is centred on the origin with its base at y=0 so the
scatter code can place it directly on the ground.

Outputs (projects/rv/assets/models/rv/):
  rv_grass_tuft.gltf / rv_grass_tuft_b.gltf / rv_fern.gltf
  rv_flower_white/blue/yellow/red.gltf
  rv_bush.gltf
"""
import json
import math
import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFABS = ROOT / "projects/rv/assets/prefabs"

rng = random.Random(20261104)

# material slots (0 = hidden importer default)
(DEFAULT, GRASS, GRASS_LIGHT, STEM, LEAF, PETAL_WHITE, PETAL_BLUE,
 PETAL_YELLOW, PETAL_RED, FLOWER_CENTER, BUSH, BUSH_LIGHT) = range(12)

MATS = [
    ("veg_default", [0.5, 0.5, 0.5], False),
    ("veg_grass", [0.20, 0.38, 0.13], True),
    ("veg_grass_light", [0.31, 0.52, 0.17], True),
    ("veg_stem", [0.22, 0.36, 0.12], True),
    ("veg_leaf", [0.19, 0.34, 0.11], True),
    ("veg_petal_white", [0.96, 0.96, 0.92], True),
    ("veg_petal_blue", [0.33, 0.53, 0.90], True),
    ("veg_petal_yellow", [0.95, 0.79, 0.24], True),
    ("veg_petal_red", [0.86, 0.30, 0.30], True),
    ("veg_flower_center", [0.95, 0.74, 0.16], True),
    ("veg_bush", [0.13, 0.26, 0.10], False),
    ("veg_bush_light", [0.19, 0.35, 0.13], False),
]


class Mesh:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], []))

    def poly(self, mat, verts, normal=None):
        if normal is None:
            a, b, c = verts[0], verts[1], verts[2]
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            normal = (u[1] * v[2] - u[2] * v[1],
                      u[2] * v[0] - u[0] * v[2],
                      u[0] * v[1] - u[1] * v[0])
            ln = math.sqrt(sum(k * k for k in normal)) or 1.0
            normal = (normal[0] / ln, normal[1] / ln, normal[2] / ln)
        pos, nrm, idx = self._group(mat)
        base = len(pos)
        for p in verts:
            pos.append(p)
            nrm.append(normal)
        for i in range(1, len(verts) - 1):
            idx.extend([base, base + i, base + i + 1])

    def box(self, mat, cx, cy, cz, sx, sy, sz):
        hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
        x0, x1 = cx - hx, cx + hx
        y0, y1 = cy - hy, cy + hy
        z0, z1 = cz - hz, cz + hz
        for n, quad in (
                ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
                ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),
                ((1, 0, 0), [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),
                ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
                ((0, 1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),
                ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)])):
            self.poly(mat, quad, n)

    def blade(self, mat, cx, cy, cz, yaw, height, lean, width):
        ux, uz = math.cos(yaw), -math.sin(yaw)
        tx, tz = math.sin(yaw) * lean, math.cos(yaw) * lean
        hw = width * 0.5
        tw = width * 0.06
        mx = cx + tx * 0.45
        mz = cz + tz * 0.45
        my = cy + height * 0.55
        base_l = (cx - ux * hw, cy, cz - uz * hw)
        base_r = (cx + ux * hw, cy, cz + uz * hw)
        mid_l = (mx - ux * hw * 0.6, my, mz - uz * hw * 0.6)
        mid_r = (mx + ux * hw * 0.6, my, mz + uz * hw * 0.6)
        tip_l = (cx + tx - ux * tw, cy + height, cz + tz - uz * tw)
        tip_r = (cx + tx + ux * tw, cy + height, cz + tz + uz * tw)
        self.poly(mat, [base_l, base_r, mid_r, mid_l])
        self.poly(mat, [mid_l, mid_r, tip_r, tip_l])

    def sphere(self, mat, cx, cy, cz, r, seg=8, rings=4):
        def p(phi, th):
            return (cx + r * math.sin(phi) * math.cos(th),
                    cy + r * math.cos(phi),
                    cz + r * math.sin(phi) * math.sin(th))
        for iy in range(rings):
            p0 = math.pi * iy / rings
            p1 = math.pi * (iy + 1) / rings
            for ix in range(seg):
                t0 = 2 * math.pi * ix / seg
                t1 = 2 * math.pi * (ix + 1) / seg
                quad = [p(p0, t0), p(p0, t1), p(p1, t1), p(p1, t0)]
                cq = (cx, cy, cz)
                n = (quad[0][0] + quad[2][0] - 2 * cq[0],
                     quad[0][1] + quad[2][1] - 2 * cq[1],
                     quad[0][2] + quad[2][2] - 2 * cq[2])
                ln = math.sqrt(sum(k * k for k in n)) or 1.0
                self.poly(mat, quad, (n[0] / ln, n[1] / ln, n[2] / ln))


def hidden(m):
    m.box(DEFAULT, 0, -0.6, 0, 0.02, 0.02, 0.02)


def grass_tuft(m, count, hmin, hmax, mat, ox=0.0, oz=0.0):
    # Blades lean in a common direction so yaw sway reads as wind, not spin.
    base = rng.uniform(0, math.pi * 2)
    for _ in range(count):
        yaw = base + rng.uniform(-1.0, 1.0)
        m.blade(mat, ox + rng.uniform(-0.05, 0.05), 0,
                oz + rng.uniform(-0.05, 0.05),
                yaw, rng.uniform(hmin, hmax), rng.uniform(0.07, 0.16),
                rng.uniform(0.012, 0.024))


def flower(m, petal, ox=0.0, oz=0.0):
    # Three blooms per model so a single scattered entity reads as a patch.
    for _ in range(3):
        bx = ox + rng.uniform(-0.06, 0.06)
        bz = oz + rng.uniform(-0.06, 0.06)
        h = rng.uniform(0.20, 0.34)
        m.box(STEM, bx, h * 0.5, bz, 0.008, h, 0.008)
        for a in (0.6, 3.4):
            lx, lz = math.cos(a) * 0.03, math.sin(a) * 0.03
            m.poly(LEAF, [(bx, h * 0.28, bz),
                          (bx + lx, h * 0.34, bz + lz),
                          (bx, h * 0.46, bz)])
        n = 6
        r0, r1 = 0.014, 0.06
        for i in range(n):
            a = math.pi * 2 * i / n
            a2 = a + 0.5
            m.poly(petal, [
                (bx + math.cos(a) * r0, h, bz + math.sin(a) * r0),
                (bx + math.cos(a2) * r1, h + 0.008, bz + math.sin(a2) * r1),
                (bx + math.cos(a + 0.6) * r0, h, bz + math.sin(a + 0.6) * r0),
            ])
        m.sphere(FLOWER_CENTER, bx, h + 0.01, bz, 0.02, seg=6, rings=2)


def bush(m, ox=0.0, oz=0.0):
    m.sphere(BUSH, ox, 0.34, oz, 0.34, seg=8, rings=4)
    m.sphere(BUSH, ox + 0.20, 0.24, oz + 0.08, 0.24, seg=8, rings=4)
    m.sphere(BUSH, ox - 0.16, 0.22, oz - 0.10, 0.22, seg=8, rings=4)
    m.sphere(BUSH_LIGHT, ox - 0.04, 0.50, oz + 0.02, 0.22, seg=8, rings=4)
    m.sphere(BUSH_LIGHT, ox + 0.16, 0.40, oz - 0.08, 0.16, seg=8, rings=3)


def meadow_patch(m, petal):
    """A 6x6 m tile of grass tufts + two flower clusters as one mesh."""
    for _ in range(18):
        ox = rng.uniform(-2.7, 2.7)
        oz = rng.uniform(-2.7, 2.7)
        grass_tuft(m, rng.randint(4, 8), 0.18, 0.42, GRASS, ox, oz)
    for _ in range(2):
        ox = rng.uniform(-2.3, 2.3)
        oz = rng.uniform(-2.3, 2.3)
        flower(m, petal, ox, oz)


MODELS = {
    "rv_grass_tuft": ("RV_GrassTuft", lambda m: grass_tuft(m, 14, 0.22, 0.40, GRASS)),
    "rv_grass_tuft_b": ("RV_GrassTuftB", lambda m: grass_tuft(m, 18, 0.28, 0.52, GRASS_LIGHT)),
    "rv_fern": ("RV_Fern", lambda m: grass_tuft(m, 12, 0.18, 0.34, LEAF)),
    "rv_flower_white": ("RV_FlowerWhite", lambda m: flower(m, PETAL_WHITE)),
    "rv_flower_blue": ("RV_FlowerBlue", lambda m: flower(m, PETAL_BLUE)),
    "rv_flower_yellow": ("RV_FlowerYellow", lambda m: flower(m, PETAL_YELLOW)),
    "rv_flower_red": ("RV_FlowerRed", lambda m: flower(m, PETAL_RED)),
    "rv_bush": ("RV_Bush", bush),
    "rv_meadow_a": ("RV_MeadowA", lambda m: meadow_patch(m, PETAL_WHITE)),
    "rv_meadow_b": ("RV_MeadowB", lambda m: meadow_patch(m, PETAL_BLUE)),
    "rv_meadow_c": ("RV_MeadowC", lambda m: meadow_patch(m, PETAL_YELLOW)),
}


def write_model(key, node, build):
    # Per-vertex wind sway amplitude read by the engine's lit vertex shader.
    # Bushes are woody and sway less; meadow patches/grass/flowers move fully.
    wind = 0.5 if key == "rv_bush" else 1.0
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
    for name, color, two_sided in MATS:
        mat = {
            "name": name.replace("veg_", "veg_%s_" % key.replace("rv_", ""))[:40],
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1],
                "metallicFactor": 0.0, "roughnessFactor": 0.9},
        }
        if two_sided:
            mat["doubleSided"] = True
        materials.append(mat)
    materials[0]["name"] = "veg_default"

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv vegetation generator"},
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
                     "castShadow": True, "receiveShadow": True,
                     "windSway": wind},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(key, "bytes", len(buffer), "prims", len(prims),
          "verts", sum(len(v[0]) for v in m.groups.values()))


def main():
    for key, (node, build) in MODELS.items():
        write_model(key, node, build)


if __name__ == "__main__":
    main()
