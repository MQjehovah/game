#!/usr/bin/env python3
"""Generate the camp-wide terrain heightfield: rv_terrain.gltf + data JSON.

The world used to get its relief from a single hand-built embankment mesh.
This generator replaces that with one general heightfield that covers the camp
and the roadside:

  * the roadside ridge is kept exactly as before (same cross-section table,
    long-wave noise and end taper);
  * rolling meadow knolls rise to the west and south of the camp;
  * everything is flattened along the fixed road network, around the camp,
    around the ponds and around the POI footprints, so roads/buildings stay
    level and only the open meadow rolls.

Two artifacts are written from the same height function, which is why the
visible mesh and the gameplay sampler cannot drift apart:

  projects/rv/assets/models/rv/rv_terrain.gltf (+ .bin)  -- visible mesh
  projects/rv/assets/data/rv_terrain.json                -- grid for lc_terrain

lc_terrain.lua bilinearly samples the JSON grid; the glTF mesh is generated
from the very same grid, so player, RV, co-op proxies and the visible ground
all agree.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
DATA = ROOT / "projects/rv/assets/data"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_terrain.json"

# Grid covering the camp, the roadside and the surrounding 288 x 288 m.
X0, X1, Z0, Z1 = -144.0, 144.0, -144.0, 144.0
STEP = 4.0
NX = int(round((X1 - X0) / STEP)) + 1
NZ = int(round((Z1 - Z0) / STEP)) + 1

# Roadside embankment cross-section (kept from gen_rv_embankment.py).
EX = [5.0, 8.0, 11.5, 15.0, 19.0, 23.5, 28.0, 33.0, 38.0]
EY = [0.0, 0.70, 1.90, 3.00, 3.50, 3.55, 3.00, 1.50, 0.0]
EZ0, EZ1 = -24.0, 46.0
DIRT_X0, DIRT_X1 = 4.0, 15.5

# Fixed road links (every link declared in worldmap.json, so the whole fixed
# network is graded flat inside the 288 m grid).
ROADS = [
    (0.0, -220.0, 0.0, 0.0),    # main road south of camp
    (0.0, 60.0, 0.0, 220.0),    # main road north of the gas stop
    (0.0, 0.0, -64.0, 44.0),    # camp -> farm
    (0.0, 0.0, -52.0, 30.0),    # camp -> lake camp
    (0.0, 0.0, 24.0, -40.0),    # camp -> clinic
    (0.0, 0.0, 38.0, 27.0),     # camp -> ruin
    (0.0, 0.0, 0.0, 60.0),      # camp -> gas stop
    (0.0, 60.0, 38.0, 27.0),    # gas stop -> ruin
    (0.0, 60.0, 100.0, -73.0),  # gas stop -> mall
    (38.0, 27.0, 131.0, -8.0),  # ruin -> town
    (100.0, -73.0, 131.0, -8.0),# mall -> town
    (131.0, -8.0, 154.0, -58.0),# town -> depot
]

# Fixed POI building centres (node + (6.5, 6.5) in lc_util.poiCenter).
NODES = [
    (0.0, 0.0), (0.0, 60.0), (38.0, 27.0), (100.0, -73.0), (131.0, -8.0),
    (154.0, -58.0), (-64.0, 44.0), (-52.0, 30.0), (24.0, -40.0),
]

PONDS = [(-18.0, -12.0), (-60.0, 23.0)]


def smoothstep(d, a, b):
    if d <= a:
        return 0.0
    if d >= b:
        return 1.0
    t = (d - a) / (b - a)
    return t * t * (3.0 - 2.0 * t)


def seg_dist(x, z, ax, az, bx, bz):
    dx, dz = bx - ax, bz - az
    l2 = dx * dx + dz * dz
    if l2 < 1e-6:
        return math.hypot(x - ax, z - az)
    t = max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / l2))
    return math.hypot(x - (ax + dx * t), z - (az + dz * t))


def embankment(x, z):
    if x <= EX[0] or x >= EX[-1]:
        return 0.0
    base = 0.0
    for i in range(len(EX) - 1):
        if EX[i] <= x <= EX[i + 1]:
            t = (x - EX[i]) / (EX[i + 1] - EX[i])
            base = EY[i] + (EY[i + 1] - EY[i]) * t
            break
    base += 0.28 * math.sin(z * 0.23 + x * 0.35) + 0.12 * math.sin(z * 0.61)
    taper = min(1.0,
                max(0.0, (z - EZ0) / 6.0),
                max(0.0, (EZ1 - z) / 6.0))
    return max(0.0, base * taper)


def bump(x, z, cx, cz, rx, rz, amp):
    d = ((x - cx) / rx) ** 2 + ((z - cz) / rz) ** 2
    if d >= 1.0:
        return 0.0
    return amp * 0.5 * (1.0 + math.cos(math.pi * math.sqrt(d)))


def knolls(x, z):
    h = 0.0
    h += bump(x, z, -26.0, -20.0, 18.0, 14.0, 2.60)   # south-west knoll
    h += bump(x, z, -38.0, 8.0, 14.0, 18.0, 1.90)     # west ridge
    h += bump(x, z, 2.0, -40.0, 20.0, 14.0, 2.20)     # south of camp
    h += bump(x, z, -10.0, -55.0, 22.0, 16.0, 1.60)   # far south meadow
    h += bump(x, z, 12.0, -28.0, 12.0, 10.0, 1.10)    # rise behind the pond
    # Extended 288 x 288 m ring: rolling highlands beyond the old 144 m grid.
    h += bump(x, z, -95.0, 15.0, 26.0, 30.0, 2.80)    # west highlands
    h += bump(x, z, -108.0, -72.0, 24.0, 24.0, 2.20)  # south-west ridge
    h += bump(x, z, -52.0, -98.0, 28.0, 24.0, 2.60)   # far south meadow
    h += bump(x, z, 28.0, 105.0, 30.0, 26.0, 2.40)    # north meadow
    h += bump(x, z, 96.0, 86.0, 26.0, 22.0, 2.00)     # north-east knoll
    h += bump(x, z, 112.0, 24.0, 22.0, 26.0, 2.60)    # east ridge
    h += bump(x, z, 72.0, -116.0, 28.0, 22.0, 2.10)   # south-east knoll
    h += bump(x, z, -126.0, -12.0, 18.0, 26.0, 1.80)  # far west shoulder
    h += bump(x, z, 55.0, -55.0, 20.0, 16.0, 1.20)    # low rise before the mall
    h += bump(x, z, -80.0, 95.0, 24.0, 20.0, 1.70)    # north-west shoulder
    h += 0.30 * math.sin(x * 0.21 + 1.1) * math.cos(z * 0.17)
    return max(0.0, h)


def masks(x, z):
    corridor = 1.0
    for ax, az, bx, bz in ROADS:
        corridor *= smoothstep(seg_dist(x, z, ax, az, bx, bz), 4.0, 8.0)

    # Camp bowl: keep the RV pad and the opening shot level.
    camp = smoothstep(math.hypot(x, z), 12.0, 20.0)

    node = 1.0
    for nx, nz in NODES:
        cx, cz = (nx + 6.5, nz + 6.5) if (nx, nz) != (0.0, 0.0) else (0.0, 0.0)
        node *= smoothstep(math.hypot(x - cx, z - cz), 9.0, 15.0)

    pond = 1.0
    for px, pz in PONDS:
        pond *= smoothstep(math.hypot(x - px, z - pz), 7.0, 11.0)

    return corridor, camp, node, pond


def height(x, z):
    corridor, camp, node, pond = masks(x, z)
    h = embankment(x, z) * corridor * node * pond
    h += knolls(x, z) * corridor * camp * node * pond
    return max(0.0, h)


class Mesh:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], [], []))

    def quad(self, mat, a, b, c, d):
        u = [b[i] - a[i] for i in range(3)]
        v = [d[i] - a[i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(k * k for k in n)) or 1.0
        n = (n[0] / ln, n[1] / ln, n[2] / ln)
        if n[1] < 0:
            b, d = d, b
            n = (-n[0], -n[1], -n[2])
        pos, nrm, idx, uvs = self._group(mat)
        base = len(pos)
        for p in (a, b, c, d):
            pos.append(p)
            nrm.append(n)
            uvs.append((p[0] * 0.30, p[2] * 0.30))
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def box(self, mat, cx, cy, cz, sx, sy, sz):
        hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
        x0, x1 = cx - hx, cx + hx
        y0, y1 = cy - hy, cy + hy
        z0, z1 = cz - hz, cz + hz
        for quad in (
                [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
                [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)],
                [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)],
                [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
                [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)],
                [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]):
            self.quad(mat, quad[0], quad[1], quad[2], quad[3])

    def hidden(self):
        self.box(DEFAULT, 0, -0.6, 0, 0.02, 0.02, 0.02)


DEFAULT, DIRT, GRASS = 0, 1, 2


def main():
    heights = []
    for j in range(NZ):
        z = Z0 + STEP * j
        for i in range(NX):
            x = X0 + STEP * i
            heights.append(round(height(x, z), 4))

    DATA.mkdir(parents=True, exist_ok=True)
    (DATA / "rv_terrain.json").write_text(json.dumps({
        "x0": X0, "z0": Z0, "step": STEP, "nx": NX, "nz": NZ,
        "h": heights,
    }, separators=(",", ":")), encoding="utf-8")

    m = Mesh()
    m.hidden()
    for j in range(NZ - 1):
        for i in range(NX - 1):
            h00 = heights[j * NX + i]
            h10 = heights[j * NX + i + 1]
            h11 = heights[(j + 1) * NX + i + 1]
            h01 = heights[(j + 1) * NX + i]
            if max(h00, h10, h11, h01) <= 0.03:
                continue  # flat: the scene's ground plane already covers it
            x = X0 + STEP * i
            z = Z0 + STEP * j
            a = (x, h00, z)
            b = (x + STEP, h10, z)
            c = (x + STEP, h11, z + STEP)
            d = (x, h01, z + STEP)
            mid_x = x + STEP * 0.5
            mid_z = z + STEP * 0.5
            dirt = (DIRT_X0 <= mid_x <= DIRT_X1 and EZ0 <= mid_z <= EZ1
                    and env_peak(mid_x, mid_z) > 0.2)
            m.quad(DIRT if dirt else GRASS, a, b, c, d)

    materials = [
        {"name": "terrain_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "terrain_dirt",
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95,
                                  "baseColorTexture": {"index": 0}}},
        {"name": "terrain_grass",
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9,
                                  "baseColorTexture": {"index": 1}}},
    ]

    buffer = b""
    views = []
    accessors = []
    prims = []
    for mat_index, (positions, normals, indices, uvs) in sorted(m.groups.items()):
        for raw in (
                b"".join(struct.pack("<3f", *p) for p in positions),
                b"".join(struct.pack("<3f", *n) for n in normals),
                b"".join(struct.pack("<I", i) for i in indices),
                b"".join(struct.pack("<2f", *t) for t in uvs)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 4
        mn = [min(p[i] for p in positions) for i in range(3)]
        mx = [max(p[i] for p in positions) for i in range(3)]
        accessors.extend([
            {"bufferView": vb, "componentType": 5126, "count": len(positions),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": vb + 1, "componentType": 5126, "count": len(normals),
             "type": "VEC3"},
            {"bufferView": vb + 2, "componentType": 5125, "count": len(indices),
             "type": "SCALAR"},
            {"bufferView": vb + 3, "componentType": 5126, "count": len(uvs),
             "type": "VEC2"},
        ])
        ai = len(accessors) - 4
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1,
                                     "TEXCOORD_0": ai + 3},
                      "indices": ai + 2, "material": mat_index})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv terrain generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Terrain", "mesh": 0}],
        "meshes": [{"name": "RV_Terrain", "primitives": prims}],
        "materials": materials,
        "images": [{"uri": "rv_dirt.png"}, {"uri": "rv_ground_meadow.png"}],
        "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
        "buffers": [{"uri": "rv_terrain.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / "rv_terrain.bin").write_bytes(buffer)
    (OUT / "rv_terrain.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_terrain.gltf",
                     "castShadow": False, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")

    verts = sum(len(v[0]) for v in m.groups.values())
    print("terrain grid", NX, "x", NZ, "step", STEP,
          "max", max(heights), "bytes", len(buffer),
          "prims", len(prims), "verts", verts)


def env_peak(x, z):
    """Raw embankment height (ignoring the flatten masks) for the dirt mask."""
    return embankment(x, z)


if __name__ == "__main__":
    main()
