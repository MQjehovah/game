# Generates NeonOps low-poly enemy/prop/weapon models as OBJ+MTL
# (per-part Kd colors, flat shading via face-normal fallback in the loader).
# Also emits the first-person arm/hand segments used by the two-bone IK rig.
# All models are Y-up. Ground units author their feet at y=-1.0 because the
# game script spawns entities at y=1 (hover bob around the spawn origin);
# flying units center their body on the origin.
# Run from the repo root: python tools/gen_fps_models.py
import math
import os

OUT = "projects/fps/assets/models"


class Mesh:
    def __init__(self):
        self.verts = []
        self.faces = []  # (v1, v2, v3, material)
        self.mats = {}   # name -> (r, g, b)

    def mat(self, name, rgb):
        self.mats[name] = rgb

    def quad(self, a, b, c, d, m):
        base = len(self.verts)
        for p in (a, b, c, d):
            self.verts.append(p)
        self.faces.append((base + 1, base + 2, base + 3, m))
        self.faces.append((base + 1, base + 3, base + 4, m))

    def tri(self, a, b, c, m):
        base = len(self.verts)
        for p in (a, b, c):
            self.verts.append(p)
        self.faces.append((base + 1, base + 2, base + 3, m))

    def box(self, cx, cy, cz, sx, sy, sz, m, yaw=0.0):
        """Axis-aligned box, optionally rotated around Y (yaw, radians).
        Vertex order: for dz in (-hz,hz) for dy in (-hy,hy) for dx in (-hx,hx).
        Quads are proper loops (no bowties) with outward-facing winding."""
        hx, hy, hz = sx / 2, sy / 2, sz / 2
        pts = []
        for dz in (-hz, hz):
            for dy in (-hy, hy):
                for dx in (-hx, hx):
                    x, z = dx, dz
                    if yaw:
                        x = dx * math.cos(yaw) + dz * math.sin(yaw)
                        z = -dx * math.sin(yaw) + dz * math.cos(yaw)
                    pts.append((cx + x, cy + dy, cz + z))
        # 0:(-x,-y,-z) 1:(+x,-y,-z) 2:(-x,+y,-z) 3:(+x,+y,-z)
        # 4:(-x,-y,+z) 5:(+x,-y,+z) 6:(-x,+y,+z) 7:(+x,+y,+z)
        self.quad(pts[4], pts[5], pts[7], pts[6], m)  # +z front
        self.quad(pts[1], pts[0], pts[2], pts[3], m)  # -z back
        self.quad(pts[5], pts[1], pts[3], pts[7], m)  # +x right
        self.quad(pts[0], pts[4], pts[6], pts[2], m)  # -x left
        self.quad(pts[6], pts[7], pts[3], pts[2], m)  # +y top
        self.quad(pts[0], pts[1], pts[5], pts[4], m)  # -y bottom

    def bevel_box(self, cx, cy, cz, sx, sy, sz, c, m):
        """Box with chamfered edges/corners (24 vertices, 26 faces). Gives
        receivers, grips, stocks and optic bodies a plausible machined edge
        instead of the hard toy-like cube silhouette."""
        hx, hy, hz = sx / 2, sy / 2, sz / 2
        c = min(c, hx * 0.45, hy * 0.45, hz * 0.45)
        if c <= 1e-5:
            self.box(cx, cy, cz, sx, sy, sz, m)
            return
        ix, iy, iz = hx - c, hy - c, hz - c
        center = (cx, cy, cz)
        faces = {
            '+x': [(hx, -iy, -iz), (hx, iy, -iz), (hx, iy, iz), (hx, -iy, iz)],
            '-x': [(-hx, iy, -iz), (-hx, -iy, -iz), (-hx, -iy, iz), (-hx, iy, iz)],
            '+y': [(-ix, hy, -iz), (-ix, hy, iz), (ix, hy, iz), (ix, hy, -iz)],
            '-y': [(-ix, -hy, iz), (-ix, -hy, -iz), (ix, -hy, -iz), (ix, -hy, iz)],
            '+z': [(-ix, -iy, hz), (ix, -iy, hz), (ix, iy, hz), (-ix, iy, hz)],
            '-z': [(ix, -iy, -hz), (-ix, -iy, -hz), (-ix, iy, -hz), (ix, iy, -hz)],
        }

        def sub(a, b):
            return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

        def cross(a, b):
            return (a[1] * b[2] - a[2] * b[1],
                    a[2] * b[0] - a[0] * b[2],
                    a[0] * b[1] - a[1] * b[0])

        def dot(a, b):
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

        def moved(p):
            return (p[0] + cx, p[1] + cy, p[2] + cz)

        def emit_quad(pts):
            n = cross(sub(pts[1], pts[0]), sub(pts[2], pts[1]))
            ctr = ((pts[0][0] + pts[1][0] + pts[2][0] + pts[3][0]) / 4,
                   (pts[0][1] + pts[1][1] + pts[2][1] + pts[3][1]) / 4,
                   (pts[0][2] + pts[1][2] + pts[2][2] + pts[3][2]) / 4)
            if dot(n, sub(ctr, center)) < 0:
                pts = [pts[3], pts[2], pts[1], pts[0]]
            self.quad(moved(pts[0]), moved(pts[1]), moved(pts[2]), moved(pts[3]), m)

        def emit_tri(pts):
            n = cross(sub(pts[1], pts[0]), sub(pts[2], pts[0]))
            ctr = ((pts[0][0] + pts[1][0] + pts[2][0]) / 3,
                   (pts[0][1] + pts[1][1] + pts[2][1]) / 3,
                   (pts[0][2] + pts[1][2] + pts[2][2]) / 3)
            if dot(n, sub(ctr, center)) < 0:
                pts = [pts[2], pts[1], pts[0]]
            self.tri(moved(pts[0]), moved(pts[1]), moved(pts[2]), m)

        for pts in faces.values():
            emit_quad(pts)

        inner = {'x': ix, 'y': iy, 'z': iz}
        axis_idx = {'x': 0, 'y': 1, 'z': 2}
        pairs = [
            ('+x', '+y', 'z'), ('+x', '-y', 'z'), ('-x', '+y', 'z'), ('-x', '-y', 'z'),
            ('+x', '+z', 'y'), ('+x', '-z', 'y'), ('-x', '+z', 'y'), ('-x', '-z', 'y'),
            ('+y', '+z', 'x'), ('+y', '-z', 'x'), ('-y', '+z', 'x'), ('-y', '-z', 'x'),
        ]
        for fa, fb, third in pairs:
            ax, sxa = fa[1], (1 if fa[0] == '+' else -1)
            bx, sxb = fb[1], (1 if fb[0] == '+' else -1)
            pa = [p for p in faces[fa] if abs(p[axis_idx[bx]] - sxb * inner[bx]) < 1e-6]
            pb = [p for p in faces[fb] if abs(p[axis_idx[ax]] - sxa * inner[ax]) < 1e-6]
            if len(pa) != 2 or len(pb) != 2:
                continue
            pa.sort(key=lambda p: p[axis_idx[third]])
            pb.sort(key=lambda p: p[axis_idx[third]])
            emit_quad([pa[0], pa[1], pb[1], pb[0]])

        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    fa = ('+' if sx > 0 else '-') + 'x'
                    fb = ('+' if sy > 0 else '-') + 'y'
                    fc = ('+' if sz > 0 else '-') + 'z'
                    pa = [p for p in faces[fa]
                          if abs(p[1] - sy * iy) < 1e-6 and abs(p[2] - sz * iz) < 1e-6][0]
                    pb = [p for p in faces[fb]
                          if abs(p[0] - sx * ix) < 1e-6 and abs(p[2] - sz * iz) < 1e-6][0]
                    pc = [p for p in faces[fc]
                          if abs(p[0] - sx * ix) < 1e-6 and abs(p[1] - sy * iy) < 1e-6][0]
                    emit_tri([pa, pb, pc])

    def cyl(self, cx, cy, cz, r_top, r_bot, h, seg, m, cap=True):
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            x0t, z0t = cx + r_top * math.cos(a0), cz + r_top * math.sin(a0)
            x1t, z1t = cx + r_top * math.cos(a1), cz + r_top * math.sin(a1)
            x0b, z0b = cx + r_bot * math.cos(a0), cz + r_bot * math.sin(a0)
            x1b, z1b = cx + r_bot * math.cos(a1), cz + r_bot * math.sin(a1)
            y0, y1 = cy - h / 2, cy + h / 2
            self.quad((x0b, y0, z0b), (x1b, y0, z1b), (x1t, y1, z1t), (x0t, y1, z0t), m)
            if cap:
                self.tri((cx, y1, cz), (x0t, y1, z0t), (x1t, y1, z1t), m)
                if r_bot > 0:
                    self.tri((cx, y0, cz), (x1b, y0, z1b), (x0b, y0, z0b), m)

    def tube_z(self, cz0, cz1, r0, r1, seg, m, cx=0.0, cy=0.0):
        """Tapered tube along Z (used by barrels, scopes and the FP arms).
        Ring 0 sits at cz0 (radius r0), ring 1 at cz1 (radius r1)."""
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            x0a, y0a = cx + r0 * math.cos(a0), cy + r0 * math.sin(a0)
            x1a, y1a = cx + r0 * math.cos(a1), cy + r0 * math.sin(a1)
            x0b, y0b = cx + r1 * math.cos(a0), cy + r1 * math.sin(a0)
            x1b, y1b = cx + r1 * math.cos(a1), cy + r1 * math.sin(a1)
            self.quad((x0a, y0a, cz0), (x0b, y0b, cz1),
                      (x1b, y1b, cz1), (x1a, y1a, cz0), m)
        # Caps (near cap faces +Z, far cap faces -Z).
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            self.tri((cx, cy, cz0),
                     (cx + r0 * math.cos(a0), cy + r0 * math.sin(a0), cz0),
                     (cx + r0 * math.cos(a1), cy + r0 * math.sin(a1), cz0), m)
            self.tri((cx, cy, cz1),
                     (cx + r1 * math.cos(a1), cy + r1 * math.sin(a1), cz1),
                     (cx + r1 * math.cos(a0), cy + r1 * math.sin(a0), cz1), m)

    def octa(self, cx, cy, cz, r, m):
        p = [(cx, cy + r, cz), (cx, cy - r, cz),
             (cx - r, cy, cz), (cx + r, cy, cz), (cx, cy, cz - r), (cx, cy, cz + r)]
        for top in (0, 1):
            ring = (2, 4, 3, 5) if top == 0 else (5, 3, 4, 2)
            for k in range(4):
                self.tri(p[top], p[ring[k]], p[ring[(k + 1) % 4]], m)

    def sphere(self, cx, cy, cz, r, seg_u, seg_v, m):
        rows = []
        for j in range(seg_v + 1):
            phi = math.pi * j / seg_v
            row = []
            for i in range(seg_u):
                th = 2 * math.pi * i / seg_u
                row.append((cx + r * math.sin(phi) * math.cos(th),
                            cy + r * math.cos(phi),
                            cz + r * math.sin(phi) * math.sin(th)))
            rows.append(row)
        for j in range(seg_v):
            for i in range(seg_u):
                j2 = i + 1 if i + 1 < seg_u else 0
                if j == 0:
                    self.tri(rows[0][0], rows[1][j2], rows[1][i], m)
                elif j == seg_v - 1:
                    self.tri(rows[j + 1][0], rows[j][i], rows[j][j2], m)
                else:
                    self.quad(rows[j][i], rows[j][j2], rows[j + 1][j2], rows[j + 1][i], m)

    def write(self, name, mtl=True):
        obj = os.path.join(OUT, name + ".obj")
        with open(obj, "w") as f:
            f.write("# %s.obj - generated by tools/gen_fps_models.py (y-up)\n" % name)
            if mtl:
                f.write("mtllib %s.mtl\n" % name)
            for v in self.verts:
                f.write("v %.4f %.4f %.4f\n" % v)
            if mtl:
                cur = None
                for a, b, c, m in self.faces:
                    if m != cur:
                        f.write("usemtl %s\n" % m)
                        cur = m
                    f.write("f %d %d %d\n" % (a, b, c))
        if mtl:
            with open(os.path.join(OUT, name + ".mtl"), "w") as f:
                f.write("# %s.mtl - generated\n" % name)
                for name_m, (r, g, b) in self.mats.items():
                    f.write("newmtl %s\nKd %.3f %.3f %.3f\nKa 0 0 0\nKs 0 0 0\n"
                            "illum 1\n\n" % (name_m, r, g, b))
        n = len(self.verts)
        print("  %-22s %4d verts %4d tris" % (name + ".obj", n, len(self.faces) * 2))


# --- palette -------------------------------------------------------------
STEEL = (0.32, 0.36, 0.42)
GUNMETAL = (0.22, 0.24, 0.28)
DARK = (0.13, 0.14, 0.17)
RED = (0.90, 0.18, 0.14)
ORANGE = (0.95, 0.55, 0.10)
AMBER = (0.98, 0.72, 0.15)
CYAN = (0.25, 0.85, 0.95)
CRIMSON = (0.75, 0.12, 0.22)
WHITE = (0.88, 0.90, 0.92)
OLIVE = (0.42, 0.46, 0.30)
BRASS = (0.80, 0.62, 0.25)
CONCRETE = (0.58, 0.60, 0.62)
YELLOW = (0.92, 0.78, 0.20)
PURPLE = (0.62, 0.25, 0.85)

# HDR emissive materials: components above 1.0 glow through the lit shader's
# tint self-glow path (tint - 1.0 emits, modulated by albedo) and bloom.
GLOW_RED = (1.0, 0.10, 0.08)        # tint 1.0 -> +0.0 red? no: tint stays, glow = tint-1
GLOW_RED = (1.6, 0.22, 0.12)        # red lamp
GLOW_CYAN = (0.30, 1.7, 1.9)        # cyan lamp
GLOW_AMBER = (1.9, 1.35, 0.25)      # amber lamp
GLOW_PURPLE = (1.45, 0.35, 1.9)     # violet lamp
GLOW_GREEN = (0.35, 1.8, 0.55)      # health glow


def gen_drone():
    m = Mesh()
    m.mat("body", STEEL); m.mat("dark", DARK); m.mat("eye", GLOW_RED)
    m.mat("rotor", GUNMETAL); m.mat("trim", CYAN)
    m.octa(0, 0, 0, 0.26, "body")                       # core
    m.box(0, -0.10, 0, 0.20, 0.14, 0.20, "dark")        # belly pod
    # 4 rotor arms in X shape
    for ax, az in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
        cx, cz = ax * 0.30, az * 0.30
        m.box(cx * 0.5, 0.02, cz * 0.5, 0.34, 0.05, 0.07, "dark",
              yaw=math.atan2(az, ax))
        m.cyl(cx, 0.06, cz, 0.16, 0.16, 0.03, 10, "rotor")   # rotor disc
        m.cyl(cx, 0.10, cz, 0.03, 0.03, 0.10, 6, "dark")     # hub
    m.box(0, 0.02, 0.24, 0.14, 0.10, 0.06, "eye")       # front eye (faces +z)
    m.box(0, 0.14, -0.16, 0.10, 0.04, 0.14, "trim")     # nav stripe
    m.write("enemy_drone")


def gen_turret():
    m = Mesh()
    m.mat("base", DARK); m.mat("steel", STEEL); m.mat("dome", GUNMETAL)
    m.mat("barrel", DARK); m.mat("acc", GLOW_RED); m.mat("ring", CYAN)
    # feet at y=-1.0 (entity spawns at y=1)
    m.cyl(0, -0.94, 0, 0.42, 0.50, 0.12, 8, "base")     # ground pad
    m.cyl(0, -0.78, 0, 0.26, 0.36, 0.28, 8, "steel")    # pedestal
    m.cyl(0, -0.52, 0, 0.34, 0.30, 0.10, 8, "ring")     # yaw ring accent
    m.sphere(0, -0.36, 0, 0.30, 10, 6, "dome")          # armored dome
    m.box(0, -0.36, 0.26, 0.16, 0.12, 0.10, "acc")      # sensor
    m.box(0.09, -0.30, 0.38, 0.07, 0.07, 0.34, "barrel")  # twin barrels (+z fwd)
    m.box(-0.09, -0.30, 0.38, 0.07, 0.07, 0.34, "barrel")
    m.box(0.09, -0.30, 0.56, 0.09, 0.09, 0.06, "acc")
    m.box(-0.09, -0.30, 0.56, 0.09, 0.09, 0.06, "acc")
    m.box(0.24, -0.44, 0, 0.10, 0.16, 0.22, "steel")    # shoulder armor
    m.box(-0.24, -0.44, 0, 0.10, 0.16, 0.22, "steel")
    m.write("enemy_turret")


def gen_sapper():
    m = Mesh()
    m.mat("shell", ORANGE); m.mat("band", DARK); m.mat("core", GLOW_AMBER)
    m.mat("fin", DARK)
    m.sphere(0, 0, 0, 0.26, 10, 6, "shell")
    m.cyl(0, 0.0, 0, 0.27, 0.27, 0.07, 10, "band")      # hazard band
    m.sphere(0, 0, 0.20, 0.09, 6, 4, "core")            # trigger core
    for a in (0, 2.09, 4.19):                            # 3 tail fins
        fx, fz = 0.28 * math.cos(a), 0.28 * math.sin(a)
        m.box(fx * 1.3, -0.05, fz * 1.3, 0.20, 0.03, 0.10, "fin",
              yaw=-a)
    m.cyl(0, -0.30, 0, 0.04, 0.04, 0.10, 6, "fin")      # ventral spike
    m.write("enemy_sapper")


def gen_heavy():
    m = Mesh()
    m.mat("armor", STEEL); m.mat("dark", DARK); m.mat("visor", GLOW_RED)
    m.mat("gun", GUNMETAL); m.mat("trim", GLOW_AMBER)
    # legs (feet at y=-1.0)
    for sx in (-1, 1):
        m.box(sx * 0.20, -0.72, 0, 0.22, 0.60, 0.26, "dark")
        m.box(sx * 0.20, -0.98, 0.04, 0.26, 0.12, 0.40, "armor")
        m.box(sx * 0.20, -0.42, 0, 0.26, 0.14, 0.30, "armor")  # knee guard
    # torso
    m.box(0, 0.02, 0, 0.66, 0.56, 0.42, "armor")
    m.box(0, 0.02, 0.20, 0.40, 0.30, 0.06, "dark")       # chest plate
    m.box(0, 0.16, 0.235, 0.26, 0.08, 0.03, "visor")     # chest sensor
    m.box(0, -0.16, 0.22, 0.30, 0.10, 0.04, "trim")      # belt light
    # head
    m.box(0, 0.40, 0.02, 0.30, 0.22, 0.30, "dark")
    m.box(0, 0.40, 0.16, 0.22, 0.10, 0.04, "visor")
    # shoulders + arm cannons
    for sx in (-1, 1):
        m.box(sx * 0.44, 0.16, 0, 0.24, 0.26, 0.34, "armor")
        m.box(sx * 0.44, -0.12, 0.10, 0.16, 0.34, 0.18, "gun")
        m.box(sx * 0.44, -0.30, 0.24, 0.10, 0.10, 0.10, "trim")
    m.write("enemy_heavy")


def gen_boss():
    m = Mesh()
    m.mat("armor", GUNMETAL); m.mat("dark", DARK); m.mat("visor", GLOW_PURPLE)
    m.mat("gun", STEEL); m.mat("acc", GLOW_PURPLE)
    # thick legs
    for sx in (-1, 1):
        m.box(sx * 0.34, -0.70, 0, 0.34, 0.56, 0.40, "dark")
        m.box(sx * 0.34, -0.97, 0.05, 0.40, 0.14, 0.58, "armor")
        m.box(sx * 0.34, -0.40, 0, 0.38, 0.16, 0.44, "armor")
    # massive torso
    m.box(0, 0.06, 0, 1.00, 0.66, 0.58, "armor")
    m.box(0, 0.06, 0.28, 0.56, 0.40, 0.08, "dark")
    m.box(0, 0.22, 0.325, 0.36, 0.10, 0.03, "visor")
    m.box(0, -0.14, 0.30, 0.44, 0.12, 0.05, "acc")
    # head
    m.box(0, 0.52, 0.02, 0.36, 0.26, 0.36, "dark")
    m.box(0, 0.52, 0.19, 0.26, 0.12, 0.04, "visor")
    # shoulders + heavy cannons
    for sx in (-1, 1):
        m.box(sx * 0.66, 0.22, 0, 0.32, 0.34, 0.44, "armor")
        m.box(sx * 0.66, -0.10, 0.14, 0.20, 0.42, 0.22, "gun")
        m.box(sx * 0.66, -0.32, 0.32, 0.13, 0.13, 0.14, "acc")
    # back thrusters
    for sx in (-1, 1):
        m.cyl(sx * 0.26, 0.10, -0.36, 0.09, 0.11, 0.30, 8, "dark")
    m.box(0, 0.44, -0.30, 0.70, 0.10, 0.10, "acc")       # top antenna bar
    m.write("enemy_boss")


def gen_barrier():
    m = Mesh()
    m.mat("concrete", CONCRETE); m.mat("stripe", YELLOW); m.mat("foot", DARK)
    # jersey barrier, feet at y=-1.0, 2.0 long (x), 0.45 deep, 1.0 tall
    m.box(0, -0.90, 0, 2.0, 0.24, 0.44, "concrete")      # base block
    m.box(0, -0.55, 0, 2.0, 0.50, 0.30, "concrete")      # mid
    m.box(0, -0.30, 0, 1.9, 0.10, 0.32, "stripe")        # hazard stripe
    m.box(0, -0.10, 0, 2.0, 0.34, 0.22, "concrete")      # top
    for sx in (-0.85, 0.85):
        m.box(sx, -0.98, 0, 0.20, 0.10, 0.52, "foot")    # feet
    m.write("cover_barrier")


def gen_pickups():
    m = Mesh()
    m.mat("case", WHITE); m.mat("cross", GLOW_GREEN); m.mat("edge", DARK)
    m.box(0, 0, 0, 0.34, 0.34, 0.22, "case")
    m.box(0, 0, 0.115, 0.20, 0.07, 0.02, "cross")
    m.box(0, 0, 0.115, 0.07, 0.20, 0.02, "cross")
    m.box(0, 0.185, 0, 0.36, 0.03, 0.24, "edge")
    m.box(0, -0.185, 0, 0.36, 0.03, 0.24, "edge")
    m.write("pickup_health")

    m = Mesh()
    m.mat("can", OLIVE); m.mat("lid", DARK); m.mat("tip", GLOW_AMBER)
    m.box(0, 0, 0, 0.38, 0.24, 0.24, "can")
    m.box(0, 0.135, 0, 0.40, 0.04, 0.26, "lid")
    for i in range(4):
        m.cyl(-0.12 + i * 0.08, 0.19, 0, 0.025, 0.025, 0.05, 6, "tip")
    m.write("pickup_ammo")


def gen_weapons():
    """Realistic first-person service weapons. Muzzle -Z, pistol grip near the
    origin; authored full-size and shrunk by the prefabs. Materials are matte
    aluminum, phosphate steel, black polymer, FDE furniture, brass and glass -
    no sci-fi glow trim (only the small optic dot is emissive)."""

    def bev(m, cx, cy, cz, sx, sy, sz, c, mat):
        m.bevel_box(cx, cy, cz, sx, sy, sz, c, mat)

    def rail(m, y, z0, z1, mat, step=0.028, width=0.050, height=0.013):
        lo, hi = min(z0, z1), max(z0, z1)
        m.box(0, y, (lo + hi) / 2, width, height, hi - lo, mat)
        count = max(1, int((hi - lo) / step))
        for i in range(count):
            z = lo + step * (i + 0.5)
            m.box(0, y + height * 0.55 + 0.003, z, width * 0.82, 0.007,
                  step * 0.42, mat)

    def rifle():
        m = Mesh()
        m.mat("alu", (0.225, 0.235, 0.250))
        m.mat("alu_dark", (0.120, 0.128, 0.140))
        m.mat("poly", (0.052, 0.055, 0.062))
        m.mat("poly_grey", (0.110, 0.115, 0.125))
        m.mat("steel", (0.160, 0.170, 0.190))
        m.mat("bore", (0.012, 0.012, 0.014))
        m.mat("lens", (0.035, 0.100, 0.115))
        m.mat("dot", (1.35, 0.09, 0.05))
        m.mat("mark", (0.620, 0.630, 0.660))

        # Receivers, magwell.
        bev(m, 0, 0.020, -0.10, 0.064, 0.072, 0.36, 0.007, "alu")
        bev(m, 0, -0.040, -0.02, 0.058, 0.075, 0.25, 0.008, "alu")
        bev(m, 0, -0.072, -0.155, 0.058, 0.070, 0.105, 0.006, "alu_dark")
        m.box(0, 0.056, -0.005, 0.050, 0.006, 0.16, "alu_dark")

        # Receiver + handguard top rails.
        rail(m, 0.061, -0.295, 0.060, "alu_dark")
        rail(m, 0.062, -0.695, -0.300, "alu_dark")

        # Free-float handguard with M-LOK slots.
        m.tube_z(-0.300, -0.700, 0.046, 0.043, 12, "poly_grey", cy=0.012)
        for z in (-0.36, -0.42, -0.48, -0.54, -0.60, -0.66):
            m.box(0.0455, 0.006, z, 0.006, 0.020, 0.034, "poly")
            m.box(-0.0455, 0.006, z, 0.006, 0.020, 0.034, "poly")
        m.tube_z(-0.300, -0.700, 0.047, 0.047, 12, "alu_dark", cy=0.012)
        m.tube_z(-0.302, -0.310, 0.045, 0.045, 12, "alu", cy=0.012)

        # Barrel, gas system, muzzle brake.
        m.tube_z(-0.700, -0.950, 0.0140, 0.0135, 10, "steel", cy=0.012)
        bev(m, 0, 0.046, -0.715, 0.038, 0.045, 0.055, 0.004, "steel")
        m.tube_z(-0.550, -0.715, 0.006, 0.006, 8, "steel", cy=0.044)
        m.tube_z(-0.950, -1.040, 0.021, 0.020, 10, "alu_dark", cy=0.012)
        for z in (-0.975, -1.000, -1.020):
            m.box(0.019, 0.012, z, 0.006, 0.016, 0.016, "bore")
            m.box(-0.019, 0.012, z, 0.006, 0.016, 0.016, "bore")
        m.tube_z(-1.035, -1.045, 0.012, 0.012, 10, "bore", cy=0.012)

        # Folding front sight and low-profile rear backup.
        bev(m, 0, 0.064, -0.690, 0.040, 0.016, 0.050, 0.003, "alu_dark")
        m.box(0, 0.085, -0.690, 0.006, 0.030, 0.012, "mark")
        m.box(0.014, 0.079, -0.690, 0.006, 0.024, 0.012, "alu_dark")
        m.box(-0.014, 0.079, -0.690, 0.006, 0.024, 0.012, "alu_dark")
        bev(m, 0, 0.062, 0.070, 0.040, 0.014, 0.045, 0.003, "alu_dark")

        # LPVO: mount, rings, tube, bells, lenses, turrets, red dot.
        bev(m, 0, 0.078, -0.100, 0.045, 0.028, 0.200, 0.004, "alu_dark")
        m.tube_z(-0.020, 0.000, 0.044, 0.044, 12, "alu_dark", cy=0.100)
        m.tube_z(-0.180, -0.160, 0.044, 0.044, 12, "alu_dark", cy=0.100)
        m.tube_z(-0.195, -0.005, 0.036, 0.036, 12, "poly", cy=0.100)
        m.tube_z(-0.245, -0.195, 0.050, 0.038, 12, "poly", cy=0.100)
        m.tube_z(0.035, -0.005, 0.048, 0.037, 12, "poly", cy=0.100)
        m.tube_z(-0.247, -0.242, 0.045, 0.045, 12, "lens", cy=0.100)
        m.tube_z(0.040, 0.035, 0.043, 0.043, 12, "lens", cy=0.100)
        m.cyl(0, 0.152, -0.100, 0.017, 0.017, 0.026, 10, "alu_dark")
        bev(m, 0.045, 0.100, -0.100, 0.022, 0.030, 0.030, 0.004, "alu_dark")
        m.sphere(0, 0.100, 0.043, 0.0035, 6, 4, "dot")

        # Charging handle, ejection port, forward assist, controls.
        bev(m, 0, 0.061, 0.100, 0.064, 0.016, 0.055, 0.003, "alu")
        m.box(-0.043, 0.061, 0.095, 0.020, 0.018, 0.035, "alu_dark")
        m.box(0.034, 0.018, -0.030, 0.006, 0.036, 0.085, "bore")
        bev(m, 0.040, 0.032, 0.020, 0.018, 0.024, 0.035, 0.003, "alu_dark")
        m.tube_z(0.045, 0.075, 0.012, 0.012, 10, "alu_dark", cx=0.040, cy=0.030)
        m.box(-0.032, -0.030, 0.018, 0.008, 0.020, 0.022, "alu_dark")
        m.box(0.032, -0.030, 0.018, 0.008, 0.020, 0.022, "alu_dark")
        m.box(-0.030, -0.020, 0.040, 0.002, 0.008, 0.030, "mark")

        # Trigger group.
        m.box(0, -0.082, 0.070, 0.046, 0.010, 0.100, "alu_dark")
        m.box(0, -0.055, 0.118, 0.046, 0.055, 0.010, "alu_dark")
        m.box(0, -0.055, 0.075, 0.008, 0.035, 0.012, "steel")
        m.box(0, -0.066, 0.035, 0.006, 0.010, 0.030, "alu_dark")

        # Raked pistol grip + beavertail.
        bev(m, 0, -0.066, 0.002, 0.055, 0.034, 0.075, 0.004, "poly")
        bev(m, 0, -0.106, 0.018, 0.050, 0.075, 0.076, 0.005, "poly")
        bev(m, 0, -0.154, 0.034, 0.046, 0.070, 0.068, 0.005, "poly")
        bev(m, 0, -0.198, 0.048, 0.048, 0.030, 0.060, 0.004, "poly")
        for i in range(4):
            m.box(0.026, -0.105 - i * 0.025, 0.030 + i * 0.006, 0.002,
                  0.016, 0.050, "poly_grey")
            m.box(-0.026, -0.105 - i * 0.025, 0.030 + i * 0.006, 0.002,
                  0.016, 0.050, "poly_grey")

        # 30-round curved magazine.
        bev(m, 0, -0.112, -0.168, 0.052, 0.070, 0.093, 0.005, "poly")
        bev(m, 0, -0.178, -0.175, 0.052, 0.064, 0.091, 0.005, "poly")
        bev(m, 0, -0.238, -0.184, 0.052, 0.062, 0.089, 0.005, "poly")
        bev(m, 0, -0.290, -0.196, 0.054, 0.050, 0.087, 0.004, "poly")
        bev(m, 0, -0.320, -0.202, 0.060, 0.020, 0.094, 0.003, "poly_grey")
        for i in range(3):
            m.box(0, -0.150 - i * 0.050, -0.126 + i * 0.008, 0.054, 0.006,
                  0.006, "poly_grey")

        # Buffer tube + collapsible stock.
        m.tube_z(0.100, 0.330, 0.026, 0.026, 12, "alu_dark", cy=0.010)
        m.tube_z(0.092, 0.105, 0.032, 0.032, 12, "alu", cy=0.010)
        bev(m, 0, 0.005, 0.270, 0.055, 0.085, 0.160, 0.006, "poly")
        bev(m, 0, 0.055, 0.290, 0.050, 0.028, 0.100, 0.004, "poly_grey")
        bev(m, 0, -0.005, 0.355, 0.060, 0.105, 0.030, 0.004, "poly_grey")
        m.box(0.030, -0.040, 0.280, 0.010, 0.020, 0.040, "alu_dark")
        m.tube_z(0.300, 0.315, 0.012, 0.012, 8, "steel", cx=-0.032, cy=0.000)
        return m

    def shotgun():
        m = Mesh()
        m.mat("recv", (0.105, 0.110, 0.120))
        m.mat("barrel", (0.155, 0.162, 0.175))
        m.mat("poly", (0.055, 0.057, 0.062))
        m.mat("fde", (0.360, 0.300, 0.210))
        m.mat("fde_dark", (0.250, 0.205, 0.145))
        m.mat("steel", (0.180, 0.188, 0.200))
        m.mat("brass", (0.630, 0.475, 0.185))
        m.mat("hull", (0.520, 0.070, 0.055))
        m.mat("bore", (0.010, 0.010, 0.012))
        m.mat("bead", (0.800, 0.620, 0.250))

        # Receiver and loading/ejection ports.
        bev(m, 0, -0.005, -0.060, 0.068, 0.095, 0.300, 0.009, "recv")
        m.box(0.035, 0.006, -0.030, 0.006, 0.040, 0.105, "bore")
        m.box(-0.030, -0.030, -0.020, 0.006, 0.030, 0.080, "bore")
        bev(m, 0, 0.050, -0.055, 0.050, 0.012, 0.160, 0.003, "recv")

        # Barrel + magazine tube + heat shield rings.
        m.tube_z(-0.210, -0.860, 0.021, 0.020, 12, "barrel", cy=0.030)
        m.tube_z(-0.210, -0.780, 0.017, 0.017, 12, "barrel", cy=-0.035)
        m.tube_z(-0.780, -0.795, 0.021, 0.021, 12, "recv", cy=-0.035)
        for z in (-0.360, -0.405, -0.450, -0.495, -0.540, -0.585,
                  -0.630, -0.675, -0.720):
            m.tube_z(z, z - 0.018, 0.029, 0.029, 12, "barrel", cy=0.030)
        for z in (-0.400, -0.480, -0.560, -0.640, -0.700):
            m.box(0.0, 0.052, z, 0.040, 0.006, 0.030, "bore")

        # Pump forend with ribs.
        bev(m, 0, -0.035, -0.550, 0.078, 0.066, 0.170, 0.008, "fde")
        for i in range(6):
            m.box(0, -0.035, -0.620 + i * 0.028, 0.082, 0.056, 0.010,
                  "fde_dark")
        m.box(0.040, -0.010, -0.440, 0.007, 0.014, 0.180, "steel")
        m.box(-0.040, -0.010, -0.440, 0.007, 0.014, 0.180, "steel")

        # Trigger group and wrist.
        m.box(0, -0.082, 0.080, 0.046, 0.010, 0.100, "recv")
        m.box(0, -0.055, 0.128, 0.046, 0.055, 0.010, "recv")
        m.box(0, -0.055, 0.082, 0.008, 0.035, 0.012, "steel")
        bev(m, 0, -0.105, 0.040, 0.050, 0.085, 0.075, 0.005, "fde")
        bev(m, 0, -0.155, 0.058, 0.046, 0.065, 0.068, 0.005, "fde")

        # Stock + recoil pad + comb.
        bev(m, 0, -0.005, 0.190, 0.058, 0.095, 0.240, 0.008, "fde")
        bev(m, 0, 0.058, 0.250, 0.050, 0.030, 0.120, 0.004, "fde")
        bev(m, 0, -0.005, 0.335, 0.062, 0.110, 0.030, 0.004, "fde_dark")
        m.box(0, -0.050, 0.200, 0.046, 0.006, 0.180, "fde_dark")

        # Side saddle: four 12-gauge shells.
        for i in range(4):
            z = -0.090 + i * 0.052
            m.cyl(-0.044, -0.005, z, 0.012, 0.012, 0.045, 8, "brass")
            m.cyl(-0.044, 0.021, z, 0.012, 0.012, 0.015, 8, "hull")
        m.box(-0.044, -0.036, -0.012, 0.024, 0.010, 0.210, "poly")

        # Ghost-ring rear + front bead.
        m.box(0, 0.068, 0.060, 0.040, 0.010, 0.016, "recv")
        m.box(0.016, 0.080, 0.060, 0.006, 0.024, 0.012, "recv")
        m.box(-0.016, 0.080, 0.060, 0.006, 0.024, 0.012, "recv")
        m.box(0, 0.090, 0.060, 0.038, 0.006, 0.012, "recv")
        m.sphere(0, 0.058, -0.828, 0.0065, 8, 5, "bead")
        m.box(0, 0.050, -0.828, 0.020, 0.018, 0.012, "recv")

        # Controls, sling loops.
        m.box(0.036, -0.030, 0.030, 0.008, 0.018, 0.022, "steel")
        m.box(-0.032, -0.020, 0.050, 0.008, 0.018, 0.024, "steel")
        m.tube_z(-0.030, -0.015, 0.012, 0.012, 8, "steel", cx=-0.040, cy=-0.055)
        m.tube_z(0.300, 0.315, 0.012, 0.012, 8, "steel", cx=-0.032, cy=0.000)
        return m

    def sniper():
        m = Mesh()
        m.mat("chassis", (0.135, 0.142, 0.152))
        m.mat("chassis_l", (0.195, 0.205, 0.218))
        m.mat("poly", (0.050, 0.052, 0.058))
        m.mat("fde", (0.345, 0.288, 0.205))
        m.mat("fde_dark", (0.235, 0.192, 0.135))
        m.mat("steel", (0.165, 0.175, 0.192))
        m.mat("alu_dark", (0.105, 0.112, 0.122))
        m.mat("bore", (0.010, 0.010, 0.012))
        m.mat("lens", (0.030, 0.085, 0.105))
        m.mat("dot", (1.30, 0.08, 0.05))
        m.mat("mark", (0.620, 0.630, 0.655))

        # Skeletonized chassis + forend.
        bev(m, 0, 0.000, -0.140, 0.070, 0.100, 0.560, 0.010, "chassis")
        bev(m, 0, 0.010, -0.560, 0.060, 0.075, 0.260, 0.008, "chassis")
        m.box(0.037, 0.000, -0.140, 0.006, 0.045, 0.160, "bore")
        m.box(-0.037, 0.000, -0.140, 0.006, 0.045, 0.160, "bore")
        m.box(0.032, 0.012, -0.560, 0.006, 0.026, 0.090, "bore")
        m.box(-0.032, 0.012, -0.560, 0.006, 0.026, 0.090, "bore")
        rail(m, 0.058, -0.690, -0.430, "alu_dark")
        for z in (-0.500, -0.560, -0.620, -0.670):
            m.box(0.031, -0.010, z, 0.006, 0.020, 0.034, "bore")
            m.box(-0.031, -0.010, z, 0.006, 0.020, 0.034, "bore")

        # Heavy barrel, flutes, muzzle brake.
        m.tube_z(-0.660, -1.000, 0.022, 0.020, 12, "steel", cy=0.022)
        for i in range(4):
            a = i * 3.14159 / 2
            m.box(0.020 * math.cos(a), 0.022 + 0.020 * math.sin(a), -0.830,
                  0.005, 0.005, 0.260, "alu_dark")
        m.tube_z(-1.000, -1.100, 0.030, 0.028, 12, "alu_dark", cy=0.022)
        for z in (-1.025, -1.055, -1.085):
            m.box(0.026, 0.022, z, 0.008, 0.020, 0.020, "bore")
            m.box(-0.026, 0.022, z, 0.008, 0.020, 0.020, "bore")
        m.tube_z(-1.095, -1.105, 0.014, 0.014, 10, "bore", cy=0.022)

        # Scope: rail base, rings, tube, bells, lenses, turrets.
        bev(m, 0, 0.076, -0.300, 0.048, 0.030, 0.300, 0.004, "alu_dark")
        m.tube_z(-0.180, -0.158, 0.055, 0.055, 12, "alu_dark", cy=0.125)
        m.tube_z(-0.420, -0.398, 0.055, 0.055, 12, "alu_dark", cy=0.125)
        m.tube_z(-0.460, -0.020, 0.046, 0.046, 12, "poly", cy=0.125)
        m.tube_z(-0.530, -0.460, 0.060, 0.048, 12, "poly", cy=0.125)
        m.tube_z(0.060, -0.020, 0.058, 0.047, 12, "poly", cy=0.125)
        m.tube_z(-0.532, -0.526, 0.055, 0.055, 12, "lens", cy=0.125)
        m.tube_z(0.065, 0.060, 0.052, 0.052, 12, "lens", cy=0.125)
        m.cyl(0, 0.184, -0.300, 0.020, 0.020, 0.030, 10, "alu_dark")
        bev(m, 0.060, 0.125, -0.300, 0.024, 0.032, 0.032, 0.004, "alu_dark")
        m.box(0.075, 0.125, -0.300, 0.010, 0.045, 0.010, "alu_dark")
        m.sphere(0, 0.125, 0.068, 0.0035, 6, 4, "dot")

        # Bolt handle + shroud.
        m.tube_z(0.075, 0.105, 0.020, 0.020, 10, "steel")
        m.box(0.060, 0.030, 0.040, 0.060, 0.018, 0.020, "steel")
        m.sphere(0.094, 0.030, 0.040, 0.016, 8, 5, "steel")

        # Magazine + baseplate.
        bev(m, 0, -0.108, -0.200, 0.055, 0.100, 0.090, 0.005, "alu_dark")
        bev(m, 0, -0.162, -0.205, 0.060, 0.020, 0.096, 0.003, "chassis_l")

        # Trigger group and raked grip.
        m.box(0, -0.090, 0.070, 0.046, 0.010, 0.100, "chassis")
        m.box(0, -0.062, 0.118, 0.046, 0.050, 0.010, "chassis")
        m.box(0, -0.062, 0.075, 0.008, 0.032, 0.012, "steel")
        bev(m, 0, -0.072, 0.002, 0.055, 0.035, 0.075, 0.004, "chassis")
        bev(m, 0, -0.112, 0.018, 0.050, 0.075, 0.075, 0.005, "fde")
        bev(m, 0, -0.160, 0.034, 0.046, 0.070, 0.068, 0.005, "fde")
        bev(m, 0, -0.204, 0.048, 0.048, 0.030, 0.060, 0.004, "fde_dark")

        # Skeleton stock, cheek riser, butt pad.
        m.tube_z(0.120, 0.335, 0.015, 0.015, 10, "steel", cy=0.045)
        m.tube_z(0.120, 0.335, 0.014, 0.014, 10, "steel", cy=-0.055)
        bev(m, 0, 0.020, 0.105, 0.055, 0.130, 0.060, 0.006, "chassis")
        bev(m, 0, 0.085, 0.230, 0.055, 0.035, 0.150, 0.004, "fde")
        bev(m, 0, -0.005, 0.355, 0.060, 0.160, 0.030, 0.004, "fde_dark")
        m.box(0.030, 0.020, 0.300, 0.010, 0.040, 0.030, "alu_dark")

        # Bipod legs + feet and rear monopod.
        for sx in (-1, 1):
            m.box(sx * 0.045, -0.095, -0.600, 0.014, 0.150, 0.014, "alu_dark",
                  yaw=sx * 0.22)
            m.box(sx * 0.080, -0.172, -0.600, 0.050, 0.018, 0.032, "poly")
        m.box(0, -0.100, 0.300, 0.014, 0.090, 0.014, "alu_dark")
        m.box(0, -0.148, 0.300, 0.040, 0.016, 0.030, "poly")

        # Sling QD points.
        m.tube_z(-0.450, -0.435, 0.012, 0.012, 8, "steel", cx=-0.038, cy=-0.020)
        m.tube_z(0.310, 0.325, 0.012, 0.012, 8, "steel", cx=-0.032, cy=0.000)
        return m

    def smg():
        m = Mesh()
        m.mat("alu", (0.200, 0.210, 0.225))
        m.mat("alu_dark", (0.110, 0.118, 0.130))
        m.mat("poly", (0.052, 0.055, 0.062))
        m.mat("poly_grey", (0.105, 0.110, 0.120))
        m.mat("steel", (0.160, 0.170, 0.188))
        m.mat("bore", (0.010, 0.010, 0.012))
        m.mat("lens", (0.030, 0.085, 0.105))
        m.mat("dot", (1.35, 0.09, 0.05))
        m.mat("mark", (0.620, 0.630, 0.655))

        # Compact receiver + lower.
        bev(m, 0, 0.020, -0.120, 0.062, 0.070, 0.320, 0.008, "alu")
        bev(m, 0, -0.040, -0.050, 0.056, 0.075, 0.200, 0.007, "alu")
        bev(m, 0, -0.070, -0.090, 0.056, 0.060, 0.085, 0.005, "alu_dark")
        rail(m, 0.061, -0.300, 0.060, "alu_dark", step=0.026)

        # Handguard, short barrel, suppressor.
        m.tube_z(-0.280, -0.420, 0.040, 0.038, 12, "poly_grey", cy=0.015)
        for z in (-0.310, -0.350, -0.390):
            m.box(0.039, 0.010, z, 0.006, 0.018, 0.026, "poly")
            m.box(-0.039, 0.010, z, 0.006, 0.018, 0.026, "poly")
        m.tube_z(-0.420, -0.480, 0.012, 0.012, 10, "steel", cy=0.015)
        m.tube_z(-0.480, -0.565, 0.028, 0.028, 12, "alu_dark", cy=0.015)
        m.tube_z(-0.560, -0.568, 0.028, 0.028, 12, "bore", cy=0.015)

        # Micro red dot.
        bev(m, 0, 0.070, -0.100, 0.036, 0.020, 0.060, 0.003, "alu_dark")
        bev(m, 0, 0.096, -0.100, 0.032, 0.040, 0.050, 0.004, "poly")
        m.tube_z(-0.128, -0.124, 0.017, 0.017, 10, "lens", cy=0.096)
        m.tube_z(-0.076, -0.072, 0.017, 0.017, 10, "lens", cy=0.096)
        m.sphere(0, 0.096, -0.073, 0.0035, 6, 4, "dot")

        # Charging handle, ejection port, controls.
        bev(m, -0.036, 0.030, 0.020, 0.020, 0.018, 0.060, 0.003, "alu_dark")
        m.box(0.033, 0.015, -0.070, 0.006, 0.036, 0.080, "bore")
        m.box(0.032, -0.035, 0.020, 0.008, 0.020, 0.022, "alu_dark")
        m.box(-0.030, -0.035, 0.020, 0.008, 0.020, 0.022, "alu_dark")

        # Trigger group.
        m.box(0, -0.082, 0.080, 0.046, 0.010, 0.100, "alu_dark")
        m.box(0, -0.055, 0.128, 0.046, 0.055, 0.010, "alu_dark")
        m.box(0, -0.055, 0.082, 0.008, 0.035, 0.012, "steel")

        # Raked grip.
        bev(m, 0, -0.068, 0.006, 0.055, 0.034, 0.075, 0.004, "poly")
        bev(m, 0, -0.108, 0.022, 0.050, 0.075, 0.075, 0.005, "poly")
        bev(m, 0, -0.156, 0.038, 0.046, 0.070, 0.068, 0.005, "poly")
        bev(m, 0, -0.200, 0.052, 0.048, 0.030, 0.060, 0.004, "poly")

        # Long curved magazine.
        bev(m, 0, -0.112, -0.064, 0.048, 0.070, 0.078, 0.005, "poly")
        bev(m, 0, -0.174, -0.074, 0.048, 0.066, 0.076, 0.005, "poly")
        bev(m, 0, -0.234, -0.088, 0.048, 0.062, 0.074, 0.005, "poly")
        bev(m, 0, -0.286, -0.106, 0.050, 0.050, 0.072, 0.004, "poly")
        bev(m, 0, -0.316, -0.116, 0.056, 0.020, 0.080, 0.003, "poly_grey")

        # Vertical foregrip.
        bev(m, 0, -0.090, -0.340, 0.040, 0.100, 0.050, 0.005, "poly")
        for i in range(3):
            m.box(0, -0.090, -0.355 + i * 0.014, 0.043, 0.070, 0.005,
                  "poly_grey")

        # Folding stock rods + butt plate.
        bev(m, 0, 0.020, 0.105, 0.052, 0.060, 0.040, 0.004, "alu_dark")
        m.tube_z(0.120, 0.300, 0.008, 0.008, 8, "steel", cx=0.022, cy=0.020)
        m.tube_z(0.120, 0.300, 0.008, 0.008, 8, "steel", cx=-0.022, cy=0.020)
        bev(m, 0, 0.020, 0.310, 0.065, 0.080, 0.022, 0.004, "poly_grey")
        bev(m, 0, 0.055, 0.300, 0.055, 0.025, 0.050, 0.003, "poly")
        m.tube_z(0.300, 0.315, 0.012, 0.012, 8, "steel", cx=-0.032, cy=0.000)
        return m

    for name, fn in (("weapon_rifle_fp", rifle), ("weapon_shotgun_fp", shotgun),
                     ("weapon_sniper_fp", sniper), ("weapon_smg_fp", smg)):
        fn().write(name)


def gen_fp_arms():
    """First-person arms/hands for the two-bone IK rig. Each arm segment is a
    unit-length tube along -Z (scale.z drives its length); hands are authored
    around the weapon grip with -Z pointing down the barrel."""
    GLOVE = (0.360, 0.310, 0.230)
    PAD = (0.155, 0.148, 0.130)
    SLEEVE = (0.240, 0.250, 0.190)
    STRAP = (0.315, 0.330, 0.260)

    m = Mesh()
    m.mat("sleeve", SLEEVE); m.mat("pad", PAD); m.mat("strap", STRAP)
    m.tube_z(0.02, -1.0, 0.095, 0.064, 10, "sleeve")         # shoulder -> elbow
    m.box(0, 0.0, -0.09, 0.200, 0.200, 0.105, "pad")        # shoulder pad
    m.box(0, 0.0, -0.20, 0.175, 0.175, 0.045, "strap")      # sleeve strap
    m.box(0, 0.0, -0.97, 0.115, 0.115, 0.055, "pad")        # elbow pad
    m.write("arm_upper_fp")

    m = Mesh()
    m.mat("sleeve", SLEEVE); m.mat("pad", PAD); m.mat("strap", STRAP)
    m.mat("glove", GLOVE)
    m.tube_z(0.01, -1.0, 0.070, 0.048, 10, "sleeve")         # elbow -> wrist
    m.box(0, 0.0, -0.10, 0.135, 0.135, 0.060, "strap")
    m.box(0, 0.0, -0.93, 0.105, 0.105, 0.120, "glove")      # glove cuff
    m.box(0.0, 0.0, -1.00, 0.080, 0.080, 0.035, "pad")
    m.write("arm_fore_fp")

    # Right hand: closed fist around a pistol grip at the origin. The palm sits
    # on the right side of the grip, the fingers wrap around the front.
    m = Mesh()
    m.mat("glove", GLOVE); m.mat("pad", PAD)
    m.box(0.044, -0.005, 0.000, 0.076, 0.116, 0.132, "glove")    # palm/back
    for i in range(4):
        y = -0.042 + i * 0.028
        m.box(0.000, y, -0.052, 0.078, 0.024, 0.058, "glove")    # fingers
        m.box(-0.032, y, -0.052, 0.016, 0.020, 0.048, "pad")     # knuckles
    m.box(0.012, 0.060, -0.030, 0.050, 0.034, 0.072, "glove")    # thumb
    m.box(0.058, 0.000, 0.072, 0.086, 0.106, 0.032, "pad")       # wrist cuff
    m.write("hand_right_fp")

    # Left support hand: palm under the handguard, four short fingers wrapping
    # over the top, thumb bracing the near side.
    m = Mesh()
    m.mat("glove", GLOVE); m.mat("pad", PAD)
    m.box(0.000, -0.052, 0.000, 0.104, 0.058, 0.146, "glove")    # palm
    for i in range(4):
        x = -0.039 + i * 0.026
        m.box(x, 0.000, -0.010, 0.022, 0.062, 0.108, "glove")    # fingers
        m.box(x, 0.034, -0.020, 0.019, 0.020, 0.088, "pad")      # knuckles
    m.box(0.058, -0.014, 0.020, 0.030, 0.050, 0.078, "glove")    # thumb
    m.box(0.000, -0.022, 0.090, 0.100, 0.074, 0.032, "pad")      # wrist cuff
    m.write("hand_left_fp")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("generating models into", OUT)
    gen_drone()
    gen_turret()
    gen_sapper()
    gen_heavy()
    gen_boss()
    gen_barrier()
    gen_pickups()
    gen_weapons()
    gen_fp_arms()
