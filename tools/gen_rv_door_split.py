#!/usr/bin/env python3
"""Split the RV side door out of rv_body.gltf into a hinged rv_door.gltf.

The source body is one mesh with a final door primitive (material 'door').
We remove that primitive and write a small standalone door mesh whose local
origin is the hinge line (rear edge, centre of the panel thickness). The game
can then rotate the door entity around Y for a real hinge animation instead of
using a cube overlay.
"""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RV = ROOT / "projects/rv/assets/models/rv"
BODY_GLTF = RV / "rv_body.gltf"
BODY_BIN = RV / "rv_body.bin"
DOOR_GLTF = RV / "rv_door.gltf"
DOOR_BIN = RV / "rv_door.bin"
PREFAB = ROOT / "projects/rv/assets/prefabs/rv_door.json"

HINGE_X = 1.205
HINGE_Z = -3.54


def read_accessor(gltf, blob, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    comps = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[acc["componentType"]]
    size = struct.calcsize(fmt)
    stride = view.get("byteStride", comps * size)
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    out = []
    for i in range(acc["count"]):
        out.append(struct.unpack_from("<" + fmt * comps, blob, base + i * stride))
    return out


def main():
    body = json.loads(BODY_GLTF.read_text(encoding="utf-8"))
    blob = BODY_BIN.read_bytes()
    prims = body["meshes"][0]["primitives"]
    door_prim = prims[-1]
    door_mat = body["materials"][door_prim["material"]]
    pos = read_accessor(body, blob, door_prim["attributes"]["POSITION"])
    nrm = read_accessor(body, blob, door_prim["attributes"]["NORMAL"])
    idx = read_accessor(body, blob, door_prim["indices"])

    local_pos = [(x - HINGE_X, y, z - HINGE_Z) for (x, y, z) in pos]
    minv = [min(p[i] for p in local_pos) for i in range(3)]
    maxv = [max(p[i] for p in local_pos) for i in range(3)]

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in local_pos)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in nrm)
    idx_bytes = b"".join(struct.pack("<H", i[0]) for i in idx)
    # Keep each view 4-byte aligned.
    while len(pos_bytes) % 4:
        pos_bytes += b"\0"
    while len(nrm_bytes) % 4:
        nrm_bytes += b"\0"
    while len(idx_bytes) % 4:
        idx_bytes += b"\0"
    door_blob = pos_bytes + nrm_bytes + idx_bytes
    DOOR_BIN.write_bytes(door_blob)

    door = {
        "asset": {"version": "2.0", "generator": "neon rv door split"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "RV_Door", "mesh": 0}],
        "meshes": [{
            "name": "RV_Door",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
            }],
        }],
        "materials": [door_mat],
        "buffers": [{"uri": "rv_door.bin", "byteLength": len(door_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes),
             "byteLength": len(idx_bytes), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(local_pos),
             "type": "VEC3", "min": minv, "max": maxv},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }
    DOOR_GLTF.write_text(json.dumps(door, separators=(",", ":")), encoding="utf-8")

    body["meshes"][0]["primitives"] = prims[:-1]
    BODY_GLTF.write_text(json.dumps(body, separators=(",", ":")), encoding="utf-8")

    PREFAB.write_text(json.dumps({
        "components": {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "mesh": {
                "meshKey": "gltf:assets/models/rv/rv_door.gltf",
                "castShadow": True,
                "receiveShadow": True,
            },
        },
    }, indent=2) + "\n", encoding="utf-8")

    print(f"door verts={len(local_pos)} tris={len(idx)//3} bytes={len(door_blob)}")
    print(f"hinge=({HINGE_X},{HINGE_Z}) min={minv} max={maxv}")


if __name__ == "__main__":
    main()
