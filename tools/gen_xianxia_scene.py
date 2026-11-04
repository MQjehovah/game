#!/usr/bin/env python3
"""Generate the 'immortal path' vertical-slice scene.

A narrow cliff ledge above a sea of cloud: rock wall on the right, a sharp
drop on the left, pines/ferns/red shrubs along the path, stone lanterns and a
distant gate. All geometry is procedural placeholder art.

Run from the repository root:

    python tools/gen_xianxia_scene.py
"""
import json
import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects" / "xianxia" / "assets" / "scenes" / "immortal_path.json"
rng = random.Random(23)

SEGMENTS = 64
SIZE = 160.0
HEIGHT = 8.0


def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def height(x, z):
    """Raw heightfield: a flat ledge near x=0, cliff up on the right, drop left."""
    shelf = (0.10 + 0.006 * math.sin(z * 0.16) +
             0.004 * math.sin(x * 1.3 + z * 0.7))
    right = smoothstep(6.0, 13.0, x) * (
        2.25 + 0.20 * math.sin(z * 0.09) + 0.10 * math.sin((x + z) * 0.33)
    )
    left = smoothstep(-2.5, -16.0, x) * (
        2.45 + 0.24 * math.sin(z * 0.11 + 0.5) + 0.09 * math.sin((x - z) * 0.39)
    )
    return shelf + right - left


def world_y(x, z):
    return height(x, z) * HEIGHT


def heights():
    n = SEGMENTS + 1
    half = SIZE * 0.5
    cell = SIZE / SEGMENTS
    return [
        round(height(-half + c * cell, -half + r * cell), 5)
        for r in range(n)
        for c in range(n)
    ]


def material(tex="", color_hex="#ffffff", roughness=0.9, metallic=0.0,
             uv_repeat=1.0, normal="", dirt="", rock="", emissive=1.0):
    m = {
        "colorHex": color_hex,
        "roughness": roughness,
        "metallic": metallic,
        "ao": 1.0,
        "emissiveIntensity": emissive,
    }
    if tex:
        m["albedoTex"] = tex
        m["uvRepeat"] = uv_repeat
    if normal:
        m["normalTex"] = normal
        m["normalScale"] = 0.8
    if dirt:
        m["dirtColorHex"] = dirt
    if rock:
        m["rockColorHex"] = rock
    return m


def transform(pos, rot_y=0.0, scale=(1.0, 1.0, 1.0)):
    return {
        "pos": [round(float(v), 3) for v in pos],
        "rot": [0.0, round(math.sin(rot_y * 0.5), 5), 0.0,
                round(math.cos(rot_y * 0.5), 5)],
        "scale": [round(float(v), 3) for v in scale],
    }


def mesh(mesh_key, mat, cast_shadow=True, receive_shadow=True):
    return {
        "meshKey": mesh_key,
        "material": mat,
        "castShadow": cast_shadow,
        "receiveShadow": receive_shadow,
    }


def entity(name, components):
    return {"name": name, "components": components}


STONE = "assets/textures/stone_proc.png"
GRASS = "assets/textures/grass_proc.png"

scene = {
    "renderstack": {
        "ssao": True,
        "ssaoIntensity": 0.7,
        "volumetric": True,
        "volumetricStrength": 0.90,
        "bloom": True,
        "bloomThreshold": 1.0,
        "bloomStrength": 0.40,
        "bloomWidth": 1.9,
        "tonemap": True,
        "exposure": 0.94,
        "grade": True,
        "gradeSaturation": 1.02,
        "gradeContrast": 0.34,
        "gradeGain": 1.0,
        "gradeTint": [1.07, 1.0, 0.93, 1.0],
        "autoExposure": False,
        "autoExposureKey": 0.20,
        "vignette": True,
        "vignetteRadius": 0.60,
        "vignetteIntensity": 0.40,
        "shadowDistance": 120.0,
        "shadowSoftness": 1.6,
    },
    "entities": [],
}

scene["entities"].append(entity("sun_light", {
    "transform": transform((0.0, 30.0, 0.0)),
    "light": {
        "type": "directional",
        "color": [1.0, 0.82, 0.62, 1.0],
        "sunDir": [-0.55, -0.48, -0.35],
        "intensity": 1.55,
        "radius": 10.0,
        "ambientStrength": 0.55,
        "useAtmosphere": False,
    },
    "environment": {
        "ambientColor": [0.42, 0.52, 0.68, 1.0],
        "ambientStrength": 0.55,
        "skyTop": [0.55, 0.68, 0.86, 1.0],
        "skyHorizon": [0.92, 0.86, 0.76, 1.0],
        "fogColor": [0.84, 0.78, 0.72, 1.0],
        "fogNear": 7.0,
        "fogFar": 68.0,
        "useAtmosphere": True,
        "skyTexture": "assets/sky/dusk_cloud_sea.jpg",
        "skybox": False,
    },
    "type": {"value": "Light3D"},
}))

scene["entities"].append(entity("ledge", {
    "transform": transform((0.0, 0.0, 0.0)),
    "mesh": mesh("terrain", material(
        tex="assets/textures/forest_ground/forest_floor_dark.jpg",
        normal="assets/textures/forest_ground/forest_leaves_nor.jpg",
        color_hex="#98ab7e",
        roughness=0.98,
        uv_repeat=28.0,
        dirt="#665845",
        rock="#56585a",
    ), cast_shadow=False),
    "terrain": {
        "segments": SEGMENTS,
        "size": SIZE,
        "heightScale": HEIGHT,
        "heights": heights(),
        "chunkGridDiv": 4,
        "chunkLodLevels": 2,
        "chunkBaseSubdiv": 16,
        "vegMeshKey": "grass",
        "vegCount": 42000,
        "vegSeed": 17,
        "vegSize": 0.62,
        "vegImpostorDistance": 70.0,
        "vegMinHeight": -4.0,
        "vegMaxHeight": 11.0,
        "vegMaxSlope": 0.40,
        "vegExcludeRadius": 3.0,
        "vegExcludeCenter": [0.0, 0.0, 0.0],
    },
    "type": {"value": "MeshInstance3D"},
}))

# One continuous stone path follows the flat ledge; the terrain shelf is nearly
# constant on the centreline, so no slab gaps.
scene["entities"].append(entity("path", {
    "transform": transform((0.0, world_y(0.0, 0.0) + 0.01, 0.0), rot_y=0.0,
                           scale=(1.0, 1.0, 1.0)),
    "mesh": mesh("stone_path", material(tex="assets/textures/cobblestone/diff_gray.jpg",
                                       normal="assets/textures/cobblestone/nor.jpg",
                                       color_hex="#b8bfbc", roughness=0.94, uv_repeat=1.0)),
    "type": {"value": "MeshInstance3D"},
}))

# Deterministic pines: a dense band on the high right wall and a few silhouettes
# on the left rim.
# Rock outcrops on the right wall break up the smooth grass slope.
for i, (x, z, sx, sy, sz) in enumerate([(8.9, -11.0, 3.2, 2.4, 2.2),
                                        (10.2, -23.0, 4.0, 2.8, 2.6),
                                        (9.0, -32.0, 2.6, 2.0, 2.0),
                                        (11.6, -42.0, 3.4, 2.2, 2.4)]):
    scene["entities"].append(entity(f"wall_rock_{i}", {
        "transform": transform((x, world_y(x, z) + sy * 0.35, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(sx, sy, sz)),
        "mesh": mesh("rock", material(color_hex="#6f7268", roughness=0.98)),
        "type": {"value": "MeshInstance3D"},
    }))

# Broken rock fall along the cliff foot: large faceted boulders read as a real
# talus slope and hide the smooth terrain band where the wall meets the path.
for i in range(14):
    z = -6.0 - i * 2.9 + rng.uniform(-0.8, 0.8)
    x = rng.uniform(4.4, 6.4)
    s = rng.uniform(0.9, 1.8)
    y = world_y(x, z)
    scene["entities"].append(entity(f"talus_rock_{i}", {
        "transform": transform((x, y - 0.1, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(s * 1.35, s * 0.85, s * 1.15)),
        "mesh": mesh("rock", material(color_hex="#6a6d66", roughness=0.98)),
        "type": {"value": "MeshInstance3D"},
    }))

# Craggy textured rock face over the right slope: the splat terrain alone reads
# as a smooth striped wall, while this displaced grid gives real rock relief.



# Faceted low-poly rock face laid over the right slope; matches the boulders
# and avoids the smooth, striped look of the raw terrain splat.
scene["entities"].append(entity("cliff_face", {
    "transform": transform((0.0, 0.0, 0.0)),
    "mesh": mesh("cliff_face", material(color_hex="#626862", roughness=0.94), cast_shadow=False),
    "type": {"value": "MeshInstance3D"},
}))

# Cloud sea as overlapping vertical billboards down the left drop. Vertical
# puffs keep their shape at grazing angles, unlike a horizontal texture plane.
for i, (x, y, z, w, hgt) in enumerate([
    (-8.0, 1.0, -12.0, 12.0, 8.0),
    (-13.0, 1.4, -19.0, 18.0, 11.0),
    (-18.0, 2.2, -26.0, 24.0, 14.0),
    (-9.0, 2.6, -31.0, 16.0, 12.0),
    (-25.0, 0.2, -37.0, 30.0, 17.0),
    (-15.0, 3.2, -43.0, 22.0, 14.0),
    (-31.0, 2.0, -49.0, 34.0, 19.0),
    (-10.0, 0.6, -38.0, 13.0, 9.0),
    (-6.0, -1.5, -8.0, 14.0, 8.0),
    (-12.0, -2.2, -14.0, 18.0, 10.0),
    (-20.0, -3.0, -20.0, 24.0, 12.0),
    (-9.0, -3.2, -23.0, 16.0, 10.0),
    (-30.0, -4.0, -29.0, 30.0, 16.0),
]):
    scene["entities"].append(entity(f"cloud_bank_{i}", {
        "transform": transform((x, y, z), scale=(w, hgt, 1.0)),
        "mesh": mesh("cloud_bank", material(color_hex="#eef5ff", roughness=1.0),
                     cast_shadow=False),
        "type": {"value": "MeshInstance3D"},
    }))
# Cooler, farther cloud masses give the sea depth behind the near banks.
for i, (x, y, z, w, hgt) in enumerate([
    (-22.0, -2.0, -38.0, 30.0, 16.0),
    (-34.0, -1.0, -52.0, 44.0, 22.0),
    (-18.0, 1.0, -46.0, 26.0, 14.0),
    (-40.0, -4.0, -60.0, 50.0, 26.0),
]):
    scene["entities"].append(entity(f"cloud_far_{i}", {
        "transform": transform((x, y, z), scale=(w, hgt, 1.0)),
        "mesh": mesh("cloud_far", material(color_hex="#b9c9df", roughness=1.0),
                     cast_shadow=False),
        "type": {"value": "MeshInstance3D"},
    }))

# Stone lanterns with warm point lights.
for i, (x, z) in enumerate([(3.6, -17.5), (-5.0, -25.0), (4.8, -31.0)]):
    y = world_y(x, z)
    scene["entities"].append(entity(f"lantern_{i}", {
        "transform": transform((x, y, z), rot_y=0.15 * i),
        "mesh": mesh("stone_lantern", material(color_hex="#c0c0b5", roughness=0.9)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"lantern_light_{i}", {
        "transform": transform((x, y + 1.55, z)),
        "light": {
            "type": "point",
            "color": [1.0, 0.60, 0.26, 1.0],
            "intensity": 4.0,
            "radius": 18.0,
        },
        "type": {"value": "Light3D"},
    }))

# Cool fill lights make the shadowed cliff faces readable instead of black.
for i, (x, y, z) in enumerate([(6.0, 9.0, -26.0), (5.0, 4.0, -40.0),
                                (-10.0, 3.0, -18.0)]):
    scene["entities"].append(entity(f"cliff_fill_{i}", {
        "transform": transform((x, y, z)),
        "light": {
            "type": "point",
            "color": [0.58, 0.70, 0.88, 1.0],
            "intensity": 2.6,
            "radius": 24.0,
        },
        "type": {"value": "Light3D"},
    }))

# Stone tablet and gate.
tablet_z = -32.0
tablet_y = world_y(0.0, tablet_z)
scene["entities"].append(entity("dao_tablet", {
    "transform": transform((0.0, tablet_y + 1.0, tablet_z), rot_y=0.0,
                           scale=(1.25, 2.0, 0.36)),
    "mesh": mesh("cube", material(tex=STONE, normal="assets/textures/stone_nor.png", color_hex="#5d605c",
                                  roughness=0.92, uv_repeat=1.0)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("tablet_cap", {
    "transform": transform((0.0, tablet_y + 2.18, tablet_z), rot_y=0.0,
                           scale=(1.55, 0.22, 0.62)),
    "mesh": mesh("cube", material(tex=STONE, normal="assets/textures/stone_nor.png", color_hex="#646761",
                                  roughness=0.9, uv_repeat=1.0)),
    "type": {"value": "MeshInstance3D"},
}))

gate_z = -39.0
gate_y = world_y(0.0, gate_z)
# Three worn steps lead up to the gate.
for i in range(3):
    sz = -36.0 - i * 0.62
    scene["entities"].append(entity(f"gate_step_{i}", {
        "transform": transform((0.0, world_y(0.0, sz) + 0.10 + i * 0.16, sz),
                               rot_y=0.0, scale=(5.6 - i * 0.35, 0.24, 0.72)),
        "mesh": mesh("cube", material(tex=STONE,
                                      normal="assets/textures/stone_nor.png",
                                      color_hex="#a5a397", roughness=0.97,
                                      uv_repeat=2.0)),
        "type": {"value": "MeshInstance3D"},
    }))
# Red-lacquered posts on stone plinths, gold caps and a tiled hip roof: the
# gate is the visual anchor at the end of the path.
for i, x in enumerate((-3.5, 3.5)):
    scene["entities"].append(entity(f"gate_plinth_{i}", {
        "transform": transform((x, gate_y + 0.24, gate_z), rot_y=0.0,
                               scale=(1.05, 0.48, 1.05)),
        "mesh": mesh("cube", material(tex=STONE,
                                      normal="assets/textures/stone_nor.png",
                                      color_hex="#8f8e83", roughness=0.97,
                                      uv_repeat=2.0)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"gate_pillar_{i}", {
        "transform": transform((x, gate_y + 2.30, gate_z), rot_y=0.0,
                               scale=(0.62, 4.10, 0.62)),
        "mesh": mesh("cube", material(color_hex="#7c2f24", roughness=0.76)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"gate_cap_{i}", {
        "transform": transform((x, gate_y + 4.38, gate_z), rot_y=0.0,
                               scale=(0.82, 0.18, 0.82)),
        "mesh": mesh("cube", material(color_hex="#c7a45a", roughness=0.38,
                                      metallic=0.4)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"gate_banner_{i}", {
        "transform": transform((x, gate_y + 3.08, gate_z + 0.38), rot_y=0.0,
                               scale=(0.52, 2.45, 0.05)),
        "mesh": mesh("cube", material(color_hex="#9e3527", roughness=0.86)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"gate_banner_trim_{i}", {
        "transform": transform((x, gate_y + 1.90, gate_z + 0.41), rot_y=0.0,
                               scale=(0.56, 0.09, 0.06)),
        "mesh": mesh("cube", material(color_hex="#d4ae63", roughness=0.42,
                                      metallic=0.45)),
        "type": {"value": "MeshInstance3D"},
    }))
scene["entities"].append(entity("gate_lintel", {
    "transform": transform((0.0, gate_y + 4.28, gate_z), rot_y=0.0,
                           scale=(8.2, 0.50, 0.92)),
    "mesh": mesh("cube", material(color_hex="#64241d", roughness=0.80)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_lintel_gold", {
    "transform": transform((0.0, gate_y + 4.57, gate_z), rot_y=0.0,
                           scale=(8.28, 0.08, 0.96)),
    "mesh": mesh("cube", material(color_hex="#c9a45c", roughness=0.38,
                                  metallic=0.45)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_roof", {
    "transform": transform((0.0, gate_y + 4.78, gate_z), rot_y=0.0,
                           scale=(9.7, 0.28, 1.75)),
    "mesh": mesh("cube", material(color_hex="#26383a", roughness=0.86)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_roof_top", {
    "transform": transform((0.0, gate_y + 5.06, gate_z), rot_y=0.0,
                           scale=(8.9, 0.22, 1.35)),
    "mesh": mesh("cube", material(color_hex="#1b2b2d", roughness=0.84)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_ridge_gold", {
    "transform": transform((0.0, gate_y + 5.21, gate_z), rot_y=0.0,
                           scale=(9.0, 0.07, 0.14)),
    "mesh": mesh("cube", material(color_hex="#c9a45c", roughness=0.36,
                                  metallic=0.5)),
    "type": {"value": "MeshInstance3D"},
}))
for i, x in enumerate((-4.85, 4.85)):
    scene["entities"].append(entity(f"gate_eave_tip_{i}", {
        "transform": transform((x, gate_y + 4.92, gate_z), rot_y=0.0,
                               scale=(0.38, 0.16, 0.82)),
        "mesh": mesh("cube", material(color_hex="#1b2b2d", roughness=0.84)),
        "type": {"value": "MeshInstance3D"},
    }))
scene["entities"].append(entity("gate_plaque", {
    "transform": transform((0.0, gate_y + 3.66, gate_z + 0.50), rot_y=0.0,
                           scale=(2.8, 0.72, 0.12)),
    "mesh": mesh("cube", material(color_hex="#241c16", roughness=0.58)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_plaque_gold", {
    "transform": transform((0.0, gate_y + 3.66, gate_z + 0.57), rot_y=0.0,
                           scale=(2.4, 0.42, 0.04)),
    "mesh": mesh("cube", material(color_hex="#c09a50", roughness=0.38,
                                  metallic=0.5)),
    "type": {"value": "MeshInstance3D"},
}))

# Player cultivator. Temporary skinned swordsman GLB (idle/run/attack clips);
# the procedural cultivator remains available as a fallback mesh.
hero_z = -20.0
scene["entities"].append(entity("cultivator", {
    "transform": transform((0.0, world_y(0.0, hero_z), hero_z), rot_y=0.0,
                           scale=(0.0072, 0.0072, 0.0072)),
    "mesh": mesh("gltf:assets/models/Yasuo.glb",
                 material(color_hex="#ffffff", roughness=0.85)),
    "health": {"hp": 100, "maxHp": 100},
    "script": {"backend": "lua", "path": "assets/scripts/immortal_path.lua",
               "vars": {}},
    "type": {"value": "CharacterBody"},
}))

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n",
               encoding="utf-8")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(scene['entities'])} entities)")
























































