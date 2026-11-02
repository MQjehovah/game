#!/usr/bin/env python3
"""Generate the RV passenger animation clips inside Soldier.glb.

Clips:
  Sit    - static seated pose (Idle breathing + hip/thigh/knee/foot offsets)
  Board  - one-shot Idle -> Sit transition (0.6s)
  Stand  - one-shot Sit -> Idle transition (0.6s)

The source character ships Idle/Run/TPose/Walk only. We always rebuild from the
pristine pre-Sit GLB so this script is idempotent (append-only runs would keep
growing the binary). The base revision is pinned; edit BASE_REV if the source
model is replaced upstream.
"""
import json
import math
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REL = "projects/rv/assets/models/player/Soldier.glb"
GLB = ROOT / REL
BASE_REV = "6390ede"  # last commit with the untouched Idle/Run/TPose/Walk model
TRANSITION_SECONDS = 0.6
TRANSITION_SAMPLES = 13


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


def q_norm(q):
    n = math.sqrt(sum(c * c for c in q))
    if n <= 1e-8:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(c / n for c in q)


def q_slerp(a, b, t):
    a = q_norm(a)
    b = q_norm(b)
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-c for c in b)
        dot = -dot
    if dot > 0.9995:
        return q_norm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    theta0 = math.acos(max(-1.0, min(1.0, dot)))
    theta = theta0 * t
    sin0 = math.sin(theta0)
    s0 = math.sin(theta0 - theta) / sin0
    s1 = math.sin(theta) / sin0
    return q_norm(tuple(x * s0 + y * s1 for x, y in zip(a, b)))


def read_glb_bytes(data):
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2, "not a GLB v2 file"
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


def read_accessor(js, binary, index):
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
    comps = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[typ]
    fmt = {5126: "f", 5123: "H"}[component_type]
    raw = b"".join(struct.pack("<" + fmt * comps, *v) for v in values)
    off = len(binary)
    binary += raw
    binary += b"\0" * ((4 - len(binary) % 4) % 4)
    view_index = len(js["bufferViews"])
    js["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    acc = {
        "bufferView": view_index,
        "componentType": component_type,
        "count": len(values),
        "type": typ,
    }
    if component_type == 5126:
        acc["min"] = [min(v[i] for v in values) for i in range(comps)]
        acc["max"] = [max(v[i] for v in values) for i in range(comps)]
    js["accessors"].append(acc)
    return len(js["accessors"]) - 1, binary


# Mixamo legs point +Y down the bone: a local-X rotation swings the thigh/shin.
ROT_OFFSETS = {
    "mixamorig:LeftUpLeg": q_axis((1, 0, 0), -1.15),
    "mixamorig:RightUpLeg": q_axis((1, 0, 0), -1.15),
    "mixamorig:LeftLeg": q_axis((1, 0, 0), 1.35),
    "mixamorig:RightLeg": q_axis((1, 0, 0), 1.35),
    "mixamorig:LeftFoot": q_axis((1, 0, 0), -0.55),
    "mixamorig:RightFoot": q_axis((1, 0, 0), -0.55),
}
HIP_DROP = 50.0


def build_clip(js, binary, idle, name, blend):
    """blend(t) -> 0..1 seated amount for a one-shot transition, or None for a
    static seated clip (always 1)."""
    nodes = js["nodes"]
    channels = []
    samplers = []
    for ch in idle["channels"]:
        node_name = nodes[ch["target"]["node"]].get("name", "")
        path = ch["target"]["path"]
        base = idle["samplers"][ch["sampler"]]
        out_index = base["output"]
        input_index = base["input"]
        typ = js["accessors"][base["output"]]["type"]
        if blend is None:
            if path == "rotation" and node_name in ROT_OFFSETS:
                vals = read_accessor(js, binary, base["output"])
                new_vals = [q_mul(v, ROT_OFFSETS[node_name]) for v in vals]
                out_index, binary = append_accessor(js, binary, new_vals, "VEC4")
            elif path == "translation" and node_name == "mixamorig:Hips":
                vals = read_accessor(js, binary, base["output"])
                new_vals = [(x, y, z - HIP_DROP) for (x, y, z) in vals]
                out_index, binary = append_accessor(js, binary, new_vals, "VEC3")
        else:
            # One-shot transition: resample EVERY channel onto a 0.6s timeline
            # so the clip's duration (and AnimFinished) matches the blend, not
            # the 2s Idle loop.
            vals = read_accessor(js, binary, base["output"])
            n = len(vals)
            times = []
            new_vals = []
            for k in range(TRANSITION_SAMPLES):
                t = k / (TRANSITION_SAMPLES - 1)
                src = vals[min(n - 1, int(round(t * (n - 1))))]
                amt = blend(t)
                if path == "rotation" and node_name in ROT_OFFSETS:
                    part = q_slerp((0.0, 0.0, 0.0, 1.0),
                                   ROT_OFFSETS[node_name], amt)
                    src = q_mul(src, part)
                elif path == "translation" and node_name == "mixamorig:Hips":
                    src = (src[0], src[1], src[2] - HIP_DROP * amt)
                times.append((t * TRANSITION_SECONDS,))
                new_vals.append(src)
            input_index, binary = append_accessor(js, binary, times, "SCALAR")
            out_index, binary = append_accessor(js, binary, new_vals, typ)
        samplers.append({
            "input": input_index,
            "output": out_index,
            "interpolation": "LINEAR",
        })
        channels.append({"sampler": len(samplers) - 1, "target": ch["target"]})
    js["animations"].append({"name": name, "channels": channels, "samplers": samplers})
    return binary


def load_base():
    tmp = ROOT / "tools" / "_soldier_base.glb"
    try:
        # Pipe git's blob straight into a binary file: capturing stdout through
        # a text pipe can try to decode the GLB and fail on non-UTF-8 payloads.
        with open(tmp, "wb") as fh:
            subprocess.run(["git", "show", f"{BASE_REV}:{REL}"],
                           cwd=ROOT, stdout=fh, stderr=subprocess.DEVNULL,
                           check=True)
        return tmp.read_bytes()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


def load_base_or_strip():
    base = load_base()
    if base is not None:
        return base
    if True:
        # Fallback: strip generated clips from the current file (binary may keep
        # unused padding, but the animation list stays correct).
        data = GLB.read_bytes()
        js, binary = read_glb_bytes(data)
        js["animations"] = [
            a for a in js.get("animations", [])
            if a.get("name") not in ("Sit", "Board", "Stand")
        ]
        write_glb(GLB, js, binary)
        return GLB.read_bytes()


def main():
    data = load_base_or_strip()
    js, binary = read_glb_bytes(data)
    js["animations"] = [
        a for a in js.get("animations", [])
        if a.get("name") not in ("Sit", "Board", "Stand")
    ]
    idle = next(a for a in js["animations"] if a.get("name") == "Idle")

    binary = build_clip(js, binary, idle, "Sit", None)
    binary = build_clip(js, binary, idle, "Board", lambda t: t * t * (3.0 - 2.0 * t))
    binary = build_clip(js, binary, idle, "Stand", lambda t: 1.0 - (t * t * (3.0 - 2.0 * t)))

    js["buffers"][0]["byteLength"] = len(binary)
    write_glb(GLB, js, binary)
    print("animations:", [a.get("name") for a in js["animations"]])
    print("binary bytes:", len(binary))


if __name__ == "__main__":
    main()
