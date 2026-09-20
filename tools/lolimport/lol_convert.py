#!/usr/bin/env python3
"""Convert League of Legends ``.lmesh`` / ``.lanim`` assets (tengge1/lol-model-viewer)
into glTF 2.0 binary (``.glb``) that NeonEngine imports directly.

The source format is the LoLKing / LoL model format parsed by the viewer's
``ZamModelViewer.Lol.Model`` code. This script replicates that parser and the
viewer's skinning math, then re-expresses everything as a standard glTF
skinned mesh:

* one glTF node per bone (bone index == node index), hierarchy from parents
* inverseBindMatrices = inverse(authored bind global)
* one animation per ``.lanim`` clip, TRS tracks in bone-local space
* the ``.lmesh`` texture embedded as a PNG inside the GLB

Usage::

    python tools/lolimport/lol_convert.py \
        --in  F:/assets/lol-model-viewer/LOLModelViewer/LOLModelViewer/resource \
        --out projects/moba/assets/models/lol \
        --ids 17,22,86

Layout produced::

    <out>/<Name>.glb
    <out>/index.json      (id -> {name, title, chinese, glb, anims})
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import zlib

LMESH_MAGIC = 604210091
LANIM_MAGIC = 604210092


# --------------------------------------------------------------------------
# Binary reader (little-endian, length-prefixed latin-1 strings)
# --------------------------------------------------------------------------
class Reader:
    def __init__(self, data):
        self.b = data
        self.p = 0

    def u8(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.b, self.p)[0]
        self.p += 4
        return v

    def string(self, length=None):
        if length is None:
            length = self.u16()
        s = self.b[self.p:self.p + length].decode("latin-1")
        self.p += length
        return s


# --------------------------------------------------------------------------
# Column-major 4x4 matrix helpers (same layout glTF / gl-matrix use)
# --------------------------------------------------------------------------
IDENT = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def mat_transpose(a):
    return [a[c * 4 + r] for r in range(4) for c in range(4)]


def mat_mul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return out


def mat_inverse(a):
    m = [[a[c * 4 + r] for c in range(4)] for r in range(4)]
    inv = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    for i in range(4):
        p = max(range(i, 4), key=lambda k: abs(m[k][i]))
        if abs(m[p][i]) < 1e-12:
            return None
        m[i], m[p] = m[p], m[i]
        inv[i], inv[p] = inv[p], inv[i]
        d = m[i][i]
        m[i] = [x / d for x in m[i]]
        inv[i] = [x / d for x in inv[i]]
        for k in range(4):
            if k == i:
                continue
            f = m[k][i]
            if f:
                m[k] = [m[k][j] - f * m[i][j] for j in range(4)]
                inv[k] = [inv[k][j] - f * inv[i][j] for j in range(4)]
    # back to column-major
    return [inv[r][c] for c in range(4) for r in range(4)]


# --------------------------------------------------------------------------
# .lmesh parser
# --------------------------------------------------------------------------
def parse_lmesh(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    r = Reader(raw)
    if r.u32() != LMESH_MAGIC:
        raise ValueError("bad lmesh magic in " + path)
    version = r.u32()
    anim_file = r.string()
    texture_file = r.string()

    num_meshes = r.u32()
    meshes = []
    for _ in range(num_meshes):
        meshes.append({
            "name": r.string().lower(),
            "v_start": r.u32(),
            "v_count": r.u32(),
            "i_start": r.u32(),
            "i_count": r.u32(),
        })

    num_verts = r.u32()
    verts = []
    for _ in range(num_verts):
        pos = [r.f32(), r.f32(), r.f32()]
        nrm = [r.f32(), r.f32(), r.f32()]
        u = r.f32()
        v = r.f32()
        bones = [r.u8(), r.u8(), r.u8(), r.u8()]
        weights = [r.f32(), r.f32(), r.f32(), r.f32()]
        verts.append({"pos": pos, "nrm": nrm, "uv": [u, v],
                      "bones": bones, "weights": weights})

    num_indices = r.u32()
    indices = [r.u16() for _ in range(num_indices)]

    num_bones = r.u32()
    bones = []
    for _ in range(num_bones):
        name = r.string().lower()
        parent = r.i32()
        scale = r.f32()
        orig = [r.f32() for _ in range(16)]
        incr = None
        if version >= 2:
            incr = [r.f32() for _ in range(16)]
        bones.append({"name": name, "parent": parent, "scale": scale,
                      "orig": orig, "incr": incr})

    return {
        "version": version,
        "anim_file": anim_file,
        "texture_file": texture_file,
        "meshes": meshes,
        "verts": verts,
        "indices": indices,
        "bones": bones,
    }


# --------------------------------------------------------------------------
# .lanim parser
# --------------------------------------------------------------------------
def parse_lanim(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    r = Reader(raw)
    if r.u32() != LANIM_MAGIC:
        raise ValueError("bad lanim magic in " + path)
    version = r.u32()
    if version >= 2:
        r = Reader(zlib.decompress(raw[r.p:]))

    num_anims = r.u32()
    anims = []
    for _ in range(num_anims):
        name = r.string().lower()
        fps = r.i32()
        num_bones = r.u32()
        bones = []
        for _ in range(num_bones):
            num_frames = r.u32()
            bone = r.string().lower()
            flags = r.u32()
            frames = []
            for _ in range(num_frames):
                pos = [r.f32(), r.f32(), r.f32()]
                rot = [r.f32(), r.f32(), r.f32(), r.f32()]
                scale = [r.f32(), r.f32(), r.f32()] if version >= 3 else [1.0, 1.0, 1.0]
                frames.append({"pos": pos, "rot": rot, "scale": scale})
            bones.append({"name": bone, "flags": flags, "frames": frames})
        anims.append({"name": name, "fps": fps, "bones": bones})
    return anims


# --------------------------------------------------------------------------
# GLB builder
# --------------------------------------------------------------------------
class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.buffer_views = []
        self.accessors = []

    def _align(self, n=4):
        while len(self.bin) % n:
            self.bin.append(0)

    def add_bytes(self, data, target=None):
        self._align(4)
        offset = len(self.bin)
        self.bin.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def add_accessor(self, view, comp_type, acc_type, count, byte_offset=0,
                     minv=None, maxv=None):
        acc = {"bufferView": view, "componentType": comp_type,
               "type": acc_type, "count": count}
        if byte_offset:
            acc["byteOffset"] = byte_offset
        if minv is not None:
            acc["min"] = minv
        if maxv is not None:
            acc["max"] = maxv
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def floats(self, values, acc_type, count, target=None):
        data = struct.pack("<%df" % len(values), *values)
        view = self.add_bytes(data, target)
        return self.add_accessor(view, 5126, acc_type, count)

    def u16s(self, values):
        data = struct.pack("<%dH" % len(values), *values)
        view = self.add_bytes(data, 34963)
        return self.add_accessor(view, 5123, "SCALAR", len(values))

    def u32s(self, values):
        data = struct.pack("<%dI" % len(values), *values)
        view = self.add_bytes(data, 34963)
        return self.add_accessor(view, 5125, "SCALAR", len(values))


def quat_normalize(q):
    n = math.sqrt(sum(x * x for x in q))
    if n < 1e-8:
        return [0.0, 0.0, 0.0, 1.0]
    return [x / n for x in q]


# --------------------------------------------------------------------------
# Conversion
# --------------------------------------------------------------------------
def convert(resource_dir, out_dir, stem, name, tex_subdir, log=print):
    models_dir = os.path.join(resource_dir, "models")
    lmesh_path = os.path.join(models_dir, stem + ".lmesh")
    if not os.path.isfile(lmesh_path):
        log("  skip %s: no %s" % (name, lmesh_path))
        return None

    mesh = parse_lmesh(lmesh_path)
    lanim_path = os.path.join(models_dir, mesh["anim_file"] + ".lanim")
    anims = parse_lanim(lanim_path) if os.path.isfile(lanim_path) else []

    bones = mesh["bones"]
    nb = len(bones)
    # Engine shader limit (uBoneMatrices[128]); beyond it, weighted vertices
    # collapse to skin=0 and tear spikes across the scene.
    if nb > 128:
        raise ValueError("%s: %d bones exceeds the engine skin limit of 128" % (name, nb))

    # Zero weights on out-of-range joint indices (padding bytes) so GPU
    # skinning can't read a garbage bone matrix and spike a vertex across the map.
    for v in mesh["verts"]:
        for k in range(4):
            if v["bones"][k] >= nb:
                v["weights"][k] = 0.0

    # Authored bind GLOBALS (column-major) and their inverses (inverse bind).
    orig = [mat_transpose(b["orig"]) for b in bones]
    ibm = []
    for i, o in enumerate(orig):
        inv = mat_inverse(o)
        if inv is None:
            inv = list(IDENT)
        ibm.append(inv)
    local = []
    for i, b in enumerate(bones):
        p = b["parent"]
        if p >= 0:
            local.append(mat_mul(ibm[p], orig[i]))
        else:
            local.append(orig[i])

    # Name -> model bone index (dedupe like the viewer: append "2").
    name_to_index = {}
    for i, b in enumerate(bones):
        nm = b["name"]
        if nm in name_to_index:
            nm = nm + "2"
        name_to_index[nm] = i

    g = Glb()
    primitives = []
    material_index = 0
    materials = []
    textures = []
    images = []
    tex_samplers = []
    tex_samplers.append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})

    image_index = None
    png_path = None
    if mesh["texture_file"]:
        cand = os.path.join(resource_dir, "textures", tex_subdir,
                            mesh["texture_file"] + ".png")
        if os.path.isfile(cand):
            png_path = cand
    if png_path is None:
        tex_root = os.path.join(resource_dir, "textures", tex_subdir)
        if os.path.isdir(tex_root):
            pngs = sorted(f for f in os.listdir(tex_root) if f.lower().endswith(".png"))
            if pngs:
                png_path = os.path.join(tex_root, pngs[0])
    if png_path:
        with open(png_path, "rb") as fh:
            png = fh.read()
        view = g.add_bytes(png)
        images.append({"bufferView": view, "mimeType": "image/png"})
        textures.append({"source": 0, "sampler": 0})
        image_index = 0
    materials.append({
        "name": name + "_mat",
        "doubleSided": True,
        "alphaMode": "OPAQUE",
        "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.65,
        },
    })
    if image_index is not None:
        materials[0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    if not textures:
        # material with no texture: keep index 0 valid anyway
        pass

    # For each LoL submesh build a primitive with its own sliced attributes.
    for m in mesh["meshes"]:
        v0 = m["v_start"]
        vc = m["v_count"]
        i0 = m["i_start"]
        ic = m["i_count"]
        if vc <= 0 or ic <= 0:
            continue
        sub = mesh["verts"][v0:v0 + vc]
        idx = mesh["indices"][i0:i0 + ic]
        # rebase indices to local vertex range
        idx = [i - v0 for i in idx]
        if any(i < 0 or i >= vc for i in idx):
            raise ValueError("mesh %s: index out of range" % m["name"])

        positions = []
        normals = []
        uvs = []
        joints = []
        weights = []
        for v in sub:
            positions.extend(v["pos"])
            normals.extend(v["nrm"])
            uvs.extend(v["uv"])
            joints.extend(v["bones"])
            weights.extend(v["weights"])

        minp = [min(positions[k::3]) for k in range(3)]
        maxp = [max(positions[k::3]) for k in range(3)]

        pos_data = struct.pack("<%df" % len(positions), *positions)
        pv = g.add_bytes(pos_data, 34962)
        pos_acc = g.add_accessor(pv, 5126, "VEC3", len(sub), minv=minp, maxv=maxp)

        nv = g.add_bytes(struct.pack("<%df" % len(normals), *normals), 34962)
        nrm_acc = g.add_accessor(nv, 5126, "VEC3", len(sub))

        uv = g.add_bytes(struct.pack("<%df" % len(uvs), *uvs), 34962)
        uv_acc = g.add_accessor(uv, 5126, "VEC2", len(sub))

        need_u32 = vc > 65535
        jbuf = struct.pack("<%d%s" % (len(joints), "I" if need_u32 else "H"), *joints)
        jv = g.add_bytes(jbuf, 34962)
        j_acc = g.add_accessor(jv, 5125 if need_u32 else 5123, "VEC4", len(sub))

        wv = g.add_bytes(struct.pack("<%df" % len(weights), *weights), 34962)
        w_acc = g.add_accessor(wv, 5126, "VEC4", len(sub))

        if need_u32:
            idx_acc = g.u32s(idx)
        else:
            idx_acc = g.u16s(idx)

        primitives.append({
            "attributes": {
                "POSITION": pos_acc,
                "NORMAL": nrm_acc,
                "TEXCOORD_0": uv_acc,
                "JOINTS_0": j_acc,
                "WEIGHTS_0": w_acc,
            },
            "indices": idx_acc,
            "material": material_index,
            "mode": 4,
        })

    if not primitives:
        log("  skip %s: no drawable submesh" % name)
        return None

    # Inverse bind matrices accessor (MAT4, column-major, no padding).
    ibm_flat = []
    for m in ibm:
        ibm_flat.extend(m)
    ibm_view = g.add_bytes(struct.pack("<%df" % len(ibm_flat), *ibm_flat))
    ibm_acc = g.add_accessor(ibm_view, 5126, "MAT4", nb)

    # Nodes: bones 0..nb-1, then the mesh node at index nb.
    nodes = []
    for i, b in enumerate(bones):
        node = {
            "name": b["name"] if b["name"] else ("bone%d" % i),
            "matrix": local[i],
        }
        children = [j for j, bb in enumerate(bones) if bb["parent"] == i]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node_index = nb
    nodes.append({
        "name": name + "_mesh",
        "mesh": 0,
        "skin": 0,
    })

    roots = [i for i, b in enumerate(bones) if b["parent"] < 0]
    scene_nodes = roots + [mesh_node_index]

    skin = {
        "joints": list(range(nb)),
        "inverseBindMatrices": ibm_acc,
        "skeleton": roots[0] if roots else 0,
    }

    # Animations.
    gltf_anims = []
    for anim in anims:
        fps = anim["fps"] if anim["fps"] > 1 else 1
        fps = max(fps, 1)
        channels = []
        a_samplers = []
        for ab in anim["bones"]:
            bi = name_to_index.get(ab["name"])
            if bi is None:
                continue
            frames = ab["frames"]
            if not frames:
                continue
            n = len(frames)
            times = [i / float(fps) for i in range(n)]
            # Wrap key so the clip loops over n/fps seconds.
            times.append(n / float(fps))
            trans = []
            rot = []
            for f in frames:
                trans.extend(f["pos"])
                rot.extend(quat_normalize(f["rot"]))
            trans.extend(frames[0]["pos"])
            rot.extend(quat_normalize(frames[0]["rot"]))

            t_acc = g.floats(times, "SCALAR", len(times), target=None)
            tr_acc = g.floats(trans, "VEC3", len(times), target=None)
            ro_acc = g.floats(rot, "VEC4", len(times), target=None)
            s_acc = g.floats([1.0, 1.0, 1.0] * len(times), "VEC3", len(times), target=None)

            s_t = len(a_samplers)
            a_samplers.append({"input": t_acc, "output": tr_acc, "interpolation": "LINEAR"})
            s_r = len(a_samplers)
            a_samplers.append({"input": t_acc, "output": ro_acc, "interpolation": "LINEAR"})
            s_s = len(a_samplers)
            a_samplers.append({"input": t_acc, "output": s_acc, "interpolation": "LINEAR"})

            channels.append({"sampler": s_t, "target": {"node": bi, "path": "translation"}})
            channels.append({"sampler": s_r, "target": {"node": bi, "path": "rotation"}})
            channels.append({"sampler": s_s, "target": {"node": bi, "path": "scale"}})
        if not channels:
            continue
        gltf_anims.append({"name": anim["name"], "channels": channels, "samplers": a_samplers})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon lol_convert.py"},
        "scene": 0,
        "scenes": [{"name": name, "nodes": scene_nodes}],
        "nodes": nodes,
        "meshes": [{"name": name, "primitives": primitives}],
        "skins": [skin],
        "materials": materials,
        "buffers": [{"byteLength": 0}],
        "bufferViews": g.buffer_views,
        "accessors": g.accessors,
    }
    if gltf_anims:
        gltf["animations"] = gltf_anims
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        gltf["samplers"] = tex_samplers

    # Serialize GLB.
    g._align(4)
    gltf["buffers"][0]["byteLength"] = len(g.bin)
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "
    bin_bytes = bytes(g.bin)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    with open(os.path.join(out_dir, name + ".glb"), "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, total))
        fh.write(struct.pack("<I4s", len(json_bytes), b"JSON"))
        fh.write(json_bytes)
        fh.write(struct.pack("<I4s", len(bin_bytes), b"BIN\x00"))
        fh.write(bin_bytes)

    return {
        "glb": name + ".glb",
        "anims": [a["name"] for a in anims],
        "bones": nb,
        "texture": os.path.basename(png_path) if png_path else None,
    }


# --------------------------------------------------------------------------
# Champion table (parsed from the viewer's data.js)
# --------------------------------------------------------------------------
def load_champions(resource_dir):
    js_path = os.path.join(resource_dir, "js", "data.js")
    if not os.path.isfile(js_path):
        return {}
    text = open(js_path, encoding="utf-8", errors="replace").read()
    champs = {}
    for m in re.finditer(r'\{\s*"id":\s*(\d+),\s*"name":\s*"([^"]*)",\s*'
                         r'"title":\s*"([^"]*)",\s*"chinese":\s*"([^"]*)"\s*\}', text):
        champs[int(m.group(1))] = {
            "name": m.group(2),
            "title": m.group(3),
            "chinese": m.group(4),
        }
    return champs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="resource", required=True,
                    help="lol-model-viewer .../resource directory")
    ap.add_argument("--out", dest="out", required=True, help="output directory")
    ap.add_argument("--ids", default="", help="comma separated champion ids")
    ap.add_argument("--names", default="",
                    help="comma separated raw model stems (e.g. sru_baron_0)")
    ap.add_argument("--all", action="store_true", help="convert every .lmesh")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    champions = load_champions(args.resource)

    index_path = os.path.join(args.out, "index.json")
    index = {}
    if os.path.isfile(index_path):
        with open(index_path, encoding="utf-8") as fh:
            index = json.load(fh)

    jobs = []  # (stem, out_name, tex_subdir, meta)
    if args.all:
        for fn in sorted(os.listdir(os.path.join(args.resource, "models"))):
            m = re.match(r"^(.+)_0\.lmesh$", fn)
            if not m:
                continue
            stem = m.group(1)
            if stem.isdigit():
                info = champions.get(int(stem), {})
                name = info.get("name") or ("champion_%s" % stem)
                safe = re.sub(r"[^A-Za-z0-9_]", "", name) or ("champion_%s" % stem)
                jobs.append((stem + "_0", safe, stem, info))
            else:
                safe = re.sub(r"[^A-Za-z0-9_]", "", stem) or stem
                jobs.append((stem + "_0", safe, stem, {}))
    else:
        if args.ids:
            for cid in [int(x) for x in re.split(r"[,\s]+", args.ids) if x.strip()]:
                info = champions.get(cid, {})
                name = info.get("name") or ("champion_%d" % cid)
                safe = re.sub(r"[^A-Za-z0-9_]", "", name) or ("champion_%d" % cid)
                jobs.append(("%d_0" % cid, safe, str(cid), info))
        if args.names:
            for stem in [x for x in re.split(r"[,\s]+", args.names) if x.strip()]:
                stem = re.sub(r"_0$", "", stem)
                safe = re.sub(r"[^A-Za-z0-9_]", "", stem) or stem
                jobs.append((stem + "_0", safe, stem, {}))
    if not jobs:
        ap.error("pass --ids, --names or --all")

    for stem, safe, tex_subdir, info in jobs:
        print("[%s] %s" % (stem, safe))
        try:
            entry = convert(args.resource, args.out, stem, safe, tex_subdir)
        except Exception as exc:  # noqa: BLE001 - report and continue batch
            print("  ERROR: %s" % exc)
            continue
        if entry:
            entry.update({"name": info.get("name", safe),
                          "title": info.get("title", ""),
                          "chinese": info.get("chinese", "")})
            if stem[:-2].isdigit():
                entry["id"] = int(stem[:-2])
            index[stem[:-2]] = entry
            print("  ok: %d bones, %d clips, tex=%s" %
                  (entry["bones"], len(entry["anims"]), entry["texture"]))

    with open(index_path, "w", encoding="utf-8") as fh:
        json.dump(index, fh, ensure_ascii=False, indent=2, sort_keys=True)
    print("wrote %d models to %s" % (len(index), args.out))


if __name__ == "__main__":
    main()
