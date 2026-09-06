# One-shot scene migration for NeonOps levels: tech-floor texture, perimeter
# walls, corner beacon towers with point lights, per-level atmosphere themes.
# Run from the repo root: python tools/update_fps_scenes.py
import json

THEMES = {
    "level_01.json": {
        "ground_half": 30, "wall_uv": 8,
        "skyTop": [0.28, 0.38, 0.58, 1], "skyHorizon": [0.55, 0.65, 0.80, 1],
        "ambient": [0.45, 0.55, 0.72, 1], "ambientStrength": 0.55,
        "fog": [0.45, 0.55, 0.70, 1],
        "sunDir": [-0.45, -0.9, -0.35], "sunColor": [1, 0.95, 0.88, 1], "sunInt": 2.0,
        "beaconColor": "#FFB347", "beaconLight": [1.0, 0.70, 0.35, 1], "lightInt": 2.2,
        "bloomStrength": 0.35, "vignette": False,
    },
    "level_02.json": {
        "ground_half": 32, "wall_uv": 8,
        "skyTop": [0.16, 0.18, 0.34, 1], "skyHorizon": [0.82, 0.44, 0.24, 1],
        "ambient": [0.60, 0.44, 0.38, 1], "ambientStrength": 0.50,
        "fog": [0.62, 0.42, 0.34, 1],
        "sunDir": [-0.62, -0.38, -0.18], "sunColor": [1.0, 0.62, 0.36, 1], "sunInt": 1.8,
        "beaconColor": "#FFC46B", "beaconLight": [1.0, 0.62, 0.32, 1], "lightInt": 2.4,
        "bloomStrength": 0.42, "vignette": False,
    },
    "level_03.json": {
        "ground_half": 34, "wall_uv": 9,
        "skyTop": [0.015, 0.025, 0.06, 1], "skyHorizon": [0.07, 0.11, 0.20, 1],
        "ambient": [0.18, 0.22, 0.32, 1], "ambientStrength": 0.52,
        "fog": [0.05, 0.08, 0.14, 1],
        "sunDir": [0.35, -0.85, 0.25], "sunColor": [0.55, 0.65, 0.95, 1], "sunInt": 0.9,
        "beaconColor": "#37E1FF", "beaconLight": [0.25, 0.85, 1.0, 1], "lightInt": 2.8,
        "bloomStrength": 0.55, "vignette": True,
    },
}

BASE = "projects/fps/assets/scenes/"


def ent(name, comps, eid):
    return {"components": comps, "id": eid, "name": name}


def wall(name, pos, scale, eid):
    return ent(name, {
        "transform": {"pos": pos, "rot": [0, 0, 0, 1], "scale": scale},
        "mesh": {
            "meshKey": "cube", "castShadow": False,
            "material": {"albedoTex": "assets/textures/wall_tech.png",
                         "colorHex": "#B8C7D6", "roughness": 0.85,
                         "metallic": 0.0, "ao": 1, "emissiveIntensity": 1,
                         "uvRepeat": 8},
        },
    }, eid)


def beacon(name, x, z, theme, eid):
    ents = [
        ent(name + " Pillar", {
            "transform": {"pos": [x, 2.5, z], "rot": [0, 0, 0, 1], "scale": [0.9, 5, 0.9]},
            "mesh": {"meshKey": "cube", "castShadow": True,
                     "material": {"colorHex": "#4A5568", "roughness": 0.7,
                                  "metallic": 0.2, "ao": 1, "emissiveIntensity": 1}},
        }, eid),
        ent(name + " Head", {
            "transform": {"pos": [x, 5.4, z], "rot": [0, 0, 0, 1], "scale": [1.4, 0.9, 1.4]},
            "mesh": {"meshKey": "cube", "castShadow": False,
                     "material": {"colorHex": theme["beaconColor"], "roughness": 0.4,
                                  "metallic": 0.0, "ao": 1, "emissiveIntensity": 2.2}},
        }, eid + 1),
        ent(name + " Light", {
            "transform": {"pos": [x, 5.8, z], "rot": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "light": {"type": "point", "color": theme["beaconLight"],
                      "intensity": theme["lightInt"], "radius": 22},
            "type": {"value": "Light3D"},
        }, eid + 2),
    ]
    return ents


def update(scene, theme):
    next_id = 100
    for e in scene["entities"]:
        comps = e.get("components", {})
        if e.get("name") == "Ground":
            mesh = comps["mesh"]
            mesh["material"]["albedoTex"] = "assets/textures/ground_tech.png"
            mesh["material"]["uvRepeat"] = 12
        if comps.get("light", {}).get("type") == "directional":
            comps["light"]["sunDir"] = theme["sunDir"]
            comps["light"]["color"] = theme["sunColor"]
            comps["light"]["intensity"] = theme["sunInt"]
    half = theme["ground_half"]
    w = half * 2 + 1
    scene["entities"] += [
        wall("Wall N", [0, 1.5, -half - 0.5], [w, 3, 1], next_id),
        wall("Wall S", [0, 1.5, half + 0.5], [w, 3, 1], next_id + 1),
        wall("Wall W", [-half - 0.5, 1.5, 0], [1, 3, w], next_id + 2),
        wall("Wall E", [half + 0.5, 1.5, 0], [1, 3, w], next_id + 3),
    ]
    next_id += 10
    c = half - 3.5
    for i, (x, z) in enumerate([(-c, -c), (c, -c), (-c, c), (c, c)]):
        scene["entities"] += beacon("Beacon %d" % (i + 1), x, z, theme, next_id)
        next_id += 3
    env = scene["environment"]
    env["skyTop"] = theme["skyTop"]
    env["skyHorizon"] = theme["skyHorizon"]
    env["ambientColor"] = theme["ambient"]
    env["ambientStrength"] = theme["ambientStrength"]
    env["fogColor"] = theme["fog"]
    rs = scene.setdefault("renderstack", {
        "autoExposure": False, "autoExposureKey": 0.18, "bloom": True,
        "bloomStrength": 0.35, "bloomThreshold": 1, "exposure": 1,
        "fog": False, "fogColor": [1, 1, 1, 1], "fogDensity": 0.02,
        "grade": False, "gradeContrast": 0, "gradeGain": 1, "gradeGamma": 1,
        "gradeLift": 0, "gradeSaturation": 1, "gradeTint": [1, 1, 1, 1],
        "ssao": False, "ssaoIntensity": 1, "ssr": False, "ssrStrength": 1,
        "tonemap": True, "vignette": False, "vignetteIntensity": 0.5,
        "vignetteRadius": 0.6, "volumetric": False, "volumetricStrength": 1,
    })
    rs["bloomStrength"] = theme["bloomStrength"]
    rs["vignette"] = theme["vignette"]


for fname, theme in THEMES.items():
    with open(BASE + fname, "r", encoding="utf-8") as f:
        scene = json.load(f)
    update(scene, theme)
    with open(BASE + fname, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)
    print("updated", fname)
