"""Blender-authored wooden roadside utility pole, generated through Blender MCP.

Origin is the footprint centre at ground level. The crossarm runs along local X
so the game can aim it with SetRotationY(dirYaw). Total height ~7.0 m, wire
attachment points sit at x = +/-0.55, y = 6.08.
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_utility_pole.gltf"

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


def box(name, loc, size, mat, bevel=0.012, rot=(0, 0, 0)):
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


def cyl(name, loc, radius, depth, mat, vertices=12, rot=(0, 0, 0)):
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


def cone(name, loc, r1, r2, depth, mat, vertices=12):
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=r1, radius2=r2,
                                    depth=depth, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def main():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()

    wood = material("pole_wood", (0.40, 0.29, 0.18), roughness=0.92)
    wood_dark = material("pole_wood_dark", (0.25, 0.17, 0.10), roughness=0.95)
    metal = material("pole_metal", (0.28, 0.30, 0.30), roughness=0.55,
                     metallic=0.55)
    ceramic = material("pole_insulator", (0.34, 0.42, 0.39), roughness=0.35,
                       metallic=0.05)
    concrete = material("pole_concrete", (0.52, 0.50, 0.45), roughness=0.96)
    warning = material("pole_warning", (0.72, 0.58, 0.14), roughness=0.7)

    # Concrete footing and the tapered wooden mast.
    cyl("footing", (0, 0, 0.22), 0.30, 0.44, concrete, vertices=12)
    cone("mast", (0, 0, 3.55), 0.155, 0.095, 6.80, wood, vertices=10)

    # Crossarm with a pair of diagonal braces back to the mast.
    box("crossarm", (0, 0, 5.94), (1.70, 0.11, 0.13), wood_dark)
    box("arm_cap_l", (-0.79, 0, 5.99), (0.10, 0.13, 0.16), metal)
    box("arm_cap_r", (0.79, 0, 5.99), (0.10, 0.13, 0.16), metal)
    for s in (-1, 1):
        box("brace", (s * 0.38, 0, 5.52), (0.075, 0.075, 0.98), wood_dark,
            rot=(0, s * 36, 0))
    box("arm_bolt", (0, 0, 5.94), (0.16, 0.16, 0.20), metal)

    # Insulators: two wire positions plus a spare pair inboard.
    for x in (-0.55, 0.55):
        cyl("insulator_base", (x, 0, 6.02), 0.085, 0.07, metal, vertices=8)
        cyl("insulator", (x, 0, 6.10), 0.060, 0.18, ceramic, vertices=8)
        cyl("insulator_top", (x, 0, 6.20), 0.035, 0.05, metal, vertices=6)
    for x in (-0.24, 0.24):
        cyl("insulator_spare", (x, 0, 6.07), 0.055, 0.16, ceramic, vertices=8)

    # Pole-top pin insulator.
    cyl("top_pin", (0, 0, 6.92), 0.075, 0.07, metal, vertices=8)
    cyl("top_insulator", (0, 0, 7.02), 0.058, 0.17, ceramic, vertices=8)

    # Pole-mounted distribution transformer on the road-facing side.
    box("trafo_bracket_a", (0, 0.20, 5.10), (0.34, 0.30, 0.07), metal)
    box("trafo_bracket_b", (0, 0.20, 4.56), (0.34, 0.30, 0.07), metal)
    cyl("transformer", (0, 0.40, 4.84), 0.235, 0.80, metal, vertices=12)
    cyl("trafo_lid", (0, 0.40, 5.26), 0.205, 0.06, wood_dark, vertices=12)
    cyl("trafo_bushing_a", (-0.11, 0.44, 5.40), 0.035, 0.20, ceramic,
        vertices=6)
    cyl("trafo_bushing_b", (0.11, 0.44, 5.40), 0.035, 0.20, ceramic,
        vertices=6)
    box("trafo_label", (0, 0.645, 4.84), (0.20, 0.015, 0.24), warning,
        bevel=0.0)

    # Climbing steps alternate either side of the mast.
    for i in range(10):
        z = 1.05 + i * 0.42
        s = -1 if (i % 2 == 0) else 1
        box("step", (s * 0.16, 0, z), (0.30, 0.045, 0.045), metal,
            bevel=0.0)

    # Small hazard plate low on the mast.
    box("hazard_plate", (0, 0.145, 1.72), (0.22, 0.025, 0.30), warning,
        bevel=0.0)

    # Join into a single mesh and recalculate outward normals.
    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.view_layer.objects.active
    merged.name = "RV_UtilityPole"
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
