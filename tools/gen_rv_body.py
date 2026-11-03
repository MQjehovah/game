#!/usr/bin/env python3
"""Generate rv_body.gltf: a higher-fidelity scripted motorhome shell.

Replaces the old low-poly RV body while keeping the gameplay contracts:

  * overall footprint x +-1.2, y 0.25..2.16, z -4.0..5.6
  * the passenger-door opening on +X stays exactly z -3.54..-2.76,
    y 0.24..1.86 (rv_door.gltf hinges at local (1.205, -3.54))
  * wheel centres stay at ( +-1.26, 0.38, +-2.92 )
  * seat transforms live in Lua and are unchanged

Adds beveled roof/skirt panels, recessed side windows with frames, wheel
arches and wheel wells, front/rear bumpers, side rub strips, a roof rack,
ladder, awning case, mirrors, lights, grille, exhaust, dashboard, steering
wheel, a multi-part driver seat and overhead cabinetry.
"""
import json
import math
import struct
from pathlib import Path

from rv_surfaces import (face_uvs, surface_emissive, surface_extras,
                         surface_material)
from rv_geom import bevel_box

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_body.json"
STATS = ROOT / "projects/rv/assets/data/rv_body_stats.json"

# material indices (0 is the hidden importer-default slot)
(DEFAULT, BODY, ROOF, TRIM, GLASS, TIRE, HUB, LIGHT, TAIL, DOOR,
 WOOD, FABRIC) = range(12)

# body extents
X = 1.18          # outer side plane
XI = 1.10         # inner side plane
Y0, Y1 = 0.25, 2.10
ZB, ZF = -4.00, 5.56
WHEEL_Z = (-2.92, 2.92)
WHEEL_R = 0.40
ARCH_R = 0.50


class Mesh:
    def __init__(self):
        self.groups = {}

    def _group(self, mat):
        return self.groups.setdefault(mat, ([], [], [], []))

    def box(self, mat, cx, cy, cz, sx, sy, sz, bevel=None):
        bevel_box(self, mat, cx, cy, cz, sx, sy, sz, bevel)

    def poly(self, mat, verts, normal):
        pos, nrm, idx, uvs = self._group(mat)
        base = len(pos)
        for q in verts:
            pos.append(q)
            nrm.append(normal)
        uvs.extend(face_uvs(verts, normal))
        for i in range(1, len(verts) - 1):
            idx.extend([base, base + i, base + i + 1])

    def quad(self, mat, a, b, c, d, normal=None):
        """Add a quad, flipping the winding so its normal matches `normal`."""
        if normal is None:
            u = [b[i] - a[i] for i in range(3)]
            v = [d[i] - a[i] for i in range(3)]
            normal = (u[1] * v[2] - u[2] * v[1],
                      u[2] * v[0] - u[0] * v[2],
                      u[0] * v[1] - u[1] * v[0])
            ln = math.sqrt(sum(c * c for c in normal)) or 1.0
            normal = (normal[0] / ln, normal[1] / ln, normal[2] / ln)
        else:
            u = [b[i] - a[i] for i in range(3)]
            v = [d[i] - a[i] for i in range(3)]
            g = (u[1] * v[2] - u[2] * v[1],
                 u[2] * v[0] - u[0] * v[2],
                 u[0] * v[1] - u[1] * v[0])
            if g[0] * normal[0] + g[1] * normal[1] + g[2] * normal[2] < 0:
                b, d = d, b
        pos, nrm, idx, uvs = self._group(mat)
        base = len(pos)
        for p in (a, b, c, d):
            pos.append(p)
            nrm.append(normal)
        uvs.extend(face_uvs((a, b, c, d), normal))
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def disc(self, mat, cx, cy, cz, r, seg, axis=1, sign=1.0):
        """Flat disc perpendicular to axis (0=x, 1=y)."""
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            if axis == 1:
                p0 = (cx + math.cos(a0) * r, cy, cz + math.sin(a0) * r)
                p1 = (cx + math.cos(a1) * r, cy, cz + math.sin(a1) * r)
                n = (0, sign, 0)
            else:
                p0 = (cx, cy + math.cos(a0) * r, cz + math.sin(a0) * r)
                p1 = (cx, cy + math.cos(a1) * r, cz + math.sin(a1) * r)
                n = (sign, 0, 0)
            self.quad(mat, (cx, cy, cz), p0, p1, (cx, cy, cz), n)


ARCH_LIP_SEGMENTS = 16
ROOF_SHOULDER = [(1.18, 2.02), (1.16, 2.07), (1.11, 2.115), (1.08, 2.14)]


def arch_lip(m, sign, cz, seg=ARCH_LIP_SEGMENTS):
    """Continuous rolled lip swept around the wheel opening (not stepped boxes)."""
    x0 = sign * (X + 0.006)
    x1 = sign * (X + 0.055)
    prev = None
    for i in range(seg + 1):
        a = math.pi * i / seg
        z = cz + math.cos(a) * ARCH_R
        y = 0.38 + math.sin(a) * ARCH_R
        cur = ((x0, y, z), (x1, y + 0.015, z))
        if prev is not None:
            m.poly(TRIM, [prev[0], cur[0], cur[1], prev[1]],
                   (sign, 0.0, 0.0))
        prev = cur


def arch_well(m, sign, cz, seg=ARCH_LIP_SEGMENTS):
    """Semicircular inner wheel-well wall (triangle fan) behind the opening."""
    x = sign * 1.055
    r = ARCH_R - 0.03
    center = (x, 0.38, cz)
    for i in range(seg):
        a0 = math.pi * i / seg
        a1 = math.pi * (i + 1) / seg
        p0 = (x, 0.38 + math.sin(a0) * r, cz + math.cos(a0) * r)
        p1 = (x, 0.38 + math.sin(a1) * r, cz + math.cos(a1) * r)
        m.poly(TRIM, [center, p0, p1], (sign, 0.0, 0.0))


WALL_CURVE = 0.05
WALL_T = 0.08


def wall_x(sign, y):
    """Outer skin: the side panel bows out near mid-height and tucks in at the
    sill/roof (a single curved surface instead of a stacked box wall)."""
    t = (y - Y0) / max(1e-6, (Y1 - Y0))
    return sign * (X - WALL_CURVE * (t - 0.55) ** 2)


def wall_xi(sign, y):
    """Inner skin, offset inward by WALL_T so the wall has real thickness."""
    return wall_x(sign, y) - sign * WALL_T


def side_wall(m, sign, openings, windows):
    """Lofted wall with thickness, polygonal openings and jamb reveals.

    Outer and inner skins share the same grid; z/y cut lines sit exactly on
    every opening edge and on 8 arc samples of each wheel well. Cells whose
    centre is inside a hole are culled, then every rectangular hole gets four
    jamb strips (top/bottom/left/right) and each arch gets a curved reveal
    ring, so the openings read as cut-through walls rather than a pasted pane.
    """
    holes = list(openings) + list(windows)
    zcuts = {ZB, ZF}
    ycuts = {Y0, Y1}
    for (z0, z1, y0, y1) in holes:
        zcuts.update((z0, z1))
        ycuts.update((y0, y1))
    for cz in WHEEL_Z:
        zcuts.update((cz - ARCH_R, cz + ARCH_R))
        ycuts.update((0.38, 0.38 + ARCH_R))
        for k in range(1, 8):
            a = math.pi * k / 8
            zcuts.add(cz + math.cos(a) * ARCH_R)
            ycuts.add(0.38 + math.sin(a) * ARCH_R)
    zs = sorted(zcuts)
    ys = sorted(ycuts)
    quads = 0
    inner = 0
    for i in range(len(ys) - 1):
        y0, y1 = ys[i], ys[i + 1]
        if y1 - y0 < 1e-4:
            continue
        yc = (y0 + y1) * 0.5
        for j in range(len(zs) - 1):
            z0, z1 = zs[j], zs[j + 1]
            if z1 - z0 < 1e-4:
                continue
            zc = (z0 + z1) * 0.5
            skip = False
            for (hz0, hz1, hy0, hy1) in holes:
                if zc > hz0 and zc < hz1 and yc > hy0 and yc < hy1:
                    skip = True
                    break
            if not skip:
                for cz in WHEEL_Z:
                    dz, dy = zc - cz, yc - 0.38
                    if dz * dz + dy * dy < (ARCH_R - 0.005) ** 2:
                        skip = True
                        break
            if skip:
                continue
            xo0, xo1 = wall_x(sign, y0), wall_x(sign, y1)
            xi0, xi1 = wall_xi(sign, y0), wall_xi(sign, y1)
            m.poly(BODY, [(xo0, y0, z0), (xo0, y0, z1),
                          (xo1, y1, z1), (xo1, y1, z0)], (sign, 0.0, 0.0))
            m.poly(BODY, [(xi0, y0, z0), (xi1, y1, z0),
                          (xi1, y1, z1), (xi0, y0, z1)], (-sign, 0.0, 0.0))
            quads = quads + 1
            inner = inner + 1

    # Jambs: four reveal strips per rectangular opening.
    jambs = 0
    for (z0, z1, y0, y1) in holes:
        y0c = max(y0, Y0)
        y1c = min(y1, Y1)
        if y1c <= y0c:
            continue
        xo0, xo1 = wall_x(sign, y0c), wall_x(sign, y1c)
        xi0, xi1 = wall_xi(sign, y0c), wall_xi(sign, y1c)
        # bottom / top (normal into the opening)
        m.poly(TRIM, [(xo0, y0c, z0), (xo0, y0c, z1),
                      (xi0, y0c, z1), (xi0, y0c, z0)], (0.0, 1.0, 0.0))
        m.poly(TRIM, [(xo1, y1c, z0), (xi1, y1c, z0),
                      (xi1, y1c, z1), (xo1, y1c, z1)], (0.0, -1.0, 0.0))
        # left / right (normal along z)
        m.poly(TRIM, [(xo0, y0c, z0), (xi0, y0c, z0),
                      (xi1, y1c, z0), (xo1, y1c, z0)], (0.0, 0.0, 1.0))
        m.poly(TRIM, [(xo0, y0c, z1), (xo1, y1c, z1),
                      (xi1, y1c, z1), (xi0, y0c, z1)], (0.0, 0.0, -1.0))
        jambs = jambs + 4

    # Wheel-arch reveals: connect the outer and inner skins along the arc.
    reveals = 0
    for cz in WHEEL_Z:
        for k in range(8):
            a0 = math.pi * k / 8
            a1 = math.pi * (k + 1) / 8
            z0 = cz + math.cos(a0) * ARCH_R
            z1 = cz + math.cos(a1) * ARCH_R
            y0 = 0.38 + math.sin(a0) * ARCH_R
            y1 = 0.38 + math.sin(a1) * ARCH_R
            if y0 < Y0 or y1 < Y0:
                continue
            xo0, xo1 = wall_x(sign, y0), wall_x(sign, y1)
            xi0, xi1 = wall_xi(sign, y0), wall_xi(sign, y1)
            nx0, ny0 = 0.0, (0.38 - y0) / ARCH_R
            nx1, ny1 = 0.0, (0.38 - y1) / ARCH_R
            m.poly(TRIM, [(xo0, y0, z0), (xi0, y0, z0),
                          (xi1, y1, z1), (xo1, y1, z1)],
                   (0.0, (ny0 + ny1) * 0.5, ((cz - z0) + (cz - z1)) * 0.5 / ARCH_R))
            reveals = reveals + 1

    # Windows: recessed tinted glass + protruding frame ring.
    for (z0, z1, y0, y1) in windows:
        m.box(GLASS, sign * 1.145, (y0 + y1) * 0.5, (z0 + z1) * 0.5,
              0.03, y1 - y0, z1 - z0)
        fx = sign * 1.195
        m.box(TRIM, fx, y0, (z0 + z1) * 0.5, 0.05, 0.06, z1 - z0 + 0.10)
        m.box(TRIM, fx, y1, (z0 + z1) * 0.5, 0.05, 0.06, z1 - z0 + 0.10)
        m.box(TRIM, fx, (y0 + y1) * 0.5, z0, 0.05, y1 - y0 + 0.10, 0.06)
        m.box(TRIM, fx, (y0 + y1) * 0.5, z1, 0.05, y1 - y0 + 0.10, 0.06)
        m.box(TRIM, fx, (y0 + y1) * 0.5, (z0 + z1) * 0.5,
              0.04, 0.035, z1 - z0)  # centre mullion
    return { "quads": quads, "inner": inner, "jambs": jambs, "reveals": reveals }


def wheel(m, sign, cz):
    cx = sign * 1.26
    seg = 16
    half = 0.12
    r, ri = WHEEL_R, 0.24
    for i in range(seg):
        a0 = 2 * math.pi * i / seg
        a1 = 2 * math.pi * (i + 1) / seg
        y0, z0 = math.cos(a0) * r, math.sin(a0) * r
        y1, z1 = math.cos(a1) * r, math.sin(a1) * r
        # tread band
        n = (0.0, math.cos((a0 + a1) * 0.5), math.sin((a0 + a1) * 0.5))
        m.quad(TIRE, (cx - half, 0.38 + y0, cz + z0),
               (cx + half, 0.38 + y0, cz + z0),
               (cx + half, 0.38 + y1, cz + z1),
               (cx - half, 0.38 + y1, cz + z1), n)
        # sidewalls (annulus between tyre and hub radius)
        for xs, sn in ((cx - half, -1.0), (cx + half, 1.0)):
            m.quad(TIRE, (xs, 0.38 + y0, cz + z0), (xs, 0.38 + y1, cz + z1),
                   (xs, 0.38 + y1 * ri / r, cz + z1 * ri / r),
                   (xs, 0.38 + y0 * ri / r, cz + z0 * ri / r), (sn, 0, 0))
    # hub cap disc on the outer face
    for i in range(seg):
        a0 = 2 * math.pi * i / seg
        a1 = 2 * math.pi * (i + 1) / seg
        xs = cx + sign * (half + 0.015)
        p0 = (xs, 0.38 + math.cos(a0) * ri, cz + math.sin(a0) * ri)
        p1 = (xs, 0.38 + math.cos(a1) * ri, cz + math.sin(a1) * ri)
        m.quad(HUB, (xs, 0.38, cz), p0, p1, (xs, 0.38, cz), (sign, 0, 0))


def main():
    m = Mesh()
    # Hidden material-0 primitive.
    m.box(DEFAULT, 0, -0.60, 0, 0.02, 0.02, 0.02)

    # ---- Floor / rear wall / front cab ----
    m.box(BODY, 0, 0.275, 0.5, 2.36, 0.05, 9.0)
    m.box(BODY, 0, (Y0 + Y1) * 0.5, ZB, 2.36, Y1 - Y0, 0.08)
    # rear window + frame ring
    m.box(GLASS, 0, 1.45, ZB - 0.05, 1.30, 0.55, 0.03)
    m.box(TRIM, 0, 1.13, ZB - 0.045, 1.44, 0.07, 0.04)
    m.box(TRIM, 0, 1.77, ZB - 0.045, 1.44, 0.07, 0.04)
    m.box(TRIM, -0.68, 1.45, ZB - 0.045, 0.07, 0.71, 0.04)
    m.box(TRIM, 0.68, 1.45, ZB - 0.045, 0.07, 0.71, 0.04)
    m.box(TRIM, 0, 1.45, ZB - 0.03, 0.06, 0.55, 0.02)

    # Front cab: solid lower panel + sloped windshield + grille/bumper.
    m.box(BODY, 0, 0.70, 5.50, 2.36, 0.90, 0.12)
    m.quad(GLASS, (-1.06, 1.14, 5.54), (1.06, 1.14, 5.54),
           (1.06, 2.04, 4.92), (-1.06, 2.04, 4.92), (0.0, 0.30, 0.95))
    m.quad(GLASS, (1.06, 1.14, 5.54), (-1.06, 1.14, 5.54),
           (-1.06, 2.04, 4.92), (1.06, 2.04, 4.92), (0.0, -0.30, -0.95))
    # windshield frame: bottom/top rails + A pillars
    m.box(TRIM, 0, 1.13, 5.55, 2.20, 0.06, 0.06)
    m.box(TRIM, 0, 2.04, 4.93, 2.20, 0.06, 0.06)
    for sign in (-1, 1):
        m.quad(TRIM, (sign * 1.10, 1.13, 5.55), (sign * 1.10, 1.13, 5.50),
               (sign * 1.10, 2.04, 4.88), (sign * 1.10, 2.04, 4.93),
               (sign, 0, 0))
    # grille + headlights + bumper
    m.box(TRIM, 0, 0.86, 5.565, 1.50, 0.32, 0.04)
    for i in range(3):
        m.box(HUB, 0, 0.76 + i * 0.10, 5.585, 1.40, 0.03, 0.02)
    m.box(TRIM, 0, 0.58, 5.66, 2.34, 0.30, 0.20)

    # ---- Roof slab + beveled edges ----
    m.box(ROOF, 0, 2.15, 0.45, 2.24, 0.10, 8.95)
    for sign in (-1, 1):
        # Lofted roof shoulder: three strips round the wall-to-roof transition
        # instead of one 45-degree chamfer.
        for j in range(len(ROOF_SHOULDER) - 1):
            xa, ya = ROOF_SHOULDER[j]
            xb, yb = ROOF_SHOULDER[j + 1]
            m.poly(ROOF, [
                (sign * xa, ya, ZB + 0.05), (sign * xa, ya, 4.92),
                (sign * xb, yb, 4.92), (sign * xb, yb, ZB + 0.05),
            ], (sign * 0.55, 0.84, 0.0))
    m.quad(ROOF, (-1.10, 2.10, 4.92), (1.10, 2.10, 4.92),
           (1.14, 2.15, 4.60), (-1.14, 2.15, 4.60), (0, 0.75, 0.66))
    m.quad(ROOF, (-1.14, 2.15, -3.96), (1.14, 2.15, -3.96),
           (1.10, 2.10, -4.0), (-1.10, 2.10, -4.0), (0, 0.75, -0.66))

    # ---- Side skirts (beveled lower edge) ----
    for sign in (-1, 1):
        m.quad(BODY, (sign * X, 0.25, ZB + 0.05), (sign * 1.08, 0.18, ZB + 0.05),
               (sign * 1.08, 0.18, 4.95), (sign * X, 0.25, 4.95), (sign * 0.7, -0.7, 0))

    # ---- Side walls: door opening preserved on +X ----
    win = [(-2.25, -0.35, 0.98, 1.72), (0.25, 2.15, 0.98, 1.72),
           (3.75, 4.70, 1.05, 1.80)]
    wall = {"quads": 0, "inner": 0, "jambs": 0, "reveals": 0}
    for part in (
            side_wall(m, 1, [(-3.54, -2.76, 0.24, 1.86)], win),
            side_wall(m, -1, [],
                      [(-2.25, -0.35, 0.98, 1.72), (0.25, 2.15, 0.98, 1.72),
                       (-3.55, -2.60, 0.98, 1.72), (3.75, 4.70, 1.05, 1.80)])):
        for k in wall:
            wall[k] += part[k]
    wall_quads = wall["quads"]
    for sign in (-1, 1):
        for cz in WHEEL_Z:
            arch_lip(m, sign, cz)
            arch_well(m, sign, cz)

    # Door frame + handle + hinge plates (right side, around z -3.54..-2.76).
    fx = X + 0.02
    m.box(TRIM, fx, 1.92, -3.15, 0.05, 0.14, 0.92)
    m.box(TRIM, fx, 1.05, -3.56, 0.05, 1.70, 0.06)
    m.box(TRIM, fx, 0.30, -3.15, 0.05, 0.12, 0.92)
    m.box(HUB, fx + 0.01, 1.15, -3.44, 0.04, 0.10, 0.22)
    for hy in (0.55, 1.45):
        m.box(TRIM, fx + 0.01, hy, -3.52, 0.05, 0.10, 0.10)

    # ---- Wheels are separate entities (rv_wheel.gltf) so each one can follow
    #      its own terrain contact and steer; the arches/wells stay here. ----

    # ---- Bumpers, rub strips, lights, mirrors, exhaust ----
    m.box(TRIM, 0, 0.60, -4.08, 2.34, 0.30, 0.18)
    for sign in (-1, 1):
        m.box(TRIM, sign * 1.20, 0.82, 0.5, 0.05, 0.10, 8.8)
        m.box(TRIM, sign * 1.195, 1.86, 0.4, 0.04, 0.05, 8.7)
        # headlights
        m.box(LIGHT, sign * 0.82, 1.02, 5.57, 0.32, 0.18, 0.06)
        m.box(TRIM, sign * 0.82, 1.02, 5.585, 0.36, 0.22, 0.02)
        # taillights
        m.box(TAIL, sign * 0.86, 1.12, -4.04, 0.30, 0.24, 0.05)
        # mirrors
        m.box(TRIM, sign * 1.26, 1.55, 4.62, 0.20, 0.05, 0.05)
        m.box(TRIM, sign * 1.36, 1.52, 4.60, 0.06, 0.28, 0.16)
        m.box(GLASS, sign * 1.375, 1.52, 4.60, 0.02, 0.20, 0.12)
    m.box(HUB, 0, 0.94, 5.60, 0.55, 0.06, 0.03)
    m.box(TRIM, -0.95, 0.35, -4.05, 0.12, 0.12, 0.12)
    m.box(HUB, -0.95, 0.30, -4.12, 0.08, 0.08, 0.06)

    # ---- Roof rack, AC unit, ladder, awning case ----
    for sign in (-1, 1):
        m.box(TRIM, sign * 0.86, 2.25, 0.5, 0.07, 0.10, 8.4)
    for z in (-3.4, -1.6, 0.2, 2.0, 3.8):
        m.box(TRIM, 0, 2.25, z, 1.78, 0.06, 0.07)
    m.box(ROOF, 0.12, 2.34, -0.6, 1.05, 0.28, 1.60)
    for i in range(4):
        m.box(TRIM, -0.20 + i * 0.28, 2.49, -0.6, 0.16, 0.02, 1.30)
    m.box(BODY, 1.20, 2.00, -0.4, 0.18, 0.16, 5.4)  # awning case
    # rear ladder
    for sign in (-1, 1):
        m.box(TRIM, 0.62 + sign * 0.10, 1.35, -4.05, 0.05, 1.65, 0.05)
    for i in range(5):
        m.box(TRIM, 0.62, 0.62 + i * 0.34, -4.05, 0.26, 0.04, 0.04)

    # ---- Interior: floor, dashboard, steering wheel, seat, cabinets ----
    m.box(FABRIC, 0, 0.30, 0.6, 2.16, 0.04, 8.7)
    m.box(TRIM, 0, 0.92, 4.52, 2.12, 0.30, 0.34)
    m.box(HUB, 0, 1.02, 4.36, 0.44, 0.10, 0.05)
    m.box(GLASS, 0, 1.02, 4.375, 0.34, 0.07, 0.02)
    for sign in (-1, 1):
        m.box(TRIM, sign * 0.78, 0.96, 4.40, 0.30, 0.18, 0.12)
    m.box(TRIM, 0, 0.80, 4.58, 2.10, 0.10, 0.20)
    # steering column + faceted wheel
    m.box(TRIM, 0, 1.00, 4.18, 0.10, 0.10, 0.34)
    seg = 12
    cx, cy, cz = 0.0, 1.16, 4.02
    ro, ri = 0.19, 0.145
    for i in range(seg):
        a0 = 2 * math.pi * i / seg
        a1 = 2 * math.pi * (i + 1) / seg
        for dz, nz in ((-0.03, -1.0), (0.03, 1.0)):
            m.quad(TRIM,
                   (cx + math.cos(a0) * ri, cy + math.sin(a0) * ri, cz + dz),
                   (cx + math.cos(a0) * ro, cy + math.sin(a0) * ro, cz + dz),
                   (cx + math.cos(a1) * ro, cy + math.sin(a1) * ro, cz + dz),
                   (cx + math.cos(a1) * ri, cy + math.sin(a1) * ri, cz + dz),
                   (0, 0, nz))
        m.quad(TRIM,
               (cx + math.cos(a0) * ro, cy + math.sin(a0) * ro, cz - 0.03),
               (cx + math.cos(a0) * ro, cy + math.sin(a0) * ro, cz + 0.03),
               (cx + math.cos(a1) * ro, cy + math.sin(a1) * ro, cz + 0.03),
               (cx + math.cos(a1) * ro, cy + math.sin(a1) * ro, cz - 0.03),
               (math.cos((a0 + a1) * 0.5), math.sin((a0 + a1) * 0.5), 0))
    m.box(TRIM, 0, 1.16, 4.02, 0.30, 0.04, 0.03)
    # driver seat (multi-part: base, cushion, backrest, bolsters, headrest, rails)
    m.box(TRIM, 0, 0.36, 3.15, 0.62, 0.08, 0.60)
    m.box(FABRIC, 0, 0.48, 3.15, 0.56, 0.16, 0.56)
    m.box(FABRIC, 0, 0.82, 3.42, 0.56, 0.52, 0.14)
    for sign in (-1, 1):
        m.box(FABRIC, sign * 0.28, 0.78, 3.15, 0.07, 0.22, 0.52)
    m.box(FABRIC, 0, 1.16, 3.44, 0.30, 0.22, 0.12)
    m.box(TRIM, 0, 0.62, 3.15, 0.06, 0.30, 0.06)
    # overhead cabinets along both walls (doors + handles)
    for sign in (-1, 1):
        m.box(WOOD, sign * 0.94, 1.90, -0.45, 0.26, 0.34, 6.0)
        for z in (-2.9, -1.5, -0.1, 1.3, 2.7):
            m.box(WOOD, sign * 0.82, 1.90, z, 0.02, 0.28, 1.30)
            m.box(TRIM, sign * 0.80, 1.86, z + 0.52, 0.03, 0.04, 0.16)
    # cab-over shelf
    m.box(WOOD, 0, 1.92, 4.55, 2.00, 0.10, 0.80)
    m.box(TRIM, 0, 2.02, 4.55, 1.96, 0.06, 0.06)

    materials = [
        {"name": "body_default",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "body",
         "pbrMetallicRoughness": {"baseColorFactor": [0.87, 0.84, 0.75, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.55}},
        {"name": "roof",
         "pbrMetallicRoughness": {"baseColorFactor": [0.70, 0.70, 0.68, 1],
                                  "metallicFactor": 0.08, "roughnessFactor": 0.6}},
        {"name": "trim",
         "pbrMetallicRoughness": {"baseColorFactor": [0.13, 0.14, 0.15, 1],
                                  "metallicFactor": 0.25, "roughnessFactor": 0.5}},
        {"name": "glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.03, 0.05, 0.07, 1],
                                  "metallicFactor": 0.4, "roughnessFactor": 0.18}},
        {"name": "tire",
         "pbrMetallicRoughness": {"baseColorFactor": [0.07, 0.07, 0.08, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
        {"name": "hub",
         "pbrMetallicRoughness": {"baseColorFactor": [0.62, 0.64, 0.66, 1],
                                  "metallicFactor": 0.65, "roughnessFactor": 0.35}},
        {"name": "light",
         "pbrMetallicRoughness": {"baseColorFactor": [0.95, 0.92, 0.78, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.3}},
        {"name": "tail",
         "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.07, 0.05, 1],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.4}},
        {"name": "door",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.78, 0.70, 1],
                                  "metallicFactor": 0.05, "roughnessFactor": 0.6}},
        {"name": "wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.46, 0.31, 0.17, 1],
                                  "metallicFactor": 0.02, "roughnessFactor": 0.8}},
        {"name": "fabric",
         "pbrMetallicRoughness": {"baseColorFactor": [0.30, 0.33, 0.38, 1],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.95}},
    ]
    for mat, tex in zip(materials, (
            None, "body", "roof", "metal_dark", "glass", "rubber", "metal",
            "plastic", "plastic", "body", "wood_light", "fabric")):
        surface_material(mat, tex)
    # Self-glow for the head- and tail-lights (dedicated emissive maps).
    surface_emissive(materials[7], "lamp")
    surface_emissive(materials[8], "tail")

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
        "asset": {"version": "2.0", "generator": "neon rv body generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Body", "mesh": 0}],
        "meshes": [{"name": "RV_Body", "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": "rv_body.bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    gltf.update(surface_extras())
    (OUT / "rv_body.bin").write_bytes(buffer)
    (OUT / "rv_body.gltf").write_text(
        json.dumps(gltf, separators=(",", ":")), encoding="utf-8")
    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/rv_body.gltf",
                     "castShadow": True, "receiveShadow": True},
        },
    }, indent=2) + "\n", encoding="utf-8")
    verts = sum(len(v[0]) for v in m.groups.values())
    tris = sum(len(v[2]) // 3 for v in m.groups.values())
    STATS.parent.mkdir(parents=True, exist_ok=True)
    STATS.write_text(json.dumps({
        "wallQuads": wall_quads,
        "wallInnerQuads": wall["inner"],
        "wallThickness": WALL_T,
        "wallJambs": wall["jambs"],
        "wallArchReveals": wall["reveals"],
        "wallHoles": 12,
        "wallCurve": WALL_CURVE,
        "archLipSegments": ARCH_LIP_SEGMENTS,
        "archArcs": 4,
        "wheelWellSegments": ARCH_LIP_SEGMENTS,
        "wheelWellArcs": 4,
        "roofShoulderStrips": len(ROOF_SHOULDER) - 1,
        "verts": verts,
        "tris": tris,
        "prims": len(prims),
    }, indent=2) + "\n", encoding="utf-8")
    print("rv_body bytes", len(buffer), "prims", len(prims),
          "verts", verts, "tris", tris,
          "archSegments", ARCH_LIP_SEGMENTS,
          "roofStrips", len(ROOF_SHOULDER) - 1,
          "wallQuads", wall_quads,
          "jambs", wall["jambs"], "reveals", wall["reveals"])


if __name__ == "__main__":
    main()
