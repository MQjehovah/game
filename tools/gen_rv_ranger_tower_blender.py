"""Blender-authored ranger watchtower, exported as a glTF asset.

Run through Blender (GUI with the Blender MCP addon, or headless):
  blender --background --factory-startup --python tools/gen_rv_ranger_tower_blender.py

The origin is the tower footprint centre at ground level. Front/ladder is +Y
in Blender (which becomes -Z after the standard glTF Y-up conversion); the
script only needs to keep the model self-consistent for the game prefab.
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_ranger_tower.gltf"

PARTS = []


def material(name, rgb, roughness=0.85, metallic=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
    return m


def box(name, loc, size, mat, bevel=0.02, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot[0] or rot[1] or rot[2]:
        ob.rotation_euler = tuple(math.radians(a) for a in rot)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    ob.data.materials.append(mat)
    if bevel > 0:
        mod = ob.modifiers.new("soft_edges", "BEVEL")
        mod.width = bevel
        mod.segments = 2
        mod.limit_method = "ANGLE"
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.modifier_apply(modifier=mod.name)
    PARTS.append(ob)
    return ob


def cylinder(name, loc, radius, depth, mat, vertices=8, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                        depth=depth, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    if rot[0] or rot[1] or rot[2]:
        ob.rotation_euler = tuple(math.radians(a) for a in rot)
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def roof(name, mat, eave_z, ridge_z, x_half, y_half):
    verts = [
        (-x_half, -y_half, eave_z), (x_half, -y_half, eave_z),
        (x_half, y_half, eave_z), (-x_half, y_half, eave_z),
        (-x_half, 0.0, ridge_z), (x_half, 0.0, ridge_z),
    ]
    faces = [(0, 1, 5, 4), (2, 3, 4, 5)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(ob)
    ob.data.materials.append(mat)
    mod = ob.modifiers.new("roof_thickness", "SOLIDIFY")
    mod.thickness = 0.08
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=mod.name)
    PARTS.append(ob)
    return ob


def main():
    # Do not reset factory settings when running through the MCP addon: that
    # would tear down the addon's own scene context. Just clear the current
    # scene objects instead.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()

    wood = material("tower_wood", (0.50, 0.37, 0.24))
    trim = material("tower_trim", (0.26, 0.18, 0.11))
    roof_mat = material("tower_roof", (0.34, 0.38, 0.36), roughness=0.7)
    stone = material("tower_stone", (0.52, 0.50, 0.46), roughness=0.95)
    metal = material("tower_metal", (0.35, 0.30, 0.22), roughness=0.6)

    # Stone footings and four uprights.
    for x in (-0.88, 0.88):
        for y in (-0.88, 0.88):
            box("footing", (x, y, 0.18), (0.46, 0.46, 0.30), stone)
            box("leg", (x, y, 2.30), (0.18, 0.18, 4.35), wood)

    # Horizontal belts and cross-braces.
    for z in (1.25, 2.65, 3.95):
        box("belt_front", (0, 0.92, z), (2.0, 0.11, 0.14), trim)
        box("belt_back", (0, -0.92, z), (2.0, 0.11, 0.14), trim)
        box("belt_left", (-0.92, 0, z), (0.11, 2.0, 0.14), trim)
        box("belt_right", (0.92, 0, z), (0.11, 2.0, 0.14), trim)
    for side in (-1, 1):
        box("brace_x", (0, side * 0.92, 1.95), (0.10, 0.10, 2.15), trim,
            rot=(0, 45, 0))
        box("brace_x", (0, side * 0.92, 3.25), (0.10, 0.10, 1.85), trim,
            rot=(0, -45, 0))
        box("brace_y", (side * 0.92, 0, 1.95), (0.10, 0.10, 2.15), trim,
            rot=(45, 0, 0))
        box("brace_y", (side * 0.92, 0, 3.25), (0.10, 0.10, 1.85), trim,
            rot=(-45, 0, 0))

    # Platform, joists and railing.
    box("platform", (0, 0, 4.32), (2.65, 2.65, 0.18), wood)
    for x in (-0.75, 0, 0.75):
        box("joist", (x, 0, 4.18), (0.12, 2.75, 0.14), trim)
    for x in (-1.18, 1.18):
        for y in (-1.18, 1.18):
            box("rail_post", (x, y, 4.92), (0.10, 0.10, 1.18), trim)
    # Three rails; leave the +Y (ladder) side open in the middle.
    box("rail_front_l", (-0.62, 1.18, 5.40), (1.15, 0.10, 0.11), trim)
    box("rail_front_r", (0.62, 1.18, 5.40), (1.15, 0.10, 0.11), trim)
    box("rail_back", (0, -1.18, 5.40), (2.40, 0.10, 0.11), trim)
    box("rail_left", (-1.18, 0, 5.40), (0.10, 2.40, 0.11), trim)
    box("rail_right", (1.18, 0, 5.40), (0.10, 2.40, 0.11), trim)

    # Roof corner posts and small gable roof.
    for x in (-0.96, 0.96):
        for y in (-0.96, 0.96):
            box("roof_post", (x, y, 5.28), (0.12, 0.12, 1.65), trim)
    box("roof_beam_front", (0, 0.98, 6.02), (2.55, 0.12, 0.14), trim)
    box("roof_beam_back", (0, -0.98, 6.02), (2.55, 0.12, 0.14), trim)
    roof("roof", roof_mat, 6.08, 6.78, 1.55, 1.65)
    box("ridge", (0, 0, 6.80), (3.15, 0.12, 0.10), trim)

    # Ladder on +Y, from the ground to the platform.
    for x in (-0.36, 0.36):
        box("ladder_rail", (x, 1.35, 2.15), (0.08, 0.08, 4.30), metal)
    for i in range(10):
        z = 0.35 + i * 0.40
        box("rung", (0, 1.35, z), (0.78, 0.07, 0.06), metal)

    # A small map/flag box near the top landing.
    box("lookout_flag", (0.72, 1.24, 5.28), (0.08, 0.08, 1.35), metal)

    # Join and align outward normals.
    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.object
    merged.name = "RV_RangerTower"
    bpy.ops.object.origin_set(type="ORIGIN_CURSOR", center="MEDIAN")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bm = bmesh.new()
    bm.from_mesh(merged.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(merged.data)
    bm.free()
    merged.data.update()

    OUT.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(OUT),
        export_format="GLTF_SEPARATE",
        use_selection=True,
        export_apply=True,
        export_yup=True,
    )
    print(f"wrote {OUT}")
    print(f"verts={len(merged.data.vertices)} polys={len(merged.data.polygons)} "
          f"materials={len(merged.data.materials)}")


if __name__ == "__main__":
    main()
