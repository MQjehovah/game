"""Blender-authored lattice radio mast, generated through Blender MCP.

Origin is the footprint centre at ground level. The triangular lattice reaches
z ~= 13.5 m; the game keeps its own red beacon light on top of the model.
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_radio_mast.gltf"

PARTS = []
LEGS = ((-0.90, -0.52), (0.90, -0.52), (0.0, 1.04))


def material(name, rgb, roughness=0.85, metallic=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
    return m


def box(name, loc, size, mat, bevel=0.010, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot[0] or rot[1] or rot[2]:
        ob.rotation_euler = tuple(math.radians(a) for a in rot)
        bpy.ops.object.transform_apply(location=False, rotation=True,
                                       scale=False)
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


def cyl(name, loc, radius, depth, mat, vertices=10, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                        depth=depth, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    if rot[0] or rot[1] or rot[2]:
        ob.rotation_euler = tuple(math.radians(a) for a in rot)
        bpy.ops.object.transform_apply(location=False, rotation=True,
                                       scale=False)
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def beam_between(name, p1, p2, radius, mat, vertices=8):
    a, b = Vector(p1), Vector(p2)
    v = b - a
    length = v.length
    if length < 1e-5:
        return None
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                        depth=length,
                                        location=(a + b) * 0.5)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.rotation_mode = "QUATERNION"
    ob.rotation_quaternion = v.to_track_quat("Z", "Y")
    bpy.ops.object.transform_apply(location=False, rotation=True,
                                   scale=False)
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def main():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()

    steel = material("mast_steel", (0.40, 0.43, 0.43), roughness=0.5,
                     metallic=0.65)
    steel_dark = material("mast_steel_dark", (0.22, 0.25, 0.26),
                          roughness=0.6, metallic=0.5)
    concrete = material("mast_concrete", (0.52, 0.50, 0.45), roughness=0.96)
    dish_mat = material("mast_dish", (0.72, 0.74, 0.72), roughness=0.45,
                        metallic=0.1)
    warning = material("mast_warning", (0.68, 0.14, 0.10), roughness=0.6)

    base_z = 0.10
    top_z = 13.0

    # Concrete pad plus individual footings.
    box("pad", (0, 0, 0.14), (2.55, 2.55, 0.28), concrete, bevel=0.02)
    for lx, ly in LEGS:
        box("footing", (lx, ly, 0.30), (0.44, 0.44, 0.34), concrete,
            bevel=0.02)

    # Three tapered legs.
    for i, (lx, ly) in enumerate(LEGS):
        beam_between("leg_%d" % i, (lx, ly, base_z), (lx * 0.62, ly * 0.62,
                                                      top_z),
                     0.072, steel)

    # Horizontal belts at every other metre.
    belt_zs = (2.0, 4.0, 6.0, 8.0, 10.0, 12.0)
    for z in belt_zs:
        pts = [(lx * (1.0 - 0.38 * (z / top_z)),
                ly * (1.0 - 0.38 * (z / top_z)), z) for lx, ly in LEGS]
        for i in range(3):
            beam_between("belt", pts[i], pts[(i + 1) % 3], 0.045,
                         steel_dark)

    # Diagonal X bracing on each of the three faces.
    for level, z in enumerate(belt_zs[:-1]):
        z2 = belt_zs[level + 1]
        for i in range(3):
            la, lb = LEGS[i], LEGS[(i + 1) % 3]
            ka = 1.0 - 0.38 * (z / top_z)
            kb = 1.0 - 0.38 * (z2 / top_z)
            p1 = (la[0] * ka, la[1] * ka, z)
            p2 = (lb[0] * kb, lb[1] * kb, z2)
            p3 = (lb[0] * ka, lb[1] * ka, z)
            p4 = (la[0] * kb, la[1] * kb, z2)
            if level % 2 == 0:
                beam_between("brace", p1, p2, 0.028, steel_dark,
                             vertices=6)
            else:
                beam_between("brace", p3, p4, 0.028, steel_dark,
                             vertices=6)

    # Service platform and railing around the 12 m belt.
    cyl("platform", (0, 0, 12.12), 0.86, 0.09, steel_dark, vertices=12)
    for sx in (-0.62, 0.62):
        for sy in (-0.62, 0.62):
            box("rail_post", (sx, sy, 12.55), (0.055, 0.055, 0.82), steel)
    for sx, sy, w, d in ((0, 0.62, 1.32, 0.055),
                         (0, -0.62, 1.32, 0.055),
                         (0.62, 0, 0.055, 1.32),
                         (-0.62, 0, 0.055, 1.32)):
        box("rail", (sx, sy, 12.88), (w, d, 0.055), steel)

    # Top mast, two crossbars and a red beacon mount.
    cyl("top_mast", (0, 0, 12.85), 0.055, 1.55, steel_dark, vertices=8)
    box("top_bar_low", (0, 0, 12.55), (1.05, 0.055, 0.055), steel)
    box("top_bar_high", (0, 0, 13.15), (0.62, 0.045, 0.045), steel)
    for sx in (-0.38, 0.38):
        cyl("whip", (sx, 0, 13.35), 0.022, 0.55, steel_dark, vertices=6)
    box("beacon_base", (0, 0, 13.53), (0.20, 0.20, 0.12), warning,
        bevel=0.01)

    # Ladder up the front face.
    for sx in (-0.24, 0.24):
        box("ladder_rail", (sx, -0.60, 6.10), (0.05, 0.05, 11.6), steel)
    for i in range(20):
        z = 0.55 + i * 0.58
        box("ladder_rung", (0, -0.60, z), (0.52, 0.045, 0.045), steel,
            bevel=0.0)

    # Microwave dish on the south face.
    box("dish_arm", (0, -0.72, 9.10), (0.12, 0.34, 0.12), steel_dark)
    cyl("dish", (0, -0.96, 9.10), 0.44, 0.18, dish_mat, vertices=16,
        rot=(90, 0, 0))
    cyl("dish_feed", (0, -1.13, 9.10), 0.035, 0.34, steel_dark, vertices=6,
        rot=(90, 0, 0))
    box("dish_tip", (0, -1.30, 9.10), (0.07, 0.07, 0.07), warning)

    # Small equipment cabinet at the base.
    box("cabinet", (0.02, -1.20, 0.66), (0.62, 0.40, 0.88), steel_dark,
        bevel=0.02)
    box("cabinet_door", (0.02, -1.405, 0.66), (0.48, 0.02, 0.68), steel,
        bevel=0.0)
    box("cabinet_plate", (0.02, -1.42, 0.83), (0.20, 0.01, 0.12), warning,
        bevel=0.0)

    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.view_layer.objects.active
    merged.name = "RV_RadioMast"
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
    print("wrote %s verts=%d polys=%d materials=%d" %
          (OUT, len(merged.data.vertices), len(merged.data.polygons),
           len(merged.data.materials)))


if __name__ == "__main__":
    main()
