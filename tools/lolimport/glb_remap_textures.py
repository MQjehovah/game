#!/usr/bin/env python3
"""Full texture remap for a mapgeo-derived .glb, driven by the game's own data.

mapgeo_fbx_to_gltf.py picks each material's diffuse texture by scoring
material-name tokens against a texture directory. That heuristic is right most
of the time, but it silently picks the wrong picture for a number of materials:
Summoner's Rift's brush layer (VertexDeform_inst1, a 455k-triangle mesh) landed
on a terrain splat bake, 24 lambert* materials shared one 34 KB picture and the
big terrain layers all shared a single 581 KB image.

The authoritative answer sits in the game's own map package: base_srx.materials.bin
stores a DiffuseTexture property (legacy spelling Diffuse_Texture) for every
material, and those material paths match the .glb's material names exactly.

  extract  read the material -> texture table out of the .bin
  apply    rebind every material to its own picture and repack the .glb

Example:

  python tools/lolimport/glb_remap_textures.py extract \
      --materials-bin F:/assets/fantome/named/DATA__Maps__mapgeometry__map11__base_srx.materials.bin \
      --glb projects/moba/assets/models/sr/sr_map.glb \
      --out build-msvc/sr_tex_map.json

  python tools/lolimport/glb_remap_textures.py apply \
      --in projects/moba/assets/models/sr/sr_map.glb \
      --map build-msvc/sr_tex_map.json \
      --named-dir F:/assets/fantome/named \
      --out build-msvc/sr_map_remap.glb

apply never touches geometry: it only rewrites the material / texture / image
tables and appends the replacement pictures, so the vertex bytes stay identical.

Two traps this tool exists to avoid, both learned the hard way:

* The flat asset dump is not uniform: the same picture is often present as a
  faithful .dds and as a .png that decodes to noise. .dds therefore wins, and
  every source is checked with a noise guard before it is allowed into the .glb.
* A .tex reference does not always sit in the directory the dump exposes, so a
  leaf-name lookup has to back up the exact path (SRX/CustomMap/wallofgrass.tex
  is really Map11/textures/wallofgrass.dds).
"""

import argparse
import collections
import json
import os
import re
import struct
import sys

from glb_strip_layers import embed_texture, read_glb, rebuild, write_glb

# Material names sit in the .bin property tree as
#   <tag> <uint16 length> <name>
# The tag is what tells a real material entry apart from the same string showing
# up in the file's string pool, where there is no tree position to read a block
# from and no DiffuseTexture to find.
MATERIAL_TAG = bytes.fromhex("bd398d10")
PRINTABLE = frozenset(range(0x20, 0x7F))
DIFFUSE_KEY = re.compile(rb"Diffuse_?Texture")
ASSET_DIRS = (b"ASSETS/", b"assets/")
PREFERRED_EXT = ".dds"
FALLBACK_EXTS = (".ktx", ".tga", ".png", ".jpg", ".jpeg")
# Real artwork never has wildly unrelated neighbouring pixels; misread texture
# data does. 60 is far above any map texture measured here (the noisiest real
# one sits near 20 after a 256x256 normalisation) and far below the broken
# .png twins (100+).
NOISE_LIMIT = 60.0


def material_names(glb_path):
    """Material names, in file order (the .glb names are mapgeo paths)."""
    gltf, _ = read_glb(glb_path)
    return [m.get("name", "") for m in gltf.get("materials", [])]


def read_printable(buf, off):
    end = off
    while end < len(buf) and buf[end] in PRINTABLE:
        end += 1
    return buf[off:end].decode("ascii", "replace")


def extract_table(bin_path, glb_path):
    """{material name: ASSETS/... path} plus per-material problems."""
    with open(bin_path, "rb") as fh:
        buf = fh.read()
    names = material_names(glb_path)

    anchors, fallbacks = [], []
    for name in names:
        raw = name.encode("ascii")
        at = buf.find(MATERIAL_TAG + struct.pack("<H", len(raw)) + raw)
        if at >= 0:
            anchors.append((at + len(MATERIAL_TAG) + 2, name))
        else:
            # No tagged entry: fall back to the bare string so the tool reports
            # something useful instead of silently skipping the material.
            anchors.append((buf.find(raw), name))
            fallbacks.append(name)
    anchors.sort()
    starts = [at for at, _ in anchors]

    table, problems = {}, []
    for i, (start, name) in enumerate(anchors):
        if start < 0:
            problems.append((name, "material not found in the .bin"))
            continue
        end = starts[i + 1] if i + 1 < len(starts) else len(buf)
        block = buf[start:end]
        key = DIFFUSE_KEY.search(block)
        if not key:
            problems.append((name, "no DiffuseTexture / Diffuse_Texture property"))
            continue
        at = -1
        for d in ASSET_DIRS:
            at = block.find(d, key.end())
            if at >= 0:
                break
        if at < 0:
            problems.append((name, "no ASSETS path after the diffuse key"))
            continue
        table[name] = read_printable(block, at)
    return table, problems, fallbacks


def index_named_dir(named_dir):
    """Indexes over the flat WAD dump: full flat name and leaf file name."""
    by_flat, by_leaf = {}, {}
    with os.scandir(named_dir) as it:
        for entry in it:
            if not entry.is_file():
                continue
            by_flat.setdefault(entry.name.lower(), entry.name)
            leaf = os.path.splitext(entry.name.split("__")[-1])[0].lower()
            by_leaf.setdefault(leaf, []).append(entry.name)
    return by_flat, by_leaf


def resolve_source(by_flat, by_leaf, named_dir, asset_path):
    """Local file for a game asset path; returns (path, how it was found).

    Order matters: an exact path hit on a .png must not pre-empt a leaf-name hit
    on a .dds, because for this asset set the .png twins are the broken ones.
    """
    stem = os.path.splitext(asset_path.replace("/", "__"))[0].lower()
    leaf = os.path.splitext(asset_path.split("/")[-1])[0].lower()

    hit = by_flat.get(stem + PREFERRED_EXT)
    if hit:
        return os.path.join(named_dir, hit), "path/" + PREFERRED_EXT
    twin = sorted(f for f in by_leaf.get(leaf, [])
                  if f.lower().endswith(PREFERRED_EXT))
    if twin:
        return os.path.join(named_dir, twin[0]), "leaf/" + PREFERRED_EXT
    for ext in FALLBACK_EXTS:
        hit = by_flat.get(stem + ext)
        if hit:
            return os.path.join(named_dir, hit), "path/" + ext
    for ext in FALLBACK_EXTS:
        twin = sorted(f for f in by_leaf.get(leaf, []) if f.lower().endswith(ext))
        if twin:
            return os.path.join(named_dir, twin[0]), "leaf/" + ext
    return None, "missing"


def noise_score(img):
    """Rough 'is this really a picture' metric on a normalised 256x256 copy."""
    import numpy as np

    small = np.asarray(img.convert("RGB").resize((256, 256)), dtype=np.float32)
    return float(np.abs(small[:, 1:] - small[:, :-1]).mean()
                 + np.abs(small[1:] - small[:-1]).mean())


def current_diffuse_bytes(gltf):
    """material index -> byte length of the picture it points at right now."""
    out = {}
    textures = gltf.get("textures", [])
    images = gltf.get("images", [])
    views = gltf.get("bufferViews", [])
    for i, mat in enumerate(gltf.get("materials", [])):
        ref = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture", {})
        ti = ref.get("index")
        if ti is None or not (0 <= ti < len(textures)):
            continue
        src = textures[ti].get("source")
        if src is None or not (0 <= src < len(images)):
            continue
        bv = images[src].get("bufferView")
        if bv is not None and 0 <= bv < len(views):
            out[i] = views[bv]["byteLength"]
    return out


def apply_remap(args):
    from PIL import Image
    import numpy as np

    gltf, bin_chunk = read_glb(args.src)
    with open(args.map, encoding="utf-8") as fh:
        table = json.load(fh)
    by_flat, by_leaf = index_named_dir(args.named_dir)
    old_bytes = current_diffuse_bytes(gltf)

    exact, rows, missing, noisy = {}, [], [], []
    for i, mat in enumerate(gltf.get("materials", [])):
        name = mat.get("name", "")
        leaf = name.split("/")[-1]
        asset = table.get(name)
        if not asset:
            rows.append((leaf, "-", "-", "no entry in the table (left as is)"))
            continue
        path, how = resolve_source(by_flat, by_leaf, args.named_dir, asset)
        if path is None:
            missing.append((name, asset))
            rows.append((leaf, asset.split("/")[-1], "-", "SOURCE FILE MISSING"))
            continue

        img = Image.open(path)
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        if img.width > args.max_size or img.height > args.max_size:
            img.thumbnail((args.max_size, args.max_size), Image.LANCZOS)
        alpha = np.asarray(img)[..., 3]
        cut = float((alpha < 250).mean()) * 100.0
        noise = noise_score(img)
        if noise > args.max_noise:
            noisy.append((name, path, noise))
            rows.append((leaf, asset.split("/")[-1], "-",
                         "NOISY SOURCE (%.0f) - refused" % noise))
            continue

        data, mime = embed_texture(img, args.max_size, args.quality)
        # Cut-out vs opaque is a property of the PICTURE: alpha in it means a
        # hard cut-out, none means the surface is opaque. How the surface meets
        # what is behind it is a property of the MATERIAL, and BLEND is the one
        # mode that cannot be re-derived - the Rift paints its lane paving, moss,
        # grass tufts, flowers, chasm and base seams as alpha-BLENDED ground
        # decals, and masking those would cut the soft gradients into strips.
        if str(mat.get("alphaMode", "")).upper() == "BLEND":
            mode = "BLEND"
        else:
            mode = "OPAQUE" if mime == "image/jpeg" else "MASK"
        exact[name] = (data, mime, mode)
        old = old_bytes.get(i)
        delta = "new" if old is None else ("same" if old == len(data) else "CHANGED")
        rows.append((leaf, asset.split("/")[-1],
                     "%dx%d" % (img.width, img.height),
                     "%s %-8s alpha<250 %5.1f%% noise %4.1f -> %-6s %6.0f KB [%s]"
                     % (delta, how, cut, noise, mode, len(data) / 1e3,
                        mime.split("/")[-1])))

    changed = sum(1 for r in rows if r[3].startswith(("CHANGED", "new")))
    print("input : %s (%.1f MB)" % (args.src, os.path.getsize(args.src) / 1e6))
    print("table : %d material(s) with a DiffuseTexture" % len(table))
    print("bound : %d material(s); %d pick a different picture than before"
          % (len(exact), changed))
    if args.verbose:
        for leaf, tex, dims, note in sorted(rows):
            print("   %-48s %-38s %-9s %s" % (leaf[:48], tex[:38], dims, note))
    for label, bad in (("missing source file", missing), ("noisy source", noisy)):
        if bad:
            print("%s: %d" % (label, len(bad)))
            for row in bad:
                print("   %s" % (row,))
    if args.dry_run:
        print("dry run: nothing written")
        return
    if missing or noisy:
        raise SystemExit("refusing to write: %d unresolved, %d noisy"
                         % (len(missing), len(noisy)))

    new_bin = rebuild(gltf, bin_chunk, exact_textures=exact)

    # Normalise the legacy alphaCutoffFactor spelling on the way out: glTF calls
    # this field alphaCutoff, and only this repo's own importers wrote the other.
    # A cutoff left on an opaque material is dead weight (the engine only reads
    # it for MASK/BLEND), so drop it.
    for mat in gltf.get("materials", []):
        if "alphaCutoffFactor" in mat:
            mat.setdefault("alphaCutoff", mat.pop("alphaCutoffFactor"))
        if mat.get("alphaMode", "OPAQUE") == "OPAQUE":
            mat.pop("alphaCutoff", None)

    write_glb(args.dst, gltf, new_bin)
    imgs = gltf.get("images", [])
    views = gltf.get("bufferViews", [])
    total = sum(views[im["bufferView"]]["byteLength"] for im in imgs
                if im.get("bufferView") is not None)
    modes = collections.Counter(m.get("alphaMode", "OPAQUE")
                                for m in gltf.get("materials", []))
    print("output: %s (%.1f MB; bin %.1f -> %.1f MB)"
          % (args.dst, os.path.getsize(args.dst) / 1e6,
             len(bin_chunk) / 1e6, len(new_bin) / 1e6))
    print("tables: materials %d, textures %d, images %d (%.1f MB of pictures)"
          % (len(gltf.get("materials", [])), len(gltf.get("textures", [])),
             len(imgs), total / 1e6))
    print("alpha : %s" % dict(modes))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    ex = sub.add_parser("extract", help="read material -> diffuse texture from the .bin")
    ex.add_argument("--materials-bin", required=True, help="base_srx.materials.bin")
    ex.add_argument("--glb", required=True, help="the .glb whose materials to look up")
    ex.add_argument("--out", required=True, help="where to write the JSON table")
    ex.add_argument("--quiet", action="store_true")

    ap2 = sub.add_parser("apply", help="rebind every material and repack the .glb")
    ap2.add_argument("--in", dest="src", required=True, help="input .glb")
    ap2.add_argument("--map", required=True, help="JSON table from extract")
    ap2.add_argument("--named-dir", required=True,
                     help="flat WAD-unpacked asset dump (files named a__b__c.dds)")
    ap2.add_argument("--out", dest="dst", required=True, help="output .glb")
    ap2.add_argument("--max-size", type=int, default=1024,
                     help="downscale sources larger than this (default 1024)")
    ap2.add_argument("--quality", type=int, default=85,
                     help="JPEG quality for opaque pictures (default 85)")
    ap2.add_argument("--max-noise", type=float, default=NOISE_LIMIT,
                     help="reject a source whose neighbouring-pixel noise "
                          "exceeds this (default %.0f)" % NOISE_LIMIT)
    ap2.add_argument("--verbose", action="store_true", help="list every material")
    ap2.add_argument("--dry-run", action="store_true", help="report without writing")

    args = ap.parse_args()
    if args.cmd == "extract":
        table, problems, fallbacks = extract_table(args.materials_bin, args.glb)
        if not args.quiet:
            print("materials in .glb : %d" % len(material_names(args.glb)))
            print("with a DiffuseTexture: %d" % len(table))
            if fallbacks:
                print("string-pool fallback for %d material(s): %s"
                      % (len(fallbacks), ", ".join(fallbacks)))
            for name, why in problems:
                print("   UNRESOLVED %s: %s" % (why, name))
        if problems:
            raise SystemExit("refusing to write an incomplete table (%d problems)"
                             % len(problems))
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(table, fh, ensure_ascii=False, indent=1, sort_keys=True)
        return 0

    apply_remap(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
