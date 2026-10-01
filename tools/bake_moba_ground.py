#!/usr/bin/env python3
"""Bake the moba ground-height grid from sr_map.glb.

The Summoner's Rift map's walkable surface sits at y ~= -1.3..-1.7 (the glb's
own origin is below the play surface), while moba.lua's gameplay plane is
y = 0. Units used to be hard-placed at y = 0 and therefore floated above the
ground. This tool ray-casts the map mesh downward at every walkable nav-grid
cell and emits assets/scripts/moba_ground.lua (a MOBA_GROUND height grid that
moba.lua's groundY() samples bilinearly).

Regenerate after changing the map mesh or the nav grid:
    python tools/bake_moba_ground.py
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
GLB = ROOT / "projects/moba/assets/models/sr/sr_map.glb"
NAV = ROOT / "projects/moba/assets/navgrid/summoners_rift.navgrid.json"
OUT = ROOT / "projects/moba/assets/scripts/moba_ground.lua"

# The scene entity transform of the map (moba.json): yaw -45 degrees about Y.
YAW = -np.pi / 4
STEP = 2  # baked grid cell (world units)


def read_glb(path):
    with open(path, "rb") as f:
        f.seek(12)
        clen, _ = struct.unpack("<II", f.read(8))
        js = json.loads(f.read(clen).decode("utf-8"))
        blen, _ = struct.unpack("<II", f.read(8))
        bin_data = f.read(blen)
    return js, bin_data


def accessor(js, bin_data, i):
    acc = js["accessors"][i]
    bv = js["bufferViews"][acc.get("bufferView", 0)]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    comp = {"VEC3": 3, "VEC2": 2, "SCALAR": 1}[acc["type"]]
    dt = np.float32 if acc["componentType"] == 5126 else np.uint32
    return np.frombuffer(bin_data, dtype=dt, count=acc["count"] * comp, offset=off).reshape(
        acc["count"], comp)


def world_prims(js, bin_data):
    ca, sa = np.cos(YAW), np.sin(YAW)
    prims = []
    for m in js["meshes"]:
        for pr in m["primitives"]:
            pa = pr["attributes"].get("POSITION")
            if pa is None:
                continue
            v = accessor(js, bin_data, pa).astype(np.float32)
            x, y, z = v[:, 0], v[:, 1], v[:, 2]
            w = np.stack([ca * x + sa * z, y, -sa * x + ca * z], 1).astype(np.float32)
            ia = pr.get("indices")
            if ia is not None:
                idx = accessor(js, bin_data, ia).astype(np.int64).reshape(-1, 3)
            else:
                idx = np.arange(len(v), dtype=np.int64).reshape(-1, 3)
            prims.append((w, idx))
    return prims


def height_at(prims, px, pz):
    """Topmost mesh surface Y under the world XZ point (vertical ray)."""
    best = -1e9
    for w, idx in prims:
        mn, mx = w.min(0), w.max(0)
        if px < mn[0] or px > mx[0] or pz < mn[2] or pz > mx[2]:
            continue
        tri = w[idx]
        a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
        e1, e2 = b - a, c - a
        dpx, dpz = px - a[:, 0], pz - a[:, 2]
        det = e1[:, 0] * e2[:, 2] - e1[:, 2] * e2[:, 0]
        ok = np.abs(det) > 1e-9
        if not ok.any():
            continue
        safe = np.where(ok, det, 1)
        s = (dpx * e2[:, 2] - dpz * e2[:, 0]) / safe
        t = (e1[:, 0] * dpz - e1[:, 2] * dpx) / safe
        hit = ok & (s >= 0) & (t >= 0) & (s + t <= 1)
        if not hit.any():
            continue
        y = a[:, 1] + s * e1[:, 1] + t * e2[:, 1]
        best = max(best, float(y[hit].max()))
    return best


def main():
    js, bin_data = read_glb(GLB)
    prims = world_prims(js, bin_data)
    print(f"mesh primitives: {len(prims)}, tris: {sum(len(i) for _, i in prims)}")

    nj = json.load(open(NAV, encoding="utf-8"))
    ox, oz = nj["origin"]
    cs, W, H = nj["cellSize"], nj["width"], nj["height"]
    walk = np.zeros((H, W), dtype=bool)
    for r, row in enumerate(nj["rows"]):
        for c, ch in enumerate(row):
            if ch == ".":
                walk[r, c] = True

    span = max(W, H) * cs
    n = int(span // STEP) + 1
    grid = np.full((n, n), np.nan, dtype=np.float32)
    baked = 0
    for j in range(n):
        for i in range(n):
            px, pz = ox + i * STEP, oz + j * STEP
            ci, ri = int((px - ox) / cs), int((pz - oz) / cs)
            near = any(
                0 <= rr < H and 0 <= cc < W and walk[rr, cc]
                for rr in (ri - 1, ri, ri + 1)
                for cc in (ci - 1, ci, ci + 1)
            )
            if not near:
                continue
            h = height_at(prims, px, pz)
            if h > -1e8:
                grid[j, i] = h
                baked += 1
    print(f"baked {baked}/{n * n} nodes (near walkable cells)")

    # Fill non-walkable holes by neighbour dilation, then median-smooth spikes.
    for _ in range(4 * n):
        nan = np.isnan(grid)
        if not nan.any():
            break
        for j in range(n):
            for i in range(n):
                if not nan[j, i]:
                    continue
                vals = [
                    grid[j + dj, i + di]
                    for dj, di in ((0, 1), (0, -1), (1, 0), (-1, 0))
                    if 0 <= j + dj < n and 0 <= i + di < n and not np.isnan(grid[j + dj, i + di])
                ]
                if vals:
                    grid[j, i] = float(np.mean(vals))
    grid[np.isnan(grid)] = float(np.nanmedian(grid))

    from numpy.lib.stride_tricks import sliding_window_view
    pad = np.pad(grid, 1, mode="edge")
    win = sliding_window_view(pad, (3, 3))
    med = np.median(win.reshape(n, n, 9), axis=2).astype(np.float32)
    grid = np.where(np.abs(grid - med) > 0.8, med, grid)
    grid = np.clip(grid, -8.0, 3.0)
    print(f"height stats: min {grid.min():.2f} max {grid.max():.2f} mean {grid.mean():.2f}")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("-- 自动生成（tools/bake_moba_ground.py 烘焙自 sr_map.glb）。请勿手改。\n")
        f.write(
            "MOBA_GROUND = { cell = %d, minx = %.3f, minz = %.3f, n = %d, h = {\n"
            % (STEP, ox, oz, n)
        )
        for j in range(n):
            f.write("\t" + ",".join("%.3f" % v for v in grid[j]) + (",\n" if j < n - 1 else "\n"))
        f.write("} }\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
