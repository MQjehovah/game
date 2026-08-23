# -*- coding: utf-8 -*-
"""Regenerates projects/neon_realm/scenes/realm.json.

The scene is pure data (112 entities): the village + scattered props + a
12-wolf pool, using the engine's procedural mesh keys. Run from the repo root:
    python tools/gen_realm_scene.py
"""

import json
import math
import os
import random

random.seed(20260823)


def ent(name, comps):
    return {"name": name, "components": comps}


def transform(pos, scale=(1, 1, 1)):
    return {"transform": {"pos": list(pos), "rot": [0, 0, 0, 1], "scale": list(scale)}}


def mesh(key, color="#FFFFFF", metallic=0.0, roughness=0.8):
    return {"mesh": {"meshKey": key, "colorHex": color, "metallic": metallic,
                     "roughness": roughness}}


def health(hp, maxhp):
    return {"health": {"hp": hp, "maxHp": maxhp}}


def script(path, vars_=None):
    s = {"backend": "lua", "path": path}
    if vars_ is not None:
        s["vars"] = vars_
    return {"script": s}


entities = []
entities.append(ent("地面", {**transform((0, 0, 0)), **mesh("terrain", "#FFFFFF", 0.0, 1.0)}))

for (x, z) in [(-24, -24), (24, -24), (-24, 24), (24, 24)]:
    entities.append(ent("湖泊", {**transform((x, -1.15, z), (1.05, 1, 1.05)),
                                 **mesh("water", "#2673D9", 0.0, 0.2)}))

entities.append(ent("主干道", {**transform((0, 0.03, 0), (2.8, 1, 17)), **mesh("road", "#70635B")}))
entities.append(ent("横街", {**transform((0, 0.03, 0), (15, 1, 2.8)), **mesh("road", "#665C54")}))
entities.append(ent("小路_东", {**transform((5.5, 0.03, -2.5), (2, 1, 8)), **mesh("road", "#61564D")}))
entities.append(ent("小路_西", {**transform((-5.5, 0.03, -2.5), (2, 1, 8)), **mesh("road", "#61564D")}))

for name, pos, s in [
    ("农舍_东", (4.6, 0, 3.2), 1.15),
    ("旅店", (7.2, 0, -2.2), 1.35),
    ("农舍_西", (-4.6, 0, 3.2), 1.15),
    ("铁匠铺", (-7.2, 0, -2.2), 1.2),
]:
    entities.append(ent(name, {**transform(pos, (s, s, s)), **mesh("house", "#FFFFFF")}))

for name, pos, key in [
    ("村民_商人", (1.8, 0, 1.2), "npc:199,71,46"),
    ("村民_农夫", (-1.8, 0, 1.4), "npc:77,140,199"),
    ("村民_猎人", (0.6, 0, -1.6), "npc:122,107,51"),
    ("村民_法师", (3.0, 0, -0.8), "npc:153,92,184"),
    ("村民_卫兵", (-3.0, 0, -0.8), "npc:140,140,148"),
]:
    entities.append(ent(name, {**transform(pos, (1.05, 1.05, 1.05)), **mesh(key)}))

entities.append(ent("村长", {**transform((0, 0, 0), (1.05, 1.05, 1.05)),
                             **mesh("npc:217,153,89")}))
entities.append(ent("展示头盔",
                     {**transform((0, 0.95, 2.6)),
                      **mesh("gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf")}))
entities.append(ent("展示台", {**transform((0, 0.45, 2.6), (1.4, 0.9, 1.4)),
                                **mesh("cube", "#8C6B4D")}))


def scatter(mesh_key, label, count, rmin, rmax, smin, smax, spacing, color="#FFFFFF"):
    placed = []
    out = []
    for _ in range(count):
        for _ in range(64):
            a = random.uniform(0, 2 * math.pi)
            r = random.uniform(rmin, rmax)
            x = math.cos(a) * r
            z = math.sin(a) * r
            if math.hypot(x, z) < 26:
                continue
            if any(math.hypot(x - px, z - pz) < spacing for px, pz in placed):
                continue
            s = random.uniform(smin, smax)
            placed.append((x, z))
            out.append((x, z, s))
            break
    for i, (x, z, s) in enumerate(out):
        entities.append(ent("{}_{}".format(label, i),
                            {**transform((x, 0, z), (s, s, s)), **mesh(mesh_key, color)}))


scatter("tree", "松树", 40, 26, 96, 1.0, 1.8, 3.2)
scatter("bush", "灌木", 24, 26, 96, 0.8, 1.3, 3.0)
scatter("rock", "岩石", 14, 26, 96, 0.6, 1.4, 2.6)

entities.append(ent("英雄", {
    **transform((0, 0.9, 4)),
    **mesh("hero", "#33B2FF"),
    **health(100, 100),
    **script("scripts/realm.lua"),
}))


def scatter_wolves(count):
    placed = []
    out = []
    for _ in range(count):
        for _ in range(128):
            a = random.uniform(0, 2 * math.pi)
            r = random.uniform(22, 62)
            x = math.cos(a) * r
            z = math.sin(a) * r
            if any(math.hypot(x - px, z - pz) < 9 for px, pz in placed):
                continue
            placed.append((x, z))
            out.append((x, z))
            break
    return out


for i, (x, z) in enumerate(scatter_wolves(12)):
    entities.append(ent("狼_{}".format(i + 1), {
        **transform((x, 0.55, z)),
        **mesh("wolf", "#806052"),
        **health(45, 45),
    }))

scene = {"entities": entities}
out_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "projects", "neon_realm", "scenes", "realm.json")
with open(out_path, "w", encoding="utf-8", newline="\n") as f:
    json.dump(scene, f, ensure_ascii=False, indent=2)
print("wrote {} entities -> {}".format(len(entities), out_path))
