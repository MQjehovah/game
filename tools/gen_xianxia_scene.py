#!/usr/bin/env python3
"""Generate the 'immortal path' vertical-slice scene.

The scene is intentionally a small, editable data file: a valley path with
procedural pines, rock spires, stone lanterns and a cultivator placeholder.
Run from the repository root:

    python tools/gen_xianxia_scene.py
"""
import json
import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects" / "xianxia" / "assets" / "scenes" / "immortal_path.json"

SEGMENTS = 48
SIZE = 160.0
HEIGHT = 6.0


def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def height(x, z):
    """Raw heightfield value in roughly [0, 1]; the scene scales it by HEIGHT."""
    ax = abs(x)
    base = 0.075 + 0.008 * math.sin(x * 0.55 + z * 0.31)
    ridge = smoothstep(8.0, 25.0, ax) * (
        0.78 + 0.20 * math.sin(z * 0.07) + 0.12 * math.sin(x * 0.13 + 1.7)
    )
    # A shallow notch keeps the walkable path readable between the ridges.
    path = smoothstep(5.0, 10.0, ax) * 0.03
    return base + ridge - path


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


def color(hex_value):
    return hex_value


def material(tex="", color_hex="#ffffff", roughness=0.9, metallic=0.0,
             uv_repeat=1.0, normal="", dirt="", rock=""):
    m = {
        "colorHex": color_hex,
        "roughness": roughness,
        "metallic": metallic,
        "ao": 1.0,
        "emissiveIntensity": 1.0,
    }
    if tex:
        m["albedoTex"] = tex
        m["uvRepeat"] = uv_repeat
    if normal:
        m["normalTex"] = normal
        m["normalScale"] = 0.65
    if dirt:
        m["dirtColorHex"] = dirt
    if rock:
        m["rockColorHex"] = rock
    return m


def transform(pos, rot_y=0.0, scale=(1.0, 1.0, 1.0)):
    return {
        "pos": [round(float(v), 3) for v in pos],
        "rot": [0.0, round(math.sin(rot_y * 0.5), 5), 0.0, round(math.cos(rot_y * 0.5), 5)],
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


scene = {
    "renderstack": {
        "ssao": True,
        "ssaoIntensity": 0.55,
        "volumetric": True,
        "volumetricStrength": 0.75,
        "bloom": True,
        "bloomThreshold": 1.15,
        "bloomStrength": 0.22,
        "bloomWidth": 1.6,
        "tonemap": True,
        "exposure": 0.98,
        "grade": True,
        "gradeSaturation": 0.92,
        "gradeContrast": 0.08,
        "gradeGain": 1.02,
        "gradeTint": [1.02, 1.01, 0.97, 1.0],
        "autoExposure": True,
        "autoExposureKey": 0.22,
        "vignette": True,
        "vignetteRadius": 0.62,
        "vignetteIntensity": 0.26,
        "shadowDistance": 105.0,
        "shadowSoftness": 1.4,
    },
    "entities": [],
}

# Atmosphere + sun share one entity: DrawSystem reads the environment component
# and the light component from the same world.
scene["entities"].append(entity("sun_light", {
    "transform": transform((0.0, 30.0, 0.0)),
    "light": {
        "type": "directional",
        "color": [1.0, 0.91, 0.76, 1.0],
        "sunDir": [-0.42, -0.72, -0.32],
        "intensity": 1.25,
        "radius": 10.0,
        "ambientStrength": 0.40,
        "useAtmosphere": False,
    },
    "environment": {
        "ambientColor": [0.56, 0.66, 0.76, 1.0],
        "ambientStrength": 0.46,
        "skyTop": [0.24, 0.42, 0.66, 1.0],
        "skyHorizon": [0.82, 0.87, 0.92, 1.0],
        "fogColor": [0.74, 0.82, 0.88, 1.0],
        "fogNear": 10.0,
        "fogFar": 78.0,
        "useAtmosphere": True,
        "skybox": False,
    },
    "type": {"value": "Light3D"},
}))

scene["entities"].append(entity("valley", {
    "transform": transform((0.0, 0.0, 0.0)),
    "mesh": mesh("terrain", material(
        tex="assets/textures/grass_proc.png",
        color_hex="#ffffff",
        roughness=0.98,
        uv_repeat=30.0,
        dirt="#9a8a67",
        rock="#77736a",
    ), cast_shadow=False),
    "terrain": {
        "segments": SEGMENTS,
        "size": SIZE,
        "heightScale": HEIGHT,
        "heights": heights(),
        "chunkGridDiv": 4,
        "chunkLodLevels": 2,
        "chunkBaseSubdiv": 12,
        "vegMeshKey": "grass",
        "vegCount": 6500,
        "vegSeed": 17,
        "vegSize": 0.55,
        "vegImpostorDistance": 65.0,
        "vegMinHeight": -1.0,
        "vegMaxHeight": 12.0,
        "vegMaxSlope": 0.55,
        "vegExcludeRadius": 5.5,
        "vegExcludeCenter": [0.0, 0.0, 0.0],
    },
    "type": {"value": "MeshInstance3D"},
}))

# Stone path: overlapping slabs follow the flat valley floor.
path_mat = material(tex="assets/textures/gravel_diff_2k.jpg",
                    color_hex="#c9c5b8", roughness=0.96, uv_repeat=2.0)
for i, z in enumerate(range(-36, 37, 8)):
    y = world_y(0.0, z) + 0.025
    scene["entities"].append(entity(f"path_{i}", {
        "transform": transform((0.0, y, z), rot_y=0.04 if i % 2 else -0.03,
                               scale=(4.4, 1.0, 8.4)),
        "mesh": mesh("road", path_mat),
        "type": {"value": "MeshInstance3D"},
    }))

# Pines: deterministic scatter away from the path.
rng = random.Random(23)
for i in range(38):
    side = -1.0 if i % 2 == 0 else 1.0
    x = side * rng.uniform(10.0, 34.0)
    z = rng.uniform(-46.0, 46.0)
    s = rng.uniform(0.72, 1.18)
    scene["entities"].append(entity(f"pine_{i}", {
        "transform": transform((x, world_y(x, z) - 0.10, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(s, s, s)),
        "mesh": mesh("pine", material(color_hex="#93a487", roughness=0.9)),
        "type": {"value": "MeshInstance3D"},
    }))

# Low shrubs/rocks soften the path edges.
for i in range(18):
    side = -1.0 if i % 2 == 0 else 1.0
    x = side * rng.uniform(6.0, 14.0)
    z = rng.uniform(-38.0, 38.0)
    s = rng.uniform(0.55, 1.0)
    scene["entities"].append(entity(f"bush_{i}", {
        "transform": transform((x, world_y(x, z) - 0.06, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(s, s, s)),
        "mesh": mesh("bush", material(color_hex="#aebba1", roughness=0.95)),
        "type": {"value": "MeshInstance3D"},
    }))
for i in range(12):
    side = -1.0 if i % 2 == 0 else 1.0
    x = side * rng.uniform(4.5, 11.0)
    z = rng.uniform(-40.0, 40.0)
    s = rng.uniform(0.45, 1.1)
    scene["entities"].append(entity(f"rock_{i}", {
        "transform": transform((x, world_y(x, z) - 0.12, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(s * 1.4, s * 0.7, s * 1.2)),
        "mesh": mesh("rock", material(color_hex="#77736a", roughness=0.98)),
        "type": {"value": "MeshInstance3D"},
    }))

# Distant rock spires frame the path like the reference cliffs.
spires = [
    (-8.0, -32.0, 0.95, 1.35),
    (8.0, -32.0, 1.00, 1.45),
    (-11.0, -24.0, 1.10, 1.20),
    (11.0, -24.0, 1.10, 1.30),
    (-17.0, -50.0, 2.00, 2.10),
    (17.0, -50.0, 2.00, 2.10),
    (-15.0, 22.0, 1.00, 1.10),
    (16.0, 31.0, 1.10, 1.25),
    (-29.0, 47.0, 1.40, 1.65),
    (33.0, 50.0, 1.20, 1.45),
    (-54.0, -48.0, 2.20, 2.30),
    (50.0, -44.0, 2.00, 2.10),
]
for i, (x, z, sxz, sy) in enumerate(spires):
    scene["entities"].append(entity(f"spire_{i}", {
        "transform": transform((x, world_y(x, z) - 0.8, z),
                               rot_y=rng.uniform(0.0, math.pi * 2.0),
                               scale=(sxz, sy, sxz)),
        "mesh": mesh("rock_pillar", material(color_hex="#8d887c", roughness=0.97)),
        "type": {"value": "MeshInstance3D"},
    }))

# Stone lanterns with warm point lights.
for i, (x, z) in enumerate([(3.6, -17.5), (-5.0, -25.0), (4.8, -31.0)]):
    y = world_y(x, z)
    scene["entities"].append(entity(f"lantern_{i}", {
        "transform": transform((x, y, z), rot_y=0.15 * i, scale=(1.0, 1.0, 1.0)),
        "mesh": mesh("stone_lantern", material(color_hex="#c0c0b5", roughness=0.9)),
        "type": {"value": "MeshInstance3D"},
    }))
    scene["entities"].append(entity(f"lantern_light_{i}", {
        "transform": transform((x, y + 1.55, z)),
        "light": {
            "type": "point",
            "color": [1.0, 0.60, 0.26, 1.0],
            "intensity": 2.0,
            "radius": 11.0,
        },
        "type": {"value": "Light3D"},
    }))

# Cool fill near the cliff spires so their shadowed faces read as rock.
for i, x in enumerate((-8.0, 8.0)):
    scene["entities"].append(entity(f"cliff_fill_{i}", {
        "transform": transform((x, 6.5, -30.0)),
        "light": {
            "type": "point",
            "color": [0.52, 0.66, 0.86, 1.0],
            "intensity": 1.2,
            "radius": 24.0,
        },
        "type": {"value": "Light3D"},
    }))

# A stone tablet at the end of the path is the first interaction point.
tablet_z = -32.0
tablet_y = world_y(0.0, tablet_z)
scene["entities"].append(entity("dao_tablet", {
    "transform": transform((0.0, tablet_y + 1.0, tablet_z), rot_y=0.0,
                           scale=(1.25, 2.0, 0.36)),
    "mesh": mesh("cube", material(color_hex="#3d4142", roughness=0.92)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("tablet_cap", {
    "transform": transform((0.0, tablet_y + 2.18, tablet_z), rot_y=0.0,
                           scale=(1.55, 0.22, 0.62)),
    "mesh": mesh("cube", material(color_hex="#4b4d49", roughness=0.9)),
    "type": {"value": "MeshInstance3D"},
}))

# Simple stone gate at the far end; composed from three boxes for now.
gate_z = -39.0
gate_y = world_y(0.0, gate_z)
for i, x in enumerate((-5.0, 5.0)):
    scene["entities"].append(entity(f"gate_pillar_{i}", {
        "transform": transform((x, gate_y + 2.2, gate_z), rot_y=0.0,
                               scale=(0.62, 4.4, 0.62)),
        "mesh": mesh("cube", material(color_hex="#4a4b46", roughness=0.95)),
        "type": {"value": "MeshInstance3D"},
    }))
scene["entities"].append(entity("gate_lintel", {
    "transform": transform((0.0, gate_y + 4.55, gate_z), rot_y=0.0,
                           scale=(11.0, 0.55, 0.9)),
    "mesh": mesh("cube", material(color_hex="#444741", roughness=0.94)),
    "type": {"value": "MeshInstance3D"},
}))
scene["entities"].append(entity("gate_roof", {
    "transform": transform((0.0, gate_y + 4.95, gate_z), rot_y=0.0,
                           scale=(12.2, 0.28, 1.55)),
    "mesh": mesh("cube", material(color_hex="#33362f", roughness=0.95)),
    "type": {"value": "MeshInstance3D"},
}))

# Player cultivator. The script owns movement/camera focus; the mesh is an
# origin-at-feet procedural placeholder.
hero_z = -20.0
scene["entities"].append(entity("cultivator", {
    "transform": transform((0.0, world_y(0.0, hero_z), hero_z), rot_y=0.0,
                           scale=(1.0, 1.0, 1.0)),
    "mesh": mesh("cultivator", material(color_hex="#ffffff", roughness=0.85)),
    "health": {"hp": 100, "maxHp": 100},
    "script": {"backend": "lua", "path": "assets/scripts/immortal_path.lua", "vars": {}},
    "type": {"value": "CharacterBody"},
}))

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(scene['entities'])} entities)")





