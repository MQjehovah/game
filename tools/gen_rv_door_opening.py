#!/usr/bin/env python3
"""Cut a real passenger-door opening into rv_body.gltf.

The body's right wall is two huge triangles. This script removes them and
replaces the wall with top/rear/front strips around a rectangular door hole
(z=-3.54..-2.76, y=0.25..1.86), then removes the right-side trim band inside
the same opening. The separate rv_door.gltf mesh covers the hole when closed.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RV = ROOT / "projects/rv/assets/models/rv"
GLTF = RV / "rv_body.gltf"
BIN = RV / "rv_body.bin"

X = 1.2
Z_MIN, Z_MAX = -4.0, 3.8
Y_MIN, Y_MAX = 0.25, 2.15
DOOR_Z0, DOOR_Z1 = -3.54, -2.76
DOOR_Y0, DOOR_Y1 = 0.25, 1.86


def read_acc(j, blob, i):
    a = j["accessors"][i]
    v = j["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[a["type"]]
    f = {5126: "f", 5125: "I", 5123: "H"}[a["componentType"]]
    s = struct.calcsize(f)
    st = v.get("byteStride", n * s)
    base = v.get("byteOffset", 0) + a.get("byteOffset", 0)
    return [struct.unpack_from("<" + f * n, blob, base + k * st) for k in range(a["count"])]


def append_acc(j, blob, values, typ, component=5126):
    n = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[typ]
    f = {5126: "f", 5125: "I", 5123: "H"}[component]
    raw = b"".join(struct.pack("<" + f * n, *v) for v in values)
    off = len(blob)
    blob += raw
    pad = (4 - len(blob) % 4) % 4
    blob += b"\0" * pad
    vi = len(j["bufferViews"])
    j["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    a = {"bufferView": vi, "componentType": component, "count": len(values), "type": typ}
    if component == 5126:
        a["min"] = [min(v[k] for v in values) for k in range(n)]
        a["max"] = [max(v[k] for v in values) for k in range(n)]
    j["accessors"].append(a)
    return len(j["accessors"]) - 1, blob


def wall_quad(z_hi, z_lo, y_lo, y_hi):
    verts = [
        (X, y_lo, z_hi), (X, y_lo, z_lo), (X, y_hi, z_lo),
        (X, y_hi, z_hi),
    ]
    return verts, [(0, 1, 2), (0, 2, 3)]


def main():
    j = json.loads(GLTF.read_text(encoding="utf-8"))
    blob = BIN.read_bytes()
    prims = j["meshes"][0]["primitives"]

    # --- Body wall with a door hole -------------------------------------
    body = prims[0]
    pos = read_acc(j, blob, body["attributes"]["POSITION"])
    nrm = read_acc(j, blob, body["attributes"]["NORMAL"])
    idx = [v[0] for v in read_acc(j, blob, body["indices"])]
    assert len(idx) == 108 and idx[:6] == [0, 1, 2, 3, 4, 5]
    # Drop the two full-height right-side triangles (indices 0..5).
    new_idx = idx[6:]
    new_pos = list(pos)
    new_nrm = list(nrm)
    base = len(new_pos)
    for quad in (
        wall_quad(Z_MAX, Z_MIN, DOOR_Y1, Y_MAX),   # top strip
        wall_quad(DOOR_Z1, Z_MIN, DOOR_Y0, DOOR_Y1),  # rear strip
        wall_quad(Z_MAX, DOOR_Z0, DOOR_Y0, DOOR_Y1),  # front strip
    ):
        verts, tris = quad
        for v in verts:
            new_pos.append(v)
            new_nrm.append((1.0, 0.0, 0.0))
        for t in tris:
            new_idx.extend((base + t[0], base + t[1], base + t[2]))
        base += 4
    pos_acc, blob = append_acc(j, blob, new_pos, "VEC3")
    nrm_acc, blob = append_acc(j, blob, new_nrm, "VEC3")
    idx_acc, blob = append_acc(j, blob, [(v,) for v in new_idx], "SCALAR", 5125)
    body["attributes"]["POSITION"] = pos_acc
    body["attributes"]["NORMAL"] = nrm_acc
    body["indices"] = idx_acc

    # --- Remove the right-side trim band across the opening -------------
    trim = prims[2]
    tpos = read_acc(j, blob, trim["attributes"]["POSITION"])
    tidx = [v[0] for v in read_acc(j, blob, trim["indices"])]
    kept = []
    removed = 0
    for k in range(0, len(tidx), 3):
        vs = [tpos[tidx[k + q]] for q in range(3)]
        cx = sum(v[0] for v in vs) / 3.0
        cz = sum(v[2] for v in vs) / 3.0
        if cx > 1.1 and DOOR_Z0 - 0.08 <= cz <= DOOR_Z1 + 0.08:
            removed += 1
            continue
        kept.extend(tidx[k:k + 3])
    tidx_acc, blob = append_acc(j, blob, [(v,) for v in kept], "SCALAR", 5125)
    trim["indices"] = tidx_acc

    j["buffers"][0]["byteLength"] = len(blob)
    GLTF.write_text(json.dumps(j, separators=(",", ":")), encoding="utf-8")
    BIN.write_bytes(blob)
    print(f"body tris {len(idx)//3}->{len(new_idx)//3}, trim removed {removed}, bin {len(blob)}")


if __name__ == "__main__":
    main()
