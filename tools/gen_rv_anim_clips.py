#!/usr/bin/env python3
"""Author multi-keyframe Sit/Board/Stand/Door clips for the RV passenger.

Unlike the previous generator (which duplicated Idle and added constant bone
offsets), this script defines explicit key poses per clip:

  Sit       - seated loop with a subtle breathing key at 1s
  Board     - stance -> reach -> step -> lower -> seated (0.9s)
  Stand     - seated -> lean -> rise -> step out -> stance (0.9s)
  DoorOpen  - stance -> reach handle -> grip -> pull open -> stance (0.9s)
  DoorClose - stance -> reach handle -> grip -> push shut -> stance (0.9s)

Rotations are authored as local corrections over the Mixamo bind pose; the
relaxed arm stance is baked in as constants so no Idle data is reused. The
source is rebuilt from the pinned pre-animation GLB, so re-running is
idempotent.
"""
import json
import math
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REL = "projects/rv/assets/models/player/Soldier.glb"
GLB = ROOT / REL
BASE_REV = "6390ede"  # pristine Idle/Run/TPose/Walk model


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


def read_glb_bytes(data):
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


def read_glb(path):
    return read_glb_bytes(path.read_bytes())


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


def append_accessor(js, binary, values, typ, component_type=5126):
    comps = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[typ]
    fmt = {5126: "f", 5123: "H"}[component_type]
    raw = b"".join(struct.pack("<" + fmt * comps, *v) for v in values)
    off = len(binary)
    binary += raw
    binary += b"\0" * ((4 - len(binary) % 4) % 4)
    vi = len(js["bufferViews"])
    js["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
    acc = {"bufferView": vi, "componentType": component_type,
           "count": len(values), "type": typ}
    if component_type == 5126:
        acc["min"] = [min(v[i] for v in values) for i in range(comps)]
        acc["max"] = [max(v[i] for v in values) for i in range(comps)]
    js["accessors"].append(acc)
    return len(js["accessors"]) - 1, binary


def load_base():
    tmp = ROOT / "tools" / "_soldier_base.glb"
    try:
        with open(tmp, "wb") as fh:
            subprocess.run(["git", "show", f"{BASE_REV}:{REL}"],
                           cwd=ROOT, stdout=fh, stderr=subprocess.DEVNULL,
                           check=True)
        return tmp.read_bytes()
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


# Baked relaxed arm/shoulder stance (local rotations from the character's
# neutral pose, hard-coded so the clips do not depend on the Idle animation).
BASE_ARM_ROT = {
    "mixamorig:LeftShoulder": (0.7540, 0.6567, 0.0126, -0.0052),
    "mixamorig:LeftArm": (-0.0475, -0.1166, 0.5226, 0.8432),
    "mixamorig:LeftForeArm": (-0.4050, 0.0579, -0.0125, 0.9124),
    "mixamorig:LeftHand": (-0.0150, 0.1401, -0.1628, 0.9766),
    "mixamorig:RightShoulder": (0.0711, 0.0437, 0.7629, 0.6411),
    "mixamorig:RightArm": (-0.1868, 0.0970, 0.4908, 0.8455),
    "mixamorig:RightForeArm": (0.3595, 0.0291, 0.0447, 0.9316),
    "mixamorig:RightHand": (0.1012, -0.0646, 0.1067, 0.9870),
}

UP_L = "mixamorig:LeftUpLeg"
LO_L = "mixamorig:LeftLeg"
FOOT_L = "mixamorig:LeftFoot"
UP_R = "mixamorig:RightUpLeg"
LO_R = "mixamorig:RightLeg"
FOOT_R = "mixamorig:RightFoot"
SPINE = "mixamorig:Spine"
SPINE1 = "mixamorig:Spine1"
NECK = "mixamorig:Neck"
HEAD = "mixamorig:Head"


def leg_corrs(thigh, knee, foot):
    return {UP_L: q_axis((1, 0, 0), thigh), UP_R: q_axis((1, 0, 0), thigh),
            LO_L: q_axis((1, 0, 0), knee), LO_R: q_axis((1, 0, 0), knee),
            FOOT_L: q_axis((1, 0, 0), foot), FOOT_R: q_axis((1, 0, 0), foot)}


STANCE = dict(leg_corrs(-0.06, 0.12, -0.06))
STANCE.update({
    SPINE: q_axis((1, 0, 0), 0.04),
    SPINE1: q_axis((1, 0, 0), 0.05),
    NECK: q_axis((1, 0, 0), -0.02),
    HEAD: q_axis((1, 0, 0), -0.02),
})

SIT = dict(leg_corrs(-1.15, 1.35, -0.55))
SIT.update({
    SPINE: q_axis((1, 0, 0), 0.14),
    SPINE1: q_axis((1, 0, 0), 0.16),
    NECK: q_axis((1, 0, 0), 0.05),
    HEAD: q_axis((1, 0, 0), 0.06),
    "mixamorig:LeftForeArm": q_axis((1, 0, 0), -0.55),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), 0.55),
    "mixamorig:LeftArm": q_axis((1, 0, 0), -0.12),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.12),
})
SIT_BREATH = dict(SIT)
SIT_BREATH.update({
    SPINE1: q_mul(SIT[SPINE1], q_axis((1, 0, 0), 0.02)),
    HEAD: q_mul(SIT[HEAD], q_axis((1, 0, 0), -0.02)),
    "mixamorig:LeftForeArm": q_mul(SIT["mixamorig:LeftForeArm"], q_axis((1, 0, 0), -0.02)),
    "mixamorig:RightForeArm": q_mul(SIT["mixamorig:RightForeArm"], q_axis((1, 0, 0), 0.02)),
})

REACH = dict(STANCE)
REACH.update({
    SPINE: q_axis((1, 0, 0), 0.20),
    SPINE1: q_axis((1, 0, 0), 0.22),
    HEAD: q_axis((1, 0, 0), 0.08),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.85),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -0.25),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.25),
})

STEP = dict(leg_corrs(-0.55, 0.90, -0.30))
STEP.update({
    UP_L: q_axis((1, 0, 0), -0.80),
    LO_L: q_axis((1, 0, 0), 1.15),
    SPINE: q_axis((1, 0, 0), 0.16),
    SPINE1: q_axis((1, 0, 0), 0.18),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.55),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -0.15),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.15),
})
STEP_DROP = -14.0

LOWER = dict(leg_corrs(-1.00, 1.25, -0.50))
LOWER.update({
    SPINE: q_axis((1, 0, 0), 0.22),
    SPINE1: q_axis((1, 0, 0), 0.24),
    "mixamorig:LeftForeArm": q_axis((1, 0, 0), -0.35),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), 0.35),
    "mixamorig:LeftArm": q_axis((1, 0, 0), -0.10),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.10),
})
LOWER_DROP = -38.0

LEAN = dict(SIT)
LEAN.update({
    SPINE: q_axis((1, 0, 0), 0.30),
    SPINE1: q_axis((1, 0, 0), 0.30),
    NECK: q_axis((1, 0, 0), 0.10),
    "mixamorig:LeftForeArm": q_axis((1, 0, 0), -0.20),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), 0.20),
})

RISE = dict(leg_corrs(-0.70, 1.00, -0.35))
RISE.update({
    SPINE: q_axis((1, 0, 0), 0.26),
    SPINE1: q_axis((1, 0, 0), 0.28),
    "mixamorig:LeftForeArm": q_axis((1, 0, 0), -0.15),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), 0.15),
})
RISE_DROP = -18.0

STEPOUT = dict(leg_corrs(-0.25, 0.45, -0.15))
STEPOUT.update({
    UP_L: q_axis((1, 0, 0), -0.35),
    LO_L: q_axis((1, 0, 0), 0.50),
    SPINE: q_axis((1, 0, 0), 0.12),
    SPINE1: q_axis((1, 0, 0), 0.14),
})
STEPOUT_DROP = -5.0

# --- Door interaction (hand reaches the handle, then the door swings) ---
DOOR_REACH = dict(STANCE)
DOOR_REACH.update({
    SPINE: q_axis((1, 0, 0), 0.14),
    SPINE1: q_axis((1, 0, 0), 0.16),
    NECK: q_axis((1, 0, 0), 0.05),
    HEAD: q_axis((1, 0, 0), 0.08),
    "mixamorig:RightArm": q_axis((1, 0, 0), -1.05),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -0.45),
    "mixamorig:RightHand": q_axis((1, 0, 0), 0.20),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.12),
})

DOOR_GRIP = dict(STANCE)
DOOR_GRIP.update({
    SPINE: q_axis((1, 0, 0), 0.12),
    SPINE1: q_axis((1, 0, 0), 0.14),
    HEAD: q_axis((1, 0, 0), 0.05),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.95),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -0.70),
    "mixamorig:RightHand": q_axis((1, 0, 0), 0.35),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.10),
})

DOOR_PULL = dict(STANCE)
DOOR_PULL.update({
    SPINE: q_axis((1, 0, 0), 0.02),
    SPINE1: q_axis((1, 0, 0), 0.04),
    NECK: q_axis((1, 0, 0), -0.04),
    "mixamorig:RightArm": q_axis((1, 0, 0), -0.55),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -1.10),
    "mixamorig:RightHand": q_axis((1, 0, 0), 0.30),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.16),
})

DOOR_PUSH = dict(STANCE)
DOOR_PUSH.update({
    SPINE: q_axis((1, 0, 0), 0.22),
    SPINE1: q_axis((1, 0, 0), 0.24),
    HEAD: q_axis((1, 0, 0), 0.10),
    "mixamorig:RightArm": q_axis((1, 0, 0), -1.15),
    "mixamorig:RightForeArm": q_axis((1, 0, 0), -0.20),
    "mixamorig:RightHand": q_axis((1, 0, 0), 0.10),
    "mixamorig:LeftArm": q_axis((1, 0, 0), 0.20),
})


def build_clip(js, binary, joints, bind_rot, bind_trans, name, keys):
    """keys: [(time, rotation-corrections dict, hip_drop)]."""
    times = [k[0] for k in keys]
    time_acc, binary = append_accessor(js, binary, [(t,) for t in times], "SCALAR")
    channels = []
    samplers = []
    for node in joints:
        node_name = js["nodes"][node].get("name", "")
        base = BASE_ARM_ROT.get(node_name, bind_rot.get(node, (0.0, 0.0, 0.0, 1.0)))
        vals = []
        for _, corr, _ in keys:
            q = base
            for bone, c in corr.items():
                if bone == node_name:
                    q = q_mul(q, c)
            vals.append(q_norm(q))
        out, binary = append_accessor(js, binary, vals, "VEC4")
        samplers.append({"input": time_acc, "output": out, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1,
                         "target": {"node": node, "path": "rotation"}})

    hips = next((n for n in joints
                 if js["nodes"][n].get("name") == "mixamorig:Hips"), None)
    if hips is not None:
        bt = bind_trans.get(hips, (0.0, 0.0, 0.0))
        vals = [(bt[0], bt[1], bt[2] + drop) for _, _, drop in keys]
        out, binary = append_accessor(js, binary, vals, "VEC3")
        samplers.append({"input": time_acc, "output": out, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1,
                         "target": {"node": hips, "path": "translation"}})

    js["animations"].append({"name": name, "channels": channels, "samplers": samplers})
    return binary


def main():
    js, binary = read_glb_bytes(load_base())
    js["animations"] = [a for a in js.get("animations", [])
                        if a.get("name") not in ("Sit", "Board", "Stand",
                                                 "DoorOpen", "DoorClose")]
    joints = list(js["skins"][0]["joints"])
    bind_rot = {}
    bind_trans = {}
    for node in joints:
        n = js["nodes"][node]
        r = n.get("rotation", [0.0, 0.0, 0.0, 1.0])
        t = n.get("translation", [0.0, 0.0, 0.0])
        bind_rot[node] = tuple(r)
        bind_trans[node] = tuple(t)

    binary = build_clip(js, binary, joints, bind_rot, bind_trans, "Sit", [
        (0.0, SIT, -50.0),
        (1.0, SIT_BREATH, -50.0),
        (2.0, SIT, -50.0),
    ])
    binary = build_clip(js, binary, joints, bind_rot, bind_trans, "Board", [
        (0.00, STANCE, 0.0),
        (0.18, REACH, 0.0),
        (0.40, STEP, STEP_DROP),
        (0.62, LOWER, LOWER_DROP),
        (0.90, SIT, -50.0),
    ])
    binary = build_clip(js, binary, joints, bind_rot, bind_trans, "Stand", [
        (0.00, SIT, -50.0),
        (0.16, LEAN, -50.0),
        (0.38, RISE, RISE_DROP),
        (0.62, STEPOUT, STEPOUT_DROP),
        (0.90, STANCE, 0.0),
    ])
    binary = build_clip(js, binary, joints, bind_rot, bind_trans, "DoorOpen", [
        (0.00, STANCE, 0.0),
        (0.20, DOOR_REACH, 0.0),
        (0.36, DOOR_GRIP, 0.0),
        (0.62, DOOR_PULL, 0.0),
        (0.90, STANCE, 0.0),
    ])
    binary = build_clip(js, binary, joints, bind_rot, bind_trans, "DoorClose", [
        (0.00, STANCE, 0.0),
        (0.20, DOOR_REACH, 0.0),
        (0.36, DOOR_GRIP, 0.0),
        (0.62, DOOR_PUSH, 0.0),
        (0.90, STANCE, 0.0),
    ])

    js["buffers"][0]["byteLength"] = len(binary)
    write_glb(GLB, js, binary)
    print("animations:", [a.get("name") for a in js["animations"]])
    print("binary bytes:", len(binary))


if __name__ == "__main__":
    main()
