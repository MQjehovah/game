#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rename hash-named WAD extracts back to their real (repo) file names."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_wad import xxh64  # noqa: E402

TREE = r"F:\assets\fantome\tree.json"
VARIANT = "OldSummonersRiftV2"
EXTRACT = r"F:\assets\fantome\extract"
OUT = r"F:\assets\fantome\named"


def main():
    d = json.load(open(TREE, encoding="utf-8"))
    prefix = VARIANT + "/Map11/"
    table = {}
    for t in d["tree"]:
        if t["type"] != "blob":
            continue
        p = t["path"]
        if not p.startswith(prefix):
            continue
        rel = p[len(prefix):]
        table[xxh64(rel.lower().encode())] = rel
    print("repo files under %s: %d" % (VARIANT, len(table)))

    os.makedirs(OUT, exist_ok=True)
    named = missing = 0
    for fn in os.listdir(EXTRACT):
        m = re.match(r"^([0-9a-f]{16})", fn)
        if not m:
            continue
        h = int(m.group(1), 16)
        rel = table.get(h)
        if rel is None:
            missing += 1
            continue
        base = rel.replace("/", "__")
        ext = os.path.splitext(base)[1].lower()
        # TEX payloads were written as .png (with dims/format in the name).
        if ext == ".tex":
            base = os.path.splitext(base)[0] + ".png"
        src = os.path.join(EXTRACT, fn)
        dst = os.path.join(OUT, base)
        if os.path.exists(dst):
            dst = os.path.join(OUT, m.group(1) + "__" + base)
        os.replace(src, dst)
        named += 1
    print("renamed:", named, "unmatched:", missing)


if __name__ == "__main__":
    main()
