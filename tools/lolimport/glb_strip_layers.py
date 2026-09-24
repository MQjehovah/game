#!/usr/bin/env python3
"""Strip the stacked state-layers out of an already-baked map .glb.

Summoner's Rift ships every visual state of the map at the *same* coordinates:
``mapgeo2fbx`` writes one Geometry per MapGeo instance, and each kit-piece cell
exists once per state - the shipping map (``_default_*``), the terrain-layer set
(``_blend_master_*`` x BaseLayer/ChemtechLayer/HextechLayer/CloudLayer/
OceanLayer/InfernalLayer/MountainLayer x Base/Tunnel/Upgraded/Walled) and a
third copy under ``_earth_*``. ``mapgeo_fbx_to_gltf.py`` groups geometry by
material name into ONE mesh, so the asset draws every state on top of the next
and the ground becomes a z-fighting patchwork (in ``sr_map.glb``: 21 groups of
primitives sharing an identical POSITION bound box, 66 primitives in total).

The engine can filter layers at draw time (``SceneMesh::materialExclude``), but
the stale geometry still ships, still uploads to the GPU and still costs a draw
call per primitive. This tool removes it from the asset instead: it drops the
state layers, garbage-collects the now-unused materials / textures / images /
accessors / bufferViews and repacks the binary chunk, so the .glb contains
exactly one state and everything remaining still refers to it.

Usage::

    python tools/lolimport/glb_strip_layers.py --in <map>.glb --out <clean>.glb
                                               [--dedupe-bbox] [--dry-run]

``--in-place`` rewrites the input (the repo tracks the .glb, so git is the
backup). The default rule is the Summoner's Rift state-layer list; ``--keep`` /
``--exclude`` override it.
"""

import argparse
import collections
import hashlib
import json
import os
import struct
import sys

import numpy as np

# Material-name substrings that mark a NON-base visual state of the Rift.
# Dropping these leaves the shipping map: base terrain + props + foliage.
SR_STATE_LAYERS = [
    "ChemtechLayer", "HextechLayer", "CloudLayer", "OceanLayer",
    "InfernalLayer", "MountainLayer",
    "_Tunnel_", "_Upgraded_", "_Walled_", "_earth_",
]


def read_glb(path):
    """Parse a .glb into (gltf json, binary chunk)."""
    with open(path, "rb") as fh:
        magic, version, total = struct.unpack("<III", fh.read(12))
        if magic != 0x46546C67:
            raise SystemExit("%s: not a .glb (bad magic)" % path)
        if version != 2:
            raise SystemExit("%s: only glTF 2.0 binary is supported" % path)
        gltf, bin_chunk = None, b""
        while fh.tell() < total:
            head = fh.read(8)
            if len(head) < 8:
                break
            length, kind = struct.unpack("<II", head)
            payload = fh.read(length)
            if kind == 0x4E4F534A:      # 'JSON'
                gltf = json.loads(payload.decode("utf-8"))
            elif kind == 0x004E4942:    # 'BIN-NUL'
                bin_chunk = payload
        if gltf is None:
            raise SystemExit("%s: no JSON chunk" % path)
        return gltf, bin_chunk


def write_glb(path, gltf, bin_chunk):
    """Serialise a (gltf, bin) pair, both chunks 4-byte aligned."""
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    blob = bytearray(bin_chunk)
    while len(blob) % 4:
        blob += b"\x00"
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, total))
        fh.write(struct.pack("<I4s", len(js), b"JSON"))
        fh.write(js)
        fh.write(struct.pack("<I4s", len(blob), b"BIN\x00"))
        fh.write(bytes(blob))


def material_leaf(gltf, index):
    """Last path component of a glTF material name (mapgeo names are paths)."""
    if index is None or index >= len(gltf.get("materials", [])):
        return ""
    return gltf["materials"][index].get("name", "").split("/")[-1]


def position_bounds(gltf, prim, quantize=2):
    """Rounded POSITION min/max of a primitive, or None when unannotated."""
    acc = gltf["accessors"][prim["attributes"]["POSITION"]]
    lo, hi = acc.get("min"), acc.get("max")
    if not lo or not hi:
        return None
    return (tuple(round(v, quantize) for v in lo),
            tuple(round(v, quantize) for v in hi))


def strip_primitives(gltf, exclude, keep, dedupe_bbox):
    """Drop primitives by material-name rule and (optionally) by shared bounds."""
    dropped_names = collections.Counter()
    dropped_reasons = []
    kept = dropped = 0
    seen_bounds = {}
    for mesh in gltf.get("meshes", []):
        out = []
        for prim in mesh.get("primitives", []):
            name = material_leaf(gltf, prim.get("material"))
            reason = None
            if keep:
                if not any(k and k in name for k in keep):
                    reason = "not in --keep"
            elif any(e and e in name for e in exclude):
                reason = "state layer"
            if reason is None and dedupe_bbox:
                bounds = position_bounds(gltf, prim)
                if bounds is not None:
                    if bounds in seen_bounds:
                        reason = "coincident bbox with %s" % seen_bounds[bounds]
                    else:
                        seen_bounds[bounds] = name
            if reason is None:
                out.append(prim)
                kept += 1
            else:
                dropped += 1
                dropped_names[name or "<no material>"] += 1
                dropped_reasons.append((name or "<no material>", reason))
        mesh["primitives"] = out
    return kept, dropped, dropped_names, dropped_reasons


def collect_refs(gltf):
    """Accessor / bufferView indices reachable from the kept primitives."""
    acc_used, view_used = set(), set()

    def use_accessor(i):
        if i is None or i < 0:
            return
        acc_used.add(i)
        bv = gltf["accessors"][i].get("bufferView")
        if bv is not None:
            view_used.add(bv)

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            for acc in prim.get("attributes", {}).values():
                use_accessor(acc)
            use_accessor(prim.get("indices"))
            for targets in prim.get("targets", []):
                for acc in targets.values():
                    use_accessor(acc)
    for skin in gltf.get("skins", []):
        use_accessor(skin.get("inverseBindMatrices"))
    for anim in gltf.get("animations", []):
        for sampler in anim.get("samplers", []):
            use_accessor(sampler.get("input"))
            use_accessor(sampler.get("output"))
    return acc_used, view_used


def material_texture_refs(mat):
    """Every texture index a material points at (all standard textureInfo slots).

    Walks nested objects generically so material extensions that carry their own
    textureInfo (clearcoat, sheen, ...) keep their picture too instead of being
    silently left with a dangling index.
    """
    refs = set()

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "index" and isinstance(value, int):
                    refs.add(value)
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(mat)
    return refs


def embed_texture(src, max_size=1024, quality=90):
    """Encode a source texture for embedding: (bytes, mimeType).

    Mirrors mapgeo_fbx_to_gltf.py: images with a real alpha channel stay PNG
    (foliage cut-outs need it), fully opaque ones become JPEG to keep the glb
    small. Oversized sources are downscaled first.

    `src` is either a path or an already-open PIL image (callers that need the
    alpha statistics anyway should pass the image to avoid decoding twice).
    """
    from PIL import Image  # local import: only texture replacement needs it
    import io

    img = src if hasattr(src, "mode") and hasattr(src, "size") else Image.open(src)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    if img.width > max_size or img.height > max_size:
        img.thumbnail((max_size, max_size), Image.LANCZOS)
    arr = np.asarray(img)
    if int(arr[..., 3].min()) >= 250:
        buf = io.BytesIO()
        Image.fromarray(arr[..., :3], "RGB").save(buf, format="JPEG", quality=quality)
        return buf.getvalue(), "image/jpeg"
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue(), "image/png"


def rebuild(gltf, bin_chunk, set_textures=None, alpha_modes=None,
            exact_textures=None):
    """Repack bufferViews/accessors/materials/textures/images; return new bin.

    Only data still referenced by a kept primitive survives, so dropping
    primitives actually frees the vertex bytes (and the pictures no longer used
    by any surviving material) instead of leaving dead weight in the .glb.

    ``set_textures`` is [(material-name substring, image bytes, mime, alphaMode)]
    and replaces the diffuse texture of every surviving material whose name
    matches - the way a wrong guess from an importer gets corrected without a
    full re-import. ``alpha_modes`` is [(substring, "opaque"|"mask"|"blend")].

    ``exact_textures`` is {full material name: (image bytes, mime, alphaMode)} and
    wins over ``set_textures``. A whole-map remap needs it: a substring key like
    "lambert150" would also hit "lambert150_001".

    Materials that ask for byte-identical pictures share one image and texture.
    """
    set_textures = list(set_textures or [])
    alpha_modes = list(alpha_modes or [])
    exact_textures = dict(exact_textures or {})
    acc_used, view_used = collect_refs(gltf)
    textures = gltf.get("textures", [])
    images = gltf.get("images", [])

    # Which materials (and therefore textures/images) survive the strip.
    wanted_mats = []
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if prim.get("material") is not None:
                wanted_mats.append(prim["material"])
    wanted_mats = set(wanted_mats)

    def leaf(index):
        return gltf["materials"][index].get("name", "").split("/")[-1]

    # New diffuse textures requested on the command line, per material.
    swapped = {}          # material index -> (bytes, mime, alphaMode)
    for i in sorted(wanted_mats):
        name = leaf(i)
        for sub, data, mime, mode in set_textures:
            if sub and sub in name:
                swapped[i] = (data, mime, mode)
                break
    for i in sorted(wanted_mats):
        full = gltf["materials"][i].get("name", "")
        if full in exact_textures:
            swapped[i] = exact_textures[full]
    forced_alpha = {}
    for i in sorted(wanted_mats):
        name = leaf(i)
        for sub, mode in alpha_modes:
            if sub and sub in name:
                forced_alpha[i] = mode
                break

    tex_used = set()
    for i in wanted_mats:
        mat = gltf["materials"][i]
        if i in swapped:
            # Its diffuse is being replaced, so the picture it used to point at
            # is only still needed if some OTHER slot (or material) references it.
            mat = json.loads(json.dumps(mat))
            mat.get("pbrMetallicRoughness", {}).pop("baseColorTexture", None)
        for t in material_texture_refs(mat):
            if 0 <= t < len(textures):
                tex_used.add(t)

    image_used = set()
    for i in tex_used:
        src = textures[i].get("source")
        if src is None:
            continue
        img = images[src]
        if img.get("bufferView") is None:
            raise SystemExit("image %d has no bufferView (external-URI images "
                             "are not supported by this tool)" % src)
        image_used.add(src)
        view_used.add(img["bufferView"])

    new_bin = bytearray()
    packed_views = {}
    for i, view in enumerate(gltf.get("bufferViews", [])):
        if i not in view_used:
            continue
        while len(new_bin) % 4:
            new_bin += b"\x00"
        off, length = view.get("byteOffset", 0), view["byteLength"]
        packed_views[i] = dict(view, buffer=0, byteOffset=len(new_bin))
        new_bin += bin_chunk[off:off + length]
    views = [packed_views[o] for o in sorted(packed_views)]
    view_index = {old: new for new, old in enumerate(sorted(packed_views))}

    # Replacement pictures ride along in the same buffer, one view per distinct
    # payload: a map names the same art from several materials (mapgeo's
    # _default_X / _blend_master_X pairs), so carrying identical bytes twice
    # only inflates the file.
    extra_views = []
    payload_slot = {}    # (sha1, mime) -> (view index, mime)
    mat_payload = {}     # material index -> (sha1, mime)
    for mat_index in sorted(swapped):
        data, mime, mode = swapped[mat_index]
        key = (hashlib.sha1(data).hexdigest(), mime)
        mat_payload[mat_index] = key
        if key in payload_slot:
            continue
        while len(new_bin) % 4:
            new_bin += b"\x00"
        extra_views.append({"buffer": 0, "byteOffset": len(new_bin),
                            "byteLength": len(data)})
        new_bin += data
        payload_slot[key] = (len(views) + len(extra_views) - 1, mime)

    new_acc, acc_index = [], {}
    for i, acc in enumerate(gltf.get("accessors", [])):
        if i not in acc_used:
            continue
        acc = dict(acc)
        if acc.get("bufferView") is not None:
            acc["bufferView"] = view_index[acc["bufferView"]]
        acc_index[i] = len(new_acc)
        new_acc.append(acc)

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            prim["attributes"] = {k: acc_index[v]
                                  for k, v in prim["attributes"].items()}
            if prim.get("indices") is not None:
                prim["indices"] = acc_index[prim["indices"]]
    for skin in gltf.get("skins", []):
        if skin.get("inverseBindMatrices") is not None:
            skin["inverseBindMatrices"] = acc_index[skin["inverseBindMatrices"]]
    for anim in gltf.get("animations", []):
        for sampler in anim.get("samplers", []):
            sampler["input"] = acc_index[sampler["input"]]
            sampler["output"] = acc_index[sampler["output"]]

    new_images, image_index = [], {}
    for i, img in enumerate(images):
        if i not in image_used:
            continue
        image_index[i] = len(new_images)
        new_images.append(dict(img, bufferView=view_index[img["bufferView"]]))

    new_textures, tex_index = [], {}
    for i, tex in enumerate(textures):
        if i not in tex_used:
            continue
        tex_index[i] = len(new_textures)
        tex = dict(tex)
        if tex.get("source") is not None:
            tex["source"] = image_index[tex["source"]]
        new_textures.append(tex)
    # One image + texture per distinct payload; every material that asked for it
    # is pointed at the shared entry in the loop below.
    swapped_texture = {}
    for key, (view, mime) in payload_slot.items():
        new_images.append({"bufferView": view, "mimeType": mime})
        new_textures.append({"source": len(new_images) - 1, "sampler": 0})
        swapped_texture[key] = len(new_textures) - 1
    swapped = {i: (swapped_texture[mat_payload[i]], mode)
               for i, (data, mime, mode) in swapped.items()}
    if extra_views and "samplers" not in gltf:
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]

    def remap_textures(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "index" and isinstance(value, int) and value in tex_index:
                    node[key] = tex_index[value]
                else:
                    remap_textures(value)
        elif isinstance(node, list):
            for item in node:
                remap_textures(item)

    new_mats, mat_index = [], {}
    for i, mat in enumerate(gltf.get("materials", [])):
        if i not in wanted_mats:
            continue
        mat = json.loads(json.dumps(mat))  # deep copy on purpose
        remap_textures(mat)
        if i in swapped:
            tex, mode = swapped[i]
            pbr = mat.setdefault("pbrMetallicRoughness", {})
            pbr["baseColorTexture"] = {"index": tex}
            # The picture IS the albedo now, so the factor has to be the
            # identity. Anything else left on the material is the importer's
            # category-colour preview palette (see category_color() in
            # mapgeo_fbx_to_gltf.py) - keeping it would multiply the real
            # diffuse down to a dark tinted wash.
            pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
            if mode:
                mat["alphaMode"] = mode.upper()
                # glTF spells the enum "MASK"/"BLEND" and this engine compares it
                # verbatim, so normalise rather than trusting the caller's case.
                if mode.lower() == "mask":
                    mat["alphaCutoff"] = 0.4
        if i in forced_alpha:
            mode = forced_alpha[i]
            if mode == "opaque":
                mat.pop("alphaMode", None)
                mat.pop("alphaCutoff", None)
            else:
                mat["alphaMode"] = mode.upper()
                if mode.lower() == "mask":
                    mat["alphaCutoff"] = 0.4
        mat_index[i] = len(new_mats)
        new_mats.append(mat)
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if prim.get("material") is not None:
                prim["material"] = mat_index[prim["material"]]

    gltf["bufferViews"] = views + extra_views
    gltf["accessors"] = new_acc
    gltf["materials"] = new_mats
    if new_textures or "textures" in gltf:
        gltf["textures"] = new_textures
    if new_images or "images" in gltf:
        gltf["images"] = new_images
    gltf["buffers"] = [{"byteLength": len(new_bin)}]
    return bytes(new_bin)


def parse_overrides(specs, alpha_specs):
    """Turn --set-texture/--alpha CLI strings into the forms rebuild() wants."""
    set_textures = []
    for spec in specs or []:
        if "=" not in spec:
            raise SystemExit("--set-texture expects <material-substring>=<image>")
        sub, path = spec.split("=", 1)
        if not os.path.isfile(path):
            raise SystemExit("--set-texture image not found: %s" % path)
        data, mime = embed_texture(path)
        set_textures.append((sub, data, mime, None))
    alpha_modes = []
    for spec in alpha_specs or []:
        if "=" not in spec:
            raise SystemExit("--alpha expects <material-substring>=opaque|mask|blend")
        sub, mode = spec.split("=", 1)
        if mode not in ("opaque", "mask", "blend"):
            raise SystemExit("--alpha mode must be opaque|mask|blend, got '%s'" % mode)
        alpha_modes.append((sub, mode))
    return set_textures, alpha_modes


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", required=True, help="input .glb")
    ap.add_argument("--out", dest="dst", help="output .glb (default: report only)")
    ap.add_argument("--in-place", action="store_true", help="overwrite the input")
    ap.add_argument("--exclude", action="append", default=None,
                    help="material-name substring to drop (repeatable)")
    ap.add_argument("--keep", action="append", default=None,
                    help="material-name substring to keep; drops every other name")
    ap.add_argument("--dedupe-bbox", action="store_true",
                    help="also drop primitives whose POSITION bounds match an "
                         "already-kept primitive (the stacked-layer signature)")
    ap.add_argument("--set-texture", action="append", default=None,
                    metavar="SUBSTR=IMAGE",
                    help="replace the diffuse texture of matching materials")
    ap.add_argument("--alpha", action="append", default=None,
                    metavar="SUBSTR=opaque|mask|blend",
                    help="force the alpha mode of matching materials")
    ap.add_argument("--dry-run", action="store_true", help="report without writing")
    args = ap.parse_args()

    if args.in_place and args.dst:
        raise SystemExit("--in-place and --out are mutually exclusive")
    dst = args.src if args.in_place else args.dst
    exclude = args.exclude if args.exclude is not None else SR_STATE_LAYERS

    gltf, bin_chunk = read_glb(args.src)
    prims_before = sum(len(m.get("primitives", [])) for m in gltf.get("meshes", []))
    kept, dropped, names, reasons = strip_primitives(gltf, exclude, args.keep,
                                                     args.dedupe_bbox)
    set_textures, alpha_modes = parse_overrides(args.set_texture, args.alpha)

    print("input : %s (%.1f MB)" % (args.src, os.path.getsize(args.src) / 1e6))
    print("rule  : %s" % ("keep " + ", ".join(args.keep) if args.keep else
                          "exclude " + ", ".join(exclude)))
    print("prims : %d -> %d   (%d dropped)" % (prims_before, kept, dropped))
    for name, count in names.most_common():
        print("   drop %-64s x%d" % (name, count))
    for name, reason in reasons:
        if reason != "state layer":
            print("   drop %-64s (%s)" % (name, reason))
    for sub, data, mime, _ in set_textures:
        matched = [n for n in {m.get("name", "").split("/")[-1]
                               for m in gltf.get("materials", [])} if sub in n]
        print("tex   : '%s' -> %d material(s), %s, %.0f KB: %s"
              % (sub, len(matched), mime, len(data) / 1e3, ", ".join(sorted(matched))))
    for sub, mode in alpha_modes:
        print("alpha : '%s' -> %s" % (sub, mode))

    if args.dry_run or dst is None:
        print("dry run: nothing written")
        return
    if kept == 0:
        raise SystemExit("refusing to write an empty mesh")

    new_bin = rebuild(gltf, bin_chunk, set_textures, alpha_modes)
    write_glb(dst, gltf, new_bin)
    print("output: %s (%.1f MB; bin %.1f -> %.1f MB)" %
          (dst, os.path.getsize(dst) / 1e6,
           len(bin_chunk) / 1e6, len(new_bin) / 1e6))
    print("tables: materials %d, textures %d, images %d, accessors %d, views %d" %
          (len(gltf.get("materials", [])), len(gltf.get("textures", [])),
           len(gltf.get("images", [])), len(gltf.get("accessors", [])),
           len(gltf.get("bufferViews", []))))


if __name__ == "__main__":
    sys.exit(main())
