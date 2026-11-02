#!/usr/bin/env python3
"""Add a simple skeletal 'Sit' clip to Soldier.glb.

The source character has Idle/Run/TPose/Walk but no seated clip. This script
duplicates the Idle animation and lowers the hips plus bends the thighs/knees
with local-axis quaternion offsets, producing a static seated pose with the
Idle torso/arm sway. It is intentionally a lightweight prototype clip; the
game plays it for passengers instead of scaling the standing avatar.
"""
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLB = ROOT / "projects/rv/assets/models/player/Soldier.glb"


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def q_axis(axis, angle):
    s = math.sin(angle * 0.5)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle * 0.5))


def read_glb(path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2
    off = 12
    js = None
    binary = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8:off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            binary = chunk
        off += 8 + clen
    return js, binary


def write_glb(path, js, binary):
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    bb = binary + b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(jb) + 8 + len(bb)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(jb), 0x4E4F534A) + jb
    out += struct.pack("<II", len(bb), 0x004E4942) + bb
    path.write_bytes(out)


def accessor_bytes(js, binary, index):
    acc = js["accessors"][index]
    view = js["bufferViews"][acc["bufferView"]]
    comps = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
    fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[acc["componentType"]]
    size = struct.calcsize(fmt)
    stride = view.get("byteStride", comps * size)
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    return [
        struct.unpack_from("<" + fmt * comps, binary, base + i * stride)
        for i in range(acc["count"])
    ]


def append_accessor(js, binary, values, typ, component_type=5126):
    # values: list of tuples; append as a new bufferView + accessor.
    comps = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[typ]
    fmt = {5126: "f", 5123: "H"}[component_type]
    raw = b"".join(struct.pack("<" + fmt * comps, *v) for v in values)
    off = len(binary)
    binary += raw
    pad = (4 - len(binary) % 4) % 4
    binary += b"\0" * pad
    view_index = len(js["bufferViews"])
    js["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    acc = {
        "bufferView": view_index,
        "componentType": component_type,
        "count": len(values),
        "type": typ,
    }
    if component_type == 5126:
        mins = [min(v[i] for v in values) for i in range(comps)]
        maxs = [max(v[i] for v in values) for i in range(comps)]
        acc["min"] = mins
        acc["max"] = maxs
    js["accessors"].append(acc)
    return len(js["accessors"]) - 1, binary


def main():
    js, binary = read_glb(GLB)
    idle = next(a for a in js["animations"] if a.get("name") == "Idle")
    nodes = js["nodes"]
    # Per-node local rotation offsets (post-multiply the Idle quaternion).
    # Mixamo legs use local +Y down the bone; rotating around local X swings the
    # thigh/shin forward/back. Values tuned by rendering the seated avatar.
    rot_offsets = {
        "mixamorig:LeftUpLeg": q_axis((1, 0, 0), -1.15),
        "mixamorig:RightUpLeg": q_axis((1, 0, 0), -1.15),
        "mixamorig:LeftLeg": q_axis((1, 0, 0), 1.35),
        "mixamorig:RightLeg": q_axis((1, 0, 0), 1.35),
        "mixamorig:LeftFoot": q_axis((1, 0, 0), -0.55),
        "mixamorig:RightFoot": q_axis((1, 0, 0), -0.55),
    }
    channels = []
    samplers = []
    for ch in idle["channels"]:
        node_name = nodes[ch["target"]["node"]].get("name", "")
        path = ch["target"]["path"]
        base = idle["samplers"][ch["sampler"]]
        out_index = base["output"]
        if path == "rotation" and node_name in rot_offsets:
            vals = accessor_bytes(js, binary, base["output"])
            new_vals = [q_mul(v, rot_offsets[node_name]) for v in vals]
            out_index, binary = append_accessor(js, binary, new_vals, "VEC4")
        elif path == "translation" and node_name == "mixamorig:Hips":
            vals = accessor_bytes(js, binary, base["output"])
            new_vals = [(x, y, z - 50.0) for (x, y, z) in vals]
            out_index, binary = append_accessor(js, binary, new_vals, "VEC3")
        samplers.append({
            "input": base["input"],
            "output": out_index,
            "interpolation": base.get("interpolation", "LINEAR"),
        })
        channels.append({"sampler": len(samplers) - 1, "target": ch["target"]})
    js["animations"].append({"name": "Sit", "channels": channels, "samplers": samplers})
    js["buffers"][0]["byteLength"] = len(binary)
    write_glb(GLB, js, binary)
    print("animations:", [a.get("name") for a in js["animations"]])
    print("sit channels:", len(channels), "bytes:", len(binary))


if __name__ == "__main__":
    main()
