# -*- coding: utf-8 -*-
"""Repairs mojibake entity names in the editor's local editor_scene.json.

An old tool/editor session wrote the scene through a non-UTF-8 code page, so
Chinese names arrived as Latin-1 lookalikes (e.g. "地面" became "\u00b0\u00a1").
The layout (pos/scale/tint/script/parent) is untouched; only names are
rebuilt from the same deterministic rules gen_realm_scene.py uses.

Run from the repo root:
    python tools/fix_editor_scene_names.py
"""

import json
import os
import re


def fix_name(name, mesh, pos, scale):
    """Return the correct Chinese name for a scene entity, or None to keep."""
    if mesh == "terrain":
        return "地面"
    if mesh == "water":
        return "湖泊"
    if mesh == "road":
        if pos[0] == 0.0 and pos[2] == 0.0:
            return "主干道" if scale[2] > scale[0] else "横街"
        return "小路_东" if pos[0] > 0 else "小路_西"
    if mesh == "house":
        if pos[0] == 4.6:
            return "农舍_东"
        if pos[0] == -4.6:
            return "农舍_西"
        return "旅店" if pos[0] == 7.2 else "铁匠铺"
    if mesh.startswith("gltf:"):
        return "展示头盔"
    if mesh == "cube":
        return "展示台"
    if mesh == "hero":
        return "英雄"
    if mesh == "wolf":
        return "狼_" + name.split("_")[-1]
    if mesh == "tree":
        return "松树_" + name.split("_")[-1]
    if mesh == "bush":
        return "灌木_" + name.split("_")[-1]
    if mesh == "rock":
        return "岩石_" + name.split("_")[-1]
    return None


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(repo, "editor_scene.json")
    with open(path, "rb") as f:
        data = f.read()
    doc = json.loads(data.decode("utf-8"))

    renamed = {}  # old name -> new name (for parent references)
    fixed = 0
    for i, e in enumerate(doc.get("entities", [])):
        old = e.get("name", "")
        new = fix_name(old, e.get("mesh", ""), e.get("pos", [0, 0, 0]),
                       e.get("scale", [1, 1, 1]))
        if new and new != old:
            renamed[old] = new
            e["name"] = new
            fixed += 1
    # Keep parent references in sync with the renamed entities.
    for e in doc.get("entities", []):
        parent = e.get("parent")
        if parent in renamed:
            e["parent"] = renamed[parent]

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    print("fixed {} entity names -> {}".format(fixed, path))


if __name__ == "__main__":
    main()
