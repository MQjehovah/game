"""Blender-authored windmill tower + rotor blade.

Exports:
  projects/rv/assets/models/rv/rv_windmill_tower.gltf
  projects/rv/assets/models/rv/rv_windmill_blade.gltf
Origin is at ground/centre; blade long axis is Blender Z (which becomes world Y
in the game's Y-up glTF space).
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"
PARTS = []


def clear_all():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()


def material(name, rgb, roughness=0.85, metallic=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
    return m


def box(name, loc, size, mat, bevel=0.018, rot=(0, 0, 0)):
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


def cyl(name, loc, radius, depth, mat, vertices=10, rot=(0, 0, 0)):
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


def finish(name: str, out_path: Path):
    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.view_layer.objects.active
    merged.name = name
    bpy.ops.object.origin_set(type="ORIGIN_CURSOR", center="MEDIAN")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bm = bmesh.new()
    bm.from_mesh(merged.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(merged.data)
    bm.free()
    merged.data.update()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(out_path),
        export_format="GLTF_SEPARATE",
        use_selection=True,
        export_apply=True,
        export_yup=True,
    )
    print("wrote %s verts=%d polys=%d materials=%d" %
          (out_path, len(merged.data.vertices), len(merged.data.polygons),
           len(merged.data.materials)))


def build_tower():
    wood = material("mill_wood", (0.48, 0.35, 0.22))
    trim = material("mill_trim", (0.24, 0.17, 0.10))
    metal = material("mill_metal", (0.31, 0.34, 0.33), roughness=0.55,
                     metallic=0.45)
    stone = material("mill_stone", (0.48, 0.46, 0.41), roughness=0.95)

    # Four tapered legs approximated with upright beams + cross bracing.
    for x in (-0.58, 0.58):
        for y in (-0.58, 0.58):
            box("footing", (x, y, 0.16), (0.38, 0.38, 0.26), stone)
            box("leg", (x, y, 3.18), (0.15, 0.15, 6.10), wood)
    for z in (1.10, 2.45, 3.80, 5.10, 6.05):
        box("belt_f", (0, 0.60, z), (1.20, 0.09, 0.11), trim)
        box("belt_b", (0, -0.60, z), (1.20, 0.09, 0.11), trim)
        box("belt_l", (-0.60, 0, z), (0.09, 1.20, 0.11), trim)
        box("belt_r", (0.60, 0, z), (0.09, 1.20, 0.11), trim)
    for s in (-1, 1):
        box("brace_low_f", (0, s * 0.60, 1.75), (0.08, 0.08, 1.45), trim,
            rot=(0, 38, 0))
        box("brace_low_b", (0, s * 0.60, 1.75), (0.08, 0.08, 1.45), trim,
            rot=(0, -38, 0))
        box("brace_low_x", (s * 0.60, 0, 1.75), (0.08, 0.08, 1.45), trim,
            rot=(38, 0, 0))
        box("brace_low_y", (s * 0.60, 0, 1.75), (0.08, 0.08, 1.45), trim,
            rot=(-38, 0, 0))
        box("brace_hi_f", (0, s * 0.60, 4.55), (0.08, 0.08, 1.6), trim,
            rot=(0, 34, 0))
        box("brace_hi_x", (s * 0.60, 0, 4.55), (0.08, 0.08, 1.6), trim,
            rot=(-34, 0, 0))

    # Service platform at the top and a light rail.
    box("platform", (0, 0, 6.12), (1.45, 1.45, 0.12), trim)
    for x in (-0.62, 0.62):
        for y in (-0.62, 0.62):
            box("rail_post", (x, y, 6.42), (0.07, 0.07, 0.60), metal)
    box("rail_f", (0, 0.62, 6.68), (1.25, 0.06, 0.06), metal)
    box("rail_b", (0, -0.62, 6.68), (1.25, 0.06, 0.06), metal)
    box("rail_l", (-0.62, 0, 6.68), (0.06, 1.25, 0.06), metal)
    box("rail_r", (0.62, 0, 6.68), (0.06, 1.25, 0.06), metal)

    # Rotor hub/shaft projects toward world +Z (Blender -Y), matching the
    # scripted rotor plane at windmill.z + 0.35.
    box("hub", (0, -0.32, 6.92), (0.38, 0.55, 0.38), metal)
    cyl("shaft", (0, -0.52, 6.92), 0.11, 0.45, metal,
        vertices=8, rot=(90, 0, 0))
    box("hub_cap", (0, -0.72, 6.92), (0.26, 0.06, 0.26), metal)

    # Tail vane on the opposite side, plus a tiny ladder and platform hatch.
    box("tail_arm", (0, 0.78, 6.95), (0.12, 0.95, 0.10), metal)
    box("tail_vane", (0, 1.28, 7.15), (0.08, 0.60, 0.48), trim)
    for i in range(7):
        z = 0.42 + i * 0.52
        box("ladder_rung", (0.78, 0.18, z), (0.34, 0.05, 0.05), metal)
    box("ladder_rail", (0.93, 0.18, 2.15), (0.06, 0.06, 4.4), metal)
    box("ladder_rail2", (0.63, 0.18, 2.15), (0.06, 0.06, 4.4), metal)
    box("hatch", (-0.35, 0.0, 6.20), (0.42, 0.42, 0.06), metal)


def build_blade():
    wood = material("blade_wood", (0.55, 0.42, 0.27))
    trim = material("blade_trim", (0.26, 0.18, 0.11))
    metal = material("blade_metal", (0.30, 0.31, 0.29), roughness=0.55,
                     metallic=0.4)

    # Spar along Blender Z = world Y.
    box("spar", (0, 0, 0), (0.09, 0.08, 2.82), trim)
    # Wider sail on the outer half; root remains narrow.
    box("sail_outer", (0, 0.012, 0.55), (0.48, 0.045, 1.62), wood,
        bevel=0.01)
    box("sail_inner", (0, 0.012, -0.82), (0.26, 0.04, 0.68), wood,
        bevel=0.01)
    # Small cross ribs and a metal tip cap.
    for z in (0.0, 0.45, 0.90, 1.28):
        box("rib", (0, 0.04, z), (0.52, 0.035, 0.035), trim)
    box("tip", (0, 0.02, 1.42), (0.18, 0.06, 0.08), metal)
    box("root_clamp", (0, 0.0, -1.35), (0.18, 0.12, 0.14), metal)


def main():
    clear_all()
    build_tower()
    finish("RV_WindmillTower", OUT / "rv_windmill_tower.gltf")
    clear_all()
    build_blade()
    finish("RV_WindmillBlade", OUT / "rv_windmill_blade.gltf")


if __name__ == "__main__":
    main()
