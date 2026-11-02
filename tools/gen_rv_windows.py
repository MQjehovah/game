#!/usr/bin/env python3
"""Generate the breakable POI window assets.

  rv_window_glass.gltf - a 1 x 1 m transparent pane (thin solid box)
  rv_window_shard.gltf - a small glass chip used for the break burst

Both keep a hidden material-0 primitive because the engine's importer gives the
first primitive its default material. Solid boxes are used instead of flat
quads so the mesh never ends up with a degenerate volume.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PREFABS = ROOT / "projects/rv/assets/prefabs"


def add_box(pos, nrm, ind, cx, cy, cz, sx, sy, sz):
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
    for n, quad in faces:
        base = len(pos)
        for q in quad:
            pos.append(q)
            nrm.append(n)
        ind.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def write_mesh(name, positions, normals, indices, alpha=0.35):
    # Hidden dummy box using material 0 (importer default slot).
    pos, nrm, ind = [], [], []
    add_box(pos, nrm, ind, 0.0, -0.6, 0.0, 0.02, 0.02, 0.02)
    offset = len(pos)
    pos = list(pos) + list(positions)
    nrm = list(nrm) + list(normals)
    ind = list(ind) + [i + offset for i in indices]

    buffer = b""
    views = []
    accessors = []
    prims = []
    chunks = [((pos[:24], nrm[:24], ind[:36]), 0),
              ((pos[24:], nrm[24:], [i - 24 for i in ind[36:]]), 1)]
    for (p, n, ix), mat in chunks:
        for raw in (
                b"".join(struct.pack("<3f", *v) for v in p),
                b"".join(struct.pack("<3f", *v) for v in n),
                b"".join(struct.pack("<I", i) for i in ix)):
            pad = (4 - len(raw) % 4) % 4
            blob = raw + b"\0" * pad
            views.append({"buffer": 0, "byteOffset": len(buffer),
                          "byteLength": len(blob)})
            buffer += blob
        vb = len(views) - 3
        mn = [min(v[i] for v in p) for i in range(3)]
        mx = [max(v[i] for v in p) for i in range(3)]
        accessors.extend([
            {"bufferView": vb, "componentType": 5126, "count": len(p),
             "type": "VEC3", "min": mn, "max": mx},
            {"bufferView": vb + 1, "componentType": 5126, "count": len(n),
             "type": "VEC3"},
            {"bufferView": vb + 2, "componentType": 5125, "count": len(ix),
             "type": "SCALAR"},
        ])
        ai = len(accessors) - 3
        prims.append({"attributes": {"POSITION": ai, "NORMAL": ai + 1},
                      "indices": ai + 2, "material": mat})

    materials = [
        # The base primitive drives the entity's pass classification, so it must
        # also be BLEND (alpha 0) or the transparent pane would be drawn inside
        # the opaque pass and then overwritten by the shop shell.
        {"name": "glass_default",
         "alphaMode": "BLEND",
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 0.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "glass_clear",
         "alphaMode": "BLEND",
         "doubleSided": True,
         "pbrMetallicRoughness": {"baseColorFactor": [0.40, 0.68, 0.92, alpha],
                                  "metallicFactor": 0.1, "roughnessFactor": 0.12}},
    ]
    gltf = {
        "asset": {"version": "2.0", "generator": "neon rv window generator"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": prims}],
        "materials": materials,
        "buffers": [{"uri": name + ".bin", "byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    (OUT / (name + ".bin")).write_bytes(buffer)
    (OUT / (name + ".gltf")).write_text(json.dumps(gltf, separators=(",", ":")),
                                        encoding="utf-8")
    (PREFABS / (name + ".json")).write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {"meshKey": "gltf:assets/models/rv/" + name + ".gltf",
                     "castShadow": False, "receiveShadow": False},
        },
    }, indent=2) + "\n", encoding="utf-8")
    print(name, "bytes", len(buffer), "prims", len(prims))


def main():
    # Unit pane, 1 x 1 m, 3 cm thick.
    pos, nrm, ind = [], [], []
    add_box(pos, nrm, ind, 0.0, 0.0, 0.0, 1.0, 1.0, 0.03)
    write_mesh("rv_window_glass", pos, nrm, ind, alpha=0.35)

    # Small glass chip.
    pos, nrm, ind = [], [], []
    add_box(pos, nrm, ind, 0.0, 0.0, 0.0, 0.45, 0.45, 0.02)
    write_mesh("rv_window_shard", pos, nrm, ind, alpha=0.92)


if __name__ == "__main__":
    main()
