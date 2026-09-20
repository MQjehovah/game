#!/usr/bin/env python3
"""Generate the MOBA project's prefabs from the converted LoL model index.

Reads ``projects/moba/assets/models/lol/index.json`` (written by
``tools/lolimport/lol_convert.py``) and emits:

* ``assets/prefabs/hero_<Name>.json``   one per numeric champion model
* ``assets/prefabs/unit_<key>.json``    one per monster / non-numeric model
* ``assets/prefabs/minion_*.json``      lane minions (monster models, tuned)
* ``assets/prefabs/struct_*.json``      procedural towers / nexus / inhibitor

Champion max HP / attack range come from ``moba_roster.json`` so the prefab's
reflected ``health`` component matches the gameplay stats.
"""

import json
import math
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "projects", "moba")
LOOT = os.path.join(ROOT, "assets", "models", "lol")
PREFABS = os.path.join(ROOT, "assets", "prefabs")

# name -> {hp, mana, scale} gameplay base; scale 0.01 puts LoL units (~180)
# at ~1.8 world metres.
ROSTER_PATH = os.path.join(ROOT, "assets", "data", "moba_roster.json")


def write_prefab(name, components):
    os.makedirs(PREFABS, exist_ok=True)
    path = os.path.join(PREFABS, name + ".json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"components": components}, fh, ensure_ascii=False, indent=4)
    return path


def hero_prefab(glb, scale, hp):
    return {
        "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1],
                      "scale": [scale, scale, scale]},
        "mesh": {"meshKey": "gltf:" + glb},
        "health": {"hp": hp, "maxHp": hp},
        "groups": {"groups": ["champion"]},
    }


def unit_prefab(glb, scale, hp):
    return {
        "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1],
                      "scale": [scale, scale, scale]},
        "mesh": {"meshKey": "gltf:" + glb},
        "health": {"hp": hp, "maxHp": hp},
    }


def struct_prefab(mesh_key, scale, color, hp, emissive=0.0):
    return {
        "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1],
                      "scale": list(scale)},
        "mesh": {
            "meshKey": mesh_key,
            "material": {"colorHex": color, "roughness": 0.55, "metallic": 0.2,
                         "emissiveIntensity": emissive, "ao": 1.0},
        },
        "health": {"hp": hp, "maxHp": hp},
    }


# --------------------------------------------------------------------------
# Procedural OBJ structures (towers / nexus) — flat-normal fallback means we
# only need v/f lines.
# --------------------------------------------------------------------------
def _prism(verts, faces, r0, r1, y0, y1, segs=8, cap_bottom=True, cap_top=True):
    base = len(verts) + 1
    for i in range(segs):
        a = 2 * 3.141592653589793 * i / segs
        verts.append((r0 * math.cos(a), y0, r0 * math.sin(a)))
    for i in range(segs):
        a = 2 * 3.141592653589793 * i / segs
        verts.append((r1 * math.cos(a), y1, r1 * math.sin(a)))
    for i in range(segs):
        j = (i + 1) % segs
        faces.append((base + i, base + j, base + segs + j, base + segs + i))
    if cap_top:
        top = len(verts) + 1
        verts.append((0.0, y1, 0.0))
        for i in range(segs):
            j = (i + 1) % segs
            faces.append((base + segs + i, base + segs + j, top))
    if cap_bottom:
        bot = len(verts) + 1
        verts.append((0.0, y0, 0.0))
        for i in range(segs):
            j = (i + 1) % segs
            faces.append((base + j, base + i, bot))


def _octa(verts, faces, cx, cy, cz, r, h):
    mid = len(verts) + 1
    for i in range(4):
        a = 2 * 3.141592653589793 * i / 4
        verts.append((cx + r * math.cos(a), cy, cz + r * math.sin(a)))
    top = len(verts) + 1
    verts.append((cx, cy + h, cz))
    bot = len(verts) + 1
    verts.append((cx, cy - h, cz))
    for i in range(4):
        j = (i + 1) % 4
        faces.append((mid + i, mid + j, top))
        faces.append((mid + j, mid + i, bot))


def write_obj(path, verts, faces):
    with open(path, "w", encoding="ascii") as fh:
        for v in verts:
            fh.write("v %.4f %.4f %.4f\n" % v)
        for f in faces:
            fh.write("f " + " ".join(str(i) for i in f) + "\n")
    return path


def make_structures(models_dir):
    os.makedirs(models_dir, exist_ok=True)
    verts, faces = [], []
    _prism(verts, faces, 1.7, 1.35, 0.0, 0.7)          # base
    _prism(verts, faces, 1.05, 0.85, 0.7, 4.4)          # shaft
    _prism(verts, faces, 1.5, 1.3, 4.4, 4.9)            # platform
    _octa(verts, faces, 0.0, 6.1, 0.0, 0.9, 1.3)        # crystal
    write_obj(os.path.join(models_dir, "tower.obj"), verts, faces)

    verts, faces = [], []
    _prism(verts, faces, 2.6, 2.2, 0.0, 0.6)            # dais
    _octa(verts, faces, 0.0, 2.6, 0.0, 1.8, 2.4)        # big crystal
    write_obj(os.path.join(models_dir, "nexus.obj"), verts, faces)
    return "assets/models/tower.obj", "assets/models/nexus.obj"


def main():
    index = json.load(open(os.path.join(LOOT, "index.json"), encoding="utf-8"))
    roster = {}
    if os.path.isfile(ROSTER_PATH):
        roster = json.load(open(ROSTER_PATH, encoding="utf-8"))

    made = []
    for key, entry in sorted(index.items()):
        glb = "assets/models/lol/" + entry["glb"]
        if key.isdigit():
            name = entry["name"]
            safe = re.sub(r"[^A-Za-z0-9_]", "", name) or ("champ" + key)
            stats = roster.get(safe) or roster.get(name, {})
            hp = stats.get("hp", 600)
            scale = stats.get("scale", 0.01)
            write_prefab("hero_" + safe, hero_prefab(glb, scale, hp))
            made.append("hero_" + safe)
        else:
            write_prefab("unit_" + key, unit_prefab(glb, 0.01, 300))
            made.append("unit_" + key)

    # Real Riot unit models converted by tools/lolimport/skn_to_gltf.py.
    def unit_gltf(glb, scale, hp):
        return {
            "transform": {"pos": [0, 0, 0], "rot": [0, 0, 0, 1],
                          "scale": [scale, scale, scale]},
            "mesh": {"meshKey": "gltf:" + glb},
            "health": {"hp": hp, "maxHp": hp},
        }

    units = {
        "minion_melee_blue": ("MinionMeleeBlue", 0.01, 450),
        "minion_melee_red": ("MinionMeleeRed", 0.01, 450),
        "minion_caster_blue": ("MinionCasterBlue", 0.01, 300),
        "minion_caster_red": ("MinionCasterRed", 0.01, 300),
        "minion_siege_blue": ("MinionSiegeBlue", 0.01, 800),
        "minion_siege_red": ("MinionSiegeRed", 0.01, 800),
    }
    for prefab, (model, scale, hp) in units.items():
        write_prefab(prefab, unit_gltf("assets/models/lol_units/%s.glb" % model, scale, hp))
        made.append(prefab)

    write_prefab("struct_tower_blue",
                 unit_gltf("assets/models/lol_units/TurretBlue.glb", 0.01, 1500))
    write_prefab("struct_tower_red",
                 unit_gltf("assets/models/lol_units/TurretRed.glb", 0.01, 1500))
    write_prefab("struct_nexus_blue",
                 unit_gltf("assets/models/lol_units/NexusBlue.glb", 0.012, 3000))
    write_prefab("struct_nexus_red",
                 unit_gltf("assets/models/lol_units/NexusRed.glb", 0.012, 3000))
    made += ["struct_tower_blue", "struct_tower_red",
             "struct_nexus_blue", "struct_nexus_red"]

    print("wrote %d prefabs to %s" % (len(made), PREFABS))


if __name__ == "__main__":
    main()
