#!/usr/bin/env python3
"""Convert the ASCII FBX emitted by ``mapgeo2fbx`` into a textured glTF (.glb).

``mapgeo2fbx`` writes one FBX ``Geometry`` per MapGeo instance with positions,
normals and UVs, one ``Model`` with a local transform, and one ``Material``
(name only). NeonEngine's built-in FBX loader merges every node into a single
material-less mesh, which loses the Rift's 200 materials. This tool rebuilds a
glTF that the engine's normal glTF importer consumes: geometry grouped per
material (one primitive each), node transforms baked in, UVs preserved, and
each material pointed at a diffuse texture (PNG) chosen from the asset tree by
material-name keyword.

Usage::

    python tools/lolimport/mapgeo_fbx_to_gltf.py \
        --fbx F:/assets/sr/base_srx.fbx \
        --tex-dir F:/assets/sr/textures \
        --out projects/moba/assets/models/sr/sr_map.glb \
        [--limit N]
"""

import argparse
import glob
import json
import math
import os
import re
import struct
import numpy as np


# --------------------------------------------------------------------------
# Minimal streaming ASCII-FBX reader (only what mapgeo2fbx emits)
# --------------------------------------------------------------------------
def parse_array(text):
    """Parse the numbers after ``a:`` (comma separated), ignoring braces."""
    text = text.strip()
    if text.endswith("}"):
        text = text[:-1]
    if not text:
        return np.zeros(0, dtype=np.float64)
    return np.fromstring(text, dtype=np.float64, sep=",")


def read_fbx(path, limit=None, log=print):
    geoms = []          # {id, verts, normals, uvs, pvi, material_idx, ...}
    models = {}         # id -> {"name":..., "t":[..], "r":[..], "s":[..]}
    materials = {}      # id -> name
    mat_names = {}      # name -> id
    conns = []          # (child, parent)
    cur_geom = None
    cur_model = None
    pending = None      # (kind, target_obj) for multi-line arrays

    def finish_geom(g):
        if g is not None:
            geoms.append(g)

    with open(path, "r", encoding="latin-1") as fh:
        for line in fh:
            stripped = line.strip()
            m = re.match(r'^Geometry:\s*(\d+),\s*"([^"]*)"', stripped)
            if m:
                finish_geom(cur_geom)
                if limit and len(geoms) >= limit:
                    break
                cur_geom = {"id": int(m.group(1)), "name": m.group(2),
                            "verts": None, "normals": None, "uvs": None,
                            "pvi": None, "mat_poly": None}
                pending = None
                continue
            m = re.match(r'^Model:\s*(\d+),\s*"Model::([^"]*)"', stripped)
            if m:
                cur_model = {"id": int(m.group(1)), "name": m.group(2),
                             "t": [0.0, 0.0, 0.0], "r": [0.0, 0.0, 0.0],
                             "s": [1.0, 1.0, 1.0]}
                models[cur_model["id"]] = cur_model
                pending = None
                continue
            m = re.match(r'^Material:\s*(\d+),\s*"Material::([^"]*)"', stripped)
            if m:
                materials[int(m.group(1))] = m.group(2)
                pending = None
                continue
            if stripped.startswith("Connections:"):
                cur_geom = None
                cur_model = None
                pending = None
                continue
            m = re.match(r'^C:\s*"OO",\s*(\d+),\s*(\d+)', stripped)
            if m:
                conns.append((int(m.group(1)), int(m.group(2))))
                continue
            # local transform lines inside Model Properties70
            if cur_model is not None:
                mt = re.match(r'^P:\s*"Lcl Translation".*?,\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+)',
                              stripped)
                if mt:
                    cur_model["t"] = [float(mt.group(1)), float(mt.group(2)), float(mt.group(3))]
                    continue
                mr = re.match(r'^P:\s*"Lcl Rotation".*?,\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+)',
                              stripped)
                if mr:
                    cur_model["r"] = [float(mr.group(1)), float(mr.group(2)), float(mr.group(3))]
                    continue
                ms = re.match(r'^P:\s*"Lcl Scaling".*?,\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+),\s*(-?[\d.eE+]+)',
                              stripped)
                if ms:
                    cur_model["s"] = [float(ms.group(1)), float(ms.group(2)), float(ms.group(3))]
                    continue

            if cur_geom is not None:
                if pending is not None:
                    pending[1].append(stripped)
                    if "}" in stripped:
                        text = "".join(pending[1])
                        if "a:" in text:
                            text = text.split("a:", 1)[1]
                        text = text.replace("}", "").replace("{", " ")
                        cur_geom[pending[0]] = parse_array(text)
                        pending = None
                    continue
                for key, field in (("Vertices:", "verts"), ("Normals:", "normals"),
                                   ("UV:", "uvs"), ("PolygonVertexIndex:", "pvi"),
                                   ("Materials:", "mat_poly")):
                    if stripped.startswith(key):
                        pending = [field, [stripped[len(key):]]]
                        if "}" in stripped:
                            text = "".join(pending[1])
                            if "a:" in text:
                                text = text.split("a:", 1)[1]
                            text = text.replace("}", "").replace("{", " ")
                            cur_geom[field] = parse_array(text)
                            pending = None
                        break
                continue
    finish_geom(cur_geom)
    return geoms, models, materials, conns


# --------------------------------------------------------------------------
# Transform
# --------------------------------------------------------------------------
def trs_matrix(t, r_deg, s):
    rx, ry, rz = (math.radians(a) for a in r_deg)
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    # FBX default rotation order XYZ
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]], dtype=np.float64)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], dtype=np.float64)
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]], dtype=np.float64)
    R = Rz @ Ry @ Rx
    M = np.eye(4, dtype=np.float64)
    M[:3, :3] = R * np.array(s, dtype=np.float64)[None, :]
    M[:3, 3] = t
    return M


# --------------------------------------------------------------------------
# Material -> texture keyword heuristics
# --------------------------------------------------------------------------
CATEGORY = [
    ("water", (0.10, 0.30, 0.45)),
    ("foliage", (0.12, 0.30, 0.10)),
    ("tree", (0.12, 0.28, 0.10)),
    ("grass", (0.20, 0.36, 0.14)),
    ("bush", (0.14, 0.30, 0.12)),
    ("rock", (0.32, 0.31, 0.29)),
    ("rubble", (0.34, 0.32, 0.30)),
    ("stone", (0.36, 0.35, 0.33)),
    ("terrain", (0.30, 0.34, 0.20)),
    ("tile", (0.38, 0.37, 0.35)),
    ("gold", (0.55, 0.45, 0.18)),
    ("order", (0.35, 0.42, 0.55)),
    ("chaos", (0.50, 0.28, 0.24)),
    ("turret", (0.40, 0.40, 0.40)),
    ("nexus", (0.45, 0.42, 0.30)),
    ("vine", (0.18, 0.34, 0.14)),
]


def category_color(mat_name):
    low = mat_name.lower()
    for key, col in CATEGORY:
        if key in low:
            return col
    return (0.42, 0.40, 0.36)


# --------------------------------------------------------------------------
# GLB writer
# --------------------------------------------------------------------------
class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []

    def _align(self):
        while len(self.bin) % 4:
            self.bin.append(0)

    def add(self, data, target=None):
        self._align()
        off = len(self.bin)
        self.bin.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def accessor(self, view, ctype, atype, count, minv=None, maxv=None):
        a = {"bufferView": view, "componentType": ctype, "type": atype, "count": count}
        if minv is not None:
            a["min"] = minv
            a["max"] = maxv
        self.accessors.append(a)
        return len(self.accessors) - 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fbx", required=True)
    ap.add_argument("--tex-dir", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--category-colors", action="store_true",
                    help="ignore textures, use category colours (fast preview)")
    ap.add_argument("--merge-all", action="store_true",
                    help="merge every material group into ONE primitive")
    ap.add_argument("--center", action="store_true",
                    help="bake the map centre to the origin (for entity rotation)")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="bake a uniform scale into the vertices")
    args = ap.parse_args()

    print("parsing", args.fbx)
    geoms, models, materials, conns = read_fbx(args.fbx, limit=args.limit or None)
    print("geometries", len(geoms), "models", len(models),
          "materials", len(materials), "connections", len(conns))

    geom_ids = {g["id"] for g in geoms}
    geom_to_model = {}
    model_to_mat = {}
    for child, parent in conns:
        if child in geom_ids and parent in models:
            geom_to_model[child] = parent
        elif child in materials and parent in models:
            # Material is connected as a CHILD of its Model.
            model_to_mat[parent] = child
    print("geom->model", len(geom_to_model), "model->material", len(model_to_mat))

    # Texture index from the extracted WAD assets (flattened names use "__").
    # Only kitpiece/map textures participate; particle/skin textures are noise.
    NOISE = {"bake1", "pbr", "diffuse", "tx", "dm", "texture", "textures",
             "inst", "md", "final", "brown", "baked", "mat", "sr", "map",
             "base", "new", "place", "the", "001", "002", "003", "004", "005"}
    tex_pool = []  # (path, tokens)
    for p in glob.glob(os.path.join(args.tex_dir, "**", "*"), recursive=True):
        if not p.lower().endswith((".png", ".dds")):
            continue
        low = os.path.basename(p).lower()
        if "kitpieces" not in low:
            continue
        leaf = low.rsplit("__", 1)[-1]
        toks = []
        for t in re.split(r"[^a-z0-9]+", leaf):
            if not t:
                continue
            if t.isdigit():
                if len(t) <= 3:
                    toks.append(t)  # terrain/kitpiece index numbers
            elif len(t) >= 3 and t not in NOISE:
                toks.append(t)
        if toks:
            tex_pool.append((p, toks))

    # Explicit fallbacks for materials whose real texture shares no token.
    FORCE = [
        ("vertexdeform", "upgraded_baselayer_terrain_bake1_pbr_diffuse"),
        ("centermark", "base_center_mark3"),
        ("rubble", "base_stone_steps"),
        ("lambert", "base_stone_steps"),
        ("grass", "wallofgrass"),
    ]

    def find_tex(fragment):
        for p, _ in tex_pool:
            if fragment in os.path.basename(p).lower():
                return p
        return None

    def pick_texture(mat_name):
        if args.category_colors or not tex_pool:
            return None
        norm = re.sub(r"[^a-z0-9]", "", mat_name.split("/")[-1].lower())
        for key, frag in FORCE:
            if key in norm:
                p = find_tex(frag)
                if p:
                    return p
        # Token scoring: reward matching tokens, penalise extra texture tokens
        # (so "MountainLayer_Terrain" beats "Base_MountainLayer_Terrain").
        best, best_score = None, 0.0
        for p, toks in tex_pool:
            hit = sum(len(t) for t in toks if t in norm)
            miss = sum(len(t) for t in toks if t not in norm)
            score = hit - 0.45 * miss
            if score > best_score:
                best_score, best = score, p
        if best is not None and best_score >= 3.0:
            return best
        return None

    # Vegetation materials get alpha cut-out so foliage cards show leaves
    # instead of solid quads.
    FOLIAGE = ("drybush", "bush", "tree", "foliage", "grass", "vine",
               "wallofgrass", "plant", "flower")

    def is_foliage(mat_name):
        low = mat_name.lower()
        return any(k in low for k in FOLIAGE)

    # Group triangles by material name.
    groups = {}
    for g in geoms:
        pvi = g["pvi"]
        if pvi is None or g["verts"] is None:
            continue
        verts = g["verts"].reshape(-1, 3)
        model_id = geom_to_model.get(g["id"])
        mat_id = model_to_mat.get(model_id) if model_id else None
        mat_name = materials.get(mat_id, "_default")
        # decal/seam are alpha-blended overlays in the real renderer; opaque
        # they read as hard strips. Everything else (roads, gates, grass
        # walls, props) is legitimate kitpiece geometry and stays.
        if "decal" in mat_name.lower() or "seam" in mat_name.lower():
            continue
        M = trs_matrix(*[models[model_id][k] for k in ("t", "r", "s")]) if model_id else np.eye(4)
        world = verts @ M[:3, :3].T + M[:3, 3]

        normals = None
        if g["normals"] is not None:
            n3 = g["normals"].reshape(-1, 3)
            normals = n3 @ np.linalg.inv(M[:3, :3]).T
        uvs = g["uvs"].reshape(-1, 2) if g["uvs"] is not None else None

        # polygon indices -> triangles (triangulate fans)
        idx = []
        poly = []
        for v in pvi:
            if v < 0:
                poly.append(int(-v - 1))
                for k in range(1, len(poly) - 1):
                    idx.extend((poly[0], poly[k], poly[k + 1]))
                poly = []
            else:
                poly.append(int(v))
        if not idx:
            continue
        idx = np.array(idx, dtype=np.int64)

        grp = groups.setdefault(mat_name, {"pos": [], "nrm": [], "uv": [], "tri": [], "base": 0, "cent": []})
        grp["cent"].append(world.mean(axis=0))
        base = grp["base"]
        grp["pos"].append(world)
        grp["nrm"].append(normals if normals is not None else np.zeros_like(world))
        grp["uv"].append(uvs if uvs is not None else np.zeros((len(world), 2)))
        grp["tri"].append(idx + base)
        grp["base"] = base + len(world)

    print("material groups:", len(groups))

    # Structure centroids (blue/red bases) for lane alignment.
    for name, grp in groups.items():
        low = name.lower()
        if any(k in low for k in ("nexus", "turret", "shrine", "inhib", "barracks")):
            cs = np.array(grp["cent"])
            # cluster by sign of (x-cx) to separate the two sides
            print("  CENTROID %-60s n=%d mean=%s" % (
                name[:60], len(cs), [round(v, 0) for v in cs.mean(axis=0)]))
            for cpt in cs[:6]:
                print("      ", [round(v, 0) for v in cpt])

    # Optional bake: centre to origin + uniform scale (lets the scene entity
    # rotate about the map centre instead of the world origin).
    if args.center or args.scale != 1.0:
        allpos = np.concatenate([np.concatenate(grp["pos"]) for grp in groups.values()])
        c = (allpos.min(axis=0) + allpos.max(axis=0)) * 0.5
        print("centre", [round(v, 1) for v in c], "scale", args.scale)
        for grp in groups.values():
            for i in range(len(grp["pos"])):
                grp["pos"][i] = (grp["pos"][i] - c) * args.scale

    if args.merge_all:
        merged = {"pos": [], "nrm": [], "uv": [], "tri": [], "base": 0}
        for grp in groups.values():
            base = merged["base"]
            merged["pos"].append(np.concatenate(grp["pos"]))
            merged["nrm"].append(np.concatenate(grp["nrm"]))
            merged["uv"].append(np.concatenate(grp["uv"]))
            merged["tri"].append(np.concatenate(grp["tri"]) + base)
            merged["base"] = base + sum(len(p) for p in grp["pos"])
        groups = {"NRM_Terrain": merged}
        print("merged into 1 primitive")

    g = Glb()
    prims = []
    gltf_mats = []
    gltf_texs = []
    gltf_imgs = []
    gltf_samplers = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]

    tex_index = {}
    for mat_name, grp in groups.items():
        pos = np.concatenate(grp["pos"]).astype(np.float32)
        nrm = np.concatenate(grp["nrm"]).astype(np.float32)
        uv = np.concatenate(grp["uv"]).astype(np.float32)
        tri = np.concatenate(grp["tri"]).astype(np.uint32)

        pv = g.add(pos.tobytes(), 34962)
        pacc = g.accessor(pv, 5126, "VEC3", len(pos),
                          pos.min(axis=0).tolist(), pos.max(axis=0).tolist())
        nv = g.add(nrm.tobytes(), 34962)
        nacc = g.accessor(nv, 5126, "VEC3", len(nrm))
        uv_v = g.add(uv.tobytes(), 34962)
        uv_acc = g.accessor(uv_v, 5126, "VEC2", len(uv))
        iv = g.add(tri.tobytes(), 34963)
        iacc = g.accessor(iv, 5125, "SCALAR", len(tri))

        col = category_color(mat_name)
        mat = {
            "name": mat_name,
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [col[0], col[1], col[2], 1.0],
                "metallicFactor": 0.0, "roughnessFactor": 0.9,
            },
        }
        if is_foliage(mat_name):
            mat["alphaMode"] = "MASK"
            mat["alphaCutoffFactor"] = 0.4
        tex_path = pick_texture(mat_name)
        if tex_path is not None:
            name = os.path.basename(tex_path).lower()
            if name not in tex_index:
                with open(tex_path, "rb") as fh:
                    png = fh.read()
                view = g.add(png)
                gltf_imgs.append({"bufferView": view, "mimeType": "image/png"})
                gltf_texs.append({"source": len(gltf_imgs) - 1, "sampler": 0})
                tex_index[name] = len(gltf_texs) - 1
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": tex_index[name]}
            # white factor so the texture is shown at full brightness
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        gltf_mats.append(mat)
        prims.append({"attributes": {"POSITION": pacc, "NORMAL": nacc,
                                     "TEXCOORD_0": uv_acc},
                      "indices": iacc, "material": len(gltf_mats) - 1, "mode": 4})

    gltf = {
        "asset": {"version": "2.0", "generator": "neon mapgeo_fbx_to_gltf"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "SummonersRift"}],
        "meshes": [{"name": "SummonersRift", "primitives": prims}],
        "materials": gltf_mats,
        "buffers": [{"byteLength": 0}],
        "bufferViews": g.views,
        "accessors": g.accessors,
    }
    if gltf_imgs:
        gltf["images"] = gltf_imgs
        gltf["textures"] = gltf_texs
        gltf["samplers"] = gltf_samplers

    g._align()
    gltf["buffers"][0]["byteLength"] = len(g.bin)
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(g.bin)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, total))
        fh.write(struct.pack("<I4s", len(js), b"JSON"))
        fh.write(js)
        fh.write(struct.pack("<I4s", len(g.bin), b"BIN\x00"))
        fh.write(bytes(g.bin))
    print("wrote %s (%.1f MB, %d primitives, %d textures)" %
          (args.out, os.path.getsize(args.out) / 1e6, len(prims), len(gltf_texs)))


if __name__ == "__main__":
    main()
