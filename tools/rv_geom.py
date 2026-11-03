#!/usr/bin/env python3
"""Shared geometry helpers for the authored RV body and furniture meshes.

`bevel_box` builds a box with 45-degree chamfers on all 12 edges and 8 corners
(6 inset faces + 12 edge strips + 8 corner triangles) so silhouettes stop
reading as hard cubes. It talks to the generator Mesh classes through their
`poly(mat, verts, normal)` entry point.
"""


def _newell(verts):
    nx = ny = nz = 0.0
    n = len(verts)
    for i in range(n):
        x0, y0, z0 = verts[i]
        x1, y1, z1 = verts[(i + 1) % n]
        nx += (y0 - y1) * (z0 + z1)
        ny += (z0 - z1) * (x0 + x1)
        nz += (x0 - x1) * (y0 + y1)
    return nx, ny, nz


def _add_poly(m, mat, verts, expected):
    gx, gy, gz = _newell(verts)
    if gx * expected[0] + gy * expected[1] + gz * expected[2] < 0:
        verts = list(reversed(verts))
    m.poly(mat, verts, expected)


def _rect(h, b, axis, sign):
    """Inset rectangle on one face, in local coordinates."""
    hx, hy, hz = h
    if axis == 0:
        x = sign * hx
        return [(x, -(hy - b), -(hz - b)), (x, (hy - b), -(hz - b)),
                (x, (hy - b), (hz - b)), (x, -(hy - b), (hz - b))]
    if axis == 1:
        y = sign * hy
        return [(-(hx - b), y, -(hz - b)), ((hx - b), y, -(hz - b)),
                ((hx - b), y, (hz - b)), (-(hx - b), y, (hz - b))]
    z = sign * hz
    return [(-(hx - b), -(hy - b), z), ((hx - b), -(hy - b), z),
            ((hx - b), (hy - b), z), (-(hx - b), (hy - b), z)]


def _corner_points(h, b, signs):
    """Three points that replace one cube corner with a chamfer triangle."""
    pts = []
    for a in range(3):
        q = [0.0, 0.0, 0.0]
        q[a] = signs[a] * h[a]
        for o in range(3):
            if o != a:
                q[o] = signs[o] * (h[o] - b)
        pts.append(tuple(q))
    return pts


def _norm(v):
    ln = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    if ln < 1e-9:
        return (0.0, 1.0, 0.0)
    return (v[0] / ln, v[1] / ln, v[2] / ln)


def bevel_box(m, mat, cx, cy, cz, sx, sy, sz, bevel=None):
    """Add a chamfered box; falls back to a plain box for thin parts."""
    h = [sx * 0.5, sy * 0.5, sz * 0.5]
    mdim = min(sx, sy, sz)
    b = min(0.025, mdim * 0.22) if bevel is None else bevel
    if mdim < 0.05 or b <= 1e-4:
        b = 0.0
    else:
        b = min(b, h[0] * 0.45, h[1] * 0.45, h[2] * 0.45)

    def pt(p):
        return (cx + p[0], cy + p[1], cz + p[2])

    # 6 inset faces.
    for axis in range(3):
        for s in (-1, 1):
            n = [0.0, 0.0, 0.0]
            n[axis] = s
            _add_poly(m, mat, [pt(v) for v in _rect(h, b, axis, s)], tuple(n))

    if b <= 1e-9:
        return

    # 12 edge strips: one quad per (axis pair, sign pair), spanning the third.
    for a in range(3):
        for bb in range(a + 1, 3):
            cc = 3 - a - bb
            for sa in (-1, 1):
                for sb in (-1, 1):
                    sign_plus = [0, 0, 0]
                    sign_plus[a] = sa
                    sign_plus[bb] = sb
                    sign_plus[cc] = 1
                    sign_minus = list(sign_plus)
                    sign_minus[cc] = -1
                    cp = _corner_points(h, b, sign_plus)
                    cm = _corner_points(h, b, sign_minus)
                    verts = [cm[a], cm[bb], cp[bb], cp[a]]
                    n = [0.0, 0.0, 0.0]
                    n[a] = sa
                    n[bb] = sb
                    _add_poly(m, mat, [pt(v) for v in verts], _norm(n))

    # 8 corner triangles.
    for sx_ in (-1, 1):
        for sy_ in (-1, 1):
            for sz_ in (-1, 1):
                cp = _corner_points(h, b, (sx_, sy_, sz_))
                _add_poly(m, mat, [pt(v) for v in cp], _norm((sx_, sy_, sz_)))
