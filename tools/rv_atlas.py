#!/usr/bin/env python3
"""Shared helpers for the POI product-packaging atlas (rv_products.png)."""

TILES = 4  # 4x4 grid -> 16 packaging tiles


def tile_uvs(tile, inset=0.012):
    """Return 24 UVs (4 per box face) mapping a box to one atlas tile."""
    c = tile % TILES
    r = tile // TILES
    u0 = c / TILES + inset
    u1 = (c + 1) / TILES - inset
    v0 = r / TILES + inset
    v1 = (r + 1) / TILES - inset
    quad = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
    return quad * 6


def product_material(name):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [1, 1, 1, 1],
            "metallicFactor": 0.02,
            "roughnessFactor": 0.6,
            "baseColorTexture": {"index": 0},
        },
    }


def atlas_extras():
    """Top-level glTF images/textures/samplers for rv_products.png."""
    return {
        "images": [{"uri": "rv_products.png"}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987,
                      "wrapS": 33071, "wrapT": 33071}],
    }
