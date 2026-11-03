#!/usr/bin/env python3
"""Shared helpers for the RV surface detail textures (rv_tex_*.png).

Each surface ships three maps:
  rv_tex_<name>.png       neutral base-colour detail (tinted by baseColorFactor)
  rv_tex_<name>_n.png     tangent-space normal map
  rv_tex_<name>_orm.png   R=AO, G=roughness, B=metallic

Two emissive maps (lamp / tail) light the head- and tail-lights.
"""

SURFACES = [
    "wood_light",
    "wood_dark",
    "metal",
    "metal_dark",
    "fabric",
    "plastic",
    "glass",
    "rubber",
    "roof",
    "body",
    "solar",
    "leaf",
]
INDEX = {name: i * 3 for i, name in enumerate(SURFACES)}
EXTRA_IMAGES = ["rv_tex_lamp.png", "rv_tex_tail.png"]
EXTRA_BASE = len(SURFACES) * 3          # lamp, tail texture indices


def surface_material(mat, name):
    """Attach base/normal/ORM maps to an existing glTF material dict."""
    if name is None:
        return mat
    base = INDEX[name]
    pbr = mat["pbrMetallicRoughness"]
    pbr["baseColorTexture"] = {"index": base}
    pbr["metallicRoughnessTexture"] = {"index": base + 2}
    mat["normalTexture"] = {"index": base + 1, "scale": 1.0}
    mat["occlusionTexture"] = {"index": base + 2}
    return mat


def surface_emissive(mat, name):
    """Attach one of the extra emissive maps (lamp / tail)."""
    idx = EXTRA_BASE + (0 if name == "lamp" else 1)
    mat["emissiveTexture"] = {"index": idx}
    return mat


def face_uvs(points, normal, scale=0.85):
    """Planar UVs for a quad/triangle, projected along its dominant axis."""
    if abs(normal[0]) > 0.5:
        a, b = 2, 1          # YZ plane
    elif abs(normal[1]) > 0.5:
        a, b = 0, 2          # XZ plane
    else:
        a, b = 0, 1          # XY plane
    return [(p[a] * scale, p[b] * scale) for p in points]


def surface_extras():
    """Top-level glTF images/textures/samplers for the detail maps."""
    images = []
    for name in SURFACES:
        images.extend([
            {"uri": "rv_tex_%s.png" % name},
            {"uri": "rv_tex_%s_n.png" % name},
            {"uri": "rv_tex_%s_orm.png" % name},
        ])
    images.extend({"uri": u} for u in EXTRA_IMAGES)
    return {
        "images": images,
        "textures": [{"source": i, "sampler": 0} for i in range(len(images))],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
    }
