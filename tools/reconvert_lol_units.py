#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Re-convert the cached LoL unit .skn files to glTF (with spike filtering)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/lolimport")
import skn_to_gltf  # noqa: E402

CACHE = r"F:\assets\sr\units"
PNG = r"F:\assets\sr\units_png"
OUT = r"E:\game\projects\moba\assets\models\lol_units"
NAMES = ["MinionMeleeBlue", "MinionMeleeRed", "MinionCasterBlue", "MinionCasterRed",
         "MinionSiegeBlue", "MinionSiegeRed", "TurretBlue", "TurretRed",
         "InhibitorBlue", "InhibitorRed", "NexusBlue", "NexusRed"]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in NAMES:
        skn = os.path.join(CACHE, name + ".skn")
        png = os.path.join(PNG, name + ".png")
        if not os.path.isfile(skn):
            print("missing", skn)
            continue
        try:
            skn_to_gltf.convert(skn, png if os.path.isfile(png) else "", os.path.join(OUT, name + ".glb"), name)
        except Exception as exc:  # noqa: BLE001
            print("ERR", name, exc)


if __name__ == "__main__":
    main()
