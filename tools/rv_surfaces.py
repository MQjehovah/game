#!/usr/bin/env python3
"""Shared helpers for the RV surface detail textures (rv_tex_*.png).

The maps are neutral grayscale detail patterns; glTF materials keep their
baseColorFactor, so texture * tint gives wood grain / brushed metal / fabric
weave while preserving the existing palette.
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
INDEX = {name: i for i, name in enumerate(SURFACES)}


def surface_material(mat, name):
    """Attach the named detail map to an existing glTF material dict."""
    if name is not None:
        mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": INDEX[name]}
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
    return {
        "images": [{"uri": "rv_tex_%s.png" % n} for n in SURFACES],
        "textures": [{"source": i, "sampler": 0} for i in range(len(SURFACES))],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 10497, "wrapT": 10497}],
    }
