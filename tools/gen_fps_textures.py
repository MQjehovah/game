# Generates NeonOps floor/wall textures as PNG (pure stdlib: zlib + struct).
# Run from the repo root: python tools/gen_fps_textures.py
import math
import struct
import zlib

OUT = "projects/fps/assets/textures"


def write_png(path, w, h, pixels):
    """pixels: list of rows, each row list of (r,g,b) tuples."""
    raw = b""
    for row in pixels:
        raw += b"\x00" + b"".join(struct.pack("BBB", *px) for px in row)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    print("  %s %dx%d" % (path, w, h))


def lerp(a, b, t):
    return a + (b - a) * t


def gen_floor():
    """Tech floor: dark slate panels, subtle value noise, cyan service lines,
    darker seams, occasional accent tiles."""
    W = H = 512
    tile = 64
    rnd_state = 12345

    def rnd():
        nonlocal rnd_state
        rnd_state = (rnd_state * 1103515245 + 12345) & 0x7FFFFFFF
        return rnd_state / 0x7FFFFFFF

    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            gx, gy = x // tile, y // tile
            lx, ly = x % tile, y % tile
            # per-tile base value
            base = 58 + (gx * 7 + gy * 13) % 5 * 4
            # fine noise
            n = (math.sin(x * 0.7 + gy * 31.7) * math.cos(y * 0.9 + gx * 17.3))
            v = base + n * 2.5 + (rnd() - 0.5) * 4
            # accent tiles (deterministic pattern)
            if (gx * 31 + gy * 17) % 11 == 0 and 8 <= lx < tile - 8 and 8 <= ly < tile - 8:
                v += 10
            r, g, b = v * 0.85, v * 0.94, v * 1.08
            # cyan service line along tile center every other row
            if gy % 2 == 0 and 30 <= ly < 33 and lx > 4 and lx < tile - 4:
                r, g, b = 0.20 * 255, 0.60 * 255, 0.68 * 255
            # seams
            if lx < 2 or ly < 2 or lx > tile - 3 or ly > tile - 3:
                r, g, b = 30, 35, 42
            # rivets at tile corners
            dx, dy = min(lx, tile - 1 - lx), min(ly, tile - 1 - ly)
            if dx in (6, 7) and dy in (6, 7):
                r, g, b = min(255, r + 45), min(255, g + 50), min(255, b + 55)
            row.append((int(max(0, min(255, r))), int(max(0, min(255, g))),
                        int(max(0, min(255, b)))))
        rows.append(row)
    write_png(OUT + "/ground_tech.png", W, H, rows)


def gen_wall():
    """Arena wall: vertical panels, top vent slots, bottom hazard band."""
    W, H = 512, 256
    panel = 64
    rnd_state = 999

    def rnd():
        nonlocal rnd_state
        rnd_state = (rnd_state * 1103515245 + 12345) & 0x7FFFFFFF
        return rnd_state / 0x7FFFFFFF

    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            gx = x // panel
            lx = x % panel
            base = 40 + (gx * 11) % 4 * 4
            v = base + (rnd() - 0.5) * 7
            r, g, b = v * 0.85, v * 0.95, v * 1.12
            # vertical seams
            if lx < 2 or lx > panel - 3:
                r, g, b = 18, 21, 26
            # vent slots in upper third
            if y < 60 and (y % 18) < 6 and 10 < lx < panel - 10:
                r, g, b = 14, 16, 20
            # hazard band bottom 24px
            if y > H - 26:
                stripe = ((x + y) // 16) % 2 == 0
                r, g, b = (66, 52, 12) if stripe else (30, 30, 32)
            row.append((int(r), int(g), int(b)))
        rows.append(row)
    write_png(OUT + "/wall_tech.png", W, H, rows)


if __name__ == "__main__":
    import os
    os.makedirs(OUT, exist_ok=True)
    print("generating textures into", OUT)
    gen_floor()
    gen_wall()
