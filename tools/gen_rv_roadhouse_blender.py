"""Blender-authored roadside POI building.

Run headless:
  "D:\\blender-3.1.2-windows-x64\\blender.exe" --background --factory-startup \
      --python tools/gen_rv_roadhouse_blender.py

Exports projects/rv/assets/models/rv/rv_roadhouse.gltf (+ .bin).
The model keeps the shared POI footprint: 6.2 x 5.2 m, front door on +Z,
floor top around y=0.06, roof ridge around y=3.45.
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_roadhouse.gltf"
EMISSIVE = ROOT / "projects/rv/assets/models/rv/rv_roadhouse_emissive.png"

PARTS = []
EMISSIVE_IMG = None


def material(name, rgb, roughness=0.85, metallic=0.0, emissive=0.50):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
        # A little baked ambient keeps shaded facades readable in the low-poly
        # game lighting (the engine has very dark shadow terms).
        em = bsdf.inputs.get("Emission")
        em_s = bsdf.inputs.get("Emission Strength")
        if em is not None:
            em.default_value = (*rgb, 1.0)
        if em_s is not None:
            em_s.default_value = emissive
        if EMISSIVE_IMG is not None and em is not None:
            tex = m.node_tree.nodes.new("ShaderNodeTexImage")
            tex.image = EMISSIVE_IMG
            tex.location = (-320, 220)
            m.node_tree.links.new(tex.outputs["Color"], em)
    return m


def box(name, loc, size, mat, bevel=0.025, rot_z=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=loc)
    ob = bpy.context.object
    ob.name = name
    ob.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot_z:
        ob.rotation_euler[2] = math.radians(rot_z)
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


def cylinder(name, loc, radius, depth, mat, vertices=8):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                        depth=depth, location=loc)
    ob = bpy.context.object
    ob.name = name
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def roof(name, mat, eave_z=2.55, ridge_z=3.45, x_half=3.35, y_half=2.9):
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
    # Give the roof real thickness so it reads from inside and outside.
    mod = ob.modifiers.new("roof_thickness", "SOLIDIFY")
    mod.thickness = 0.10
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=mod.name)
    PARTS.append(ob)
    return ob


def gable(name, x, mat, eave_z=2.55, ridge_z=3.45, y_half=2.55):
    verts = [(x, -y_half, eave_z), (x, y_half, eave_z), (x, 0.0, ridge_z)]
    faces = [(0, 1, 2)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(ob)
    ob.data.materials.append(mat)
    mod = ob.modifiers.new("gable_thickness", "SOLIDIFY")
    mod.thickness = 0.08
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=mod.name)
    PARTS.append(ob)
    return ob


def window(front_y, x, z, mat_trim, mat_glass, w=0.72, h=0.72,
           side_x=None, rot_y=False):
    if side_x is None:
        # front/back window, pane normal along Y
        box("window_recess", (x, front_y, z), (w, 0.035, h), mat_glass, 0.0)
        box("window_top", (x, front_y - 0.03, z + h * 0.5 + 0.06),
            (w + 0.16, 0.10, 0.10), mat_trim)
        box("window_bottom", (x, front_y - 0.03, z - h * 0.5 - 0.06),
            (w + 0.16, 0.10, 0.10), mat_trim)
        box("window_left", (x - w * 0.5 - 0.06, front_y - 0.03, z),
            (0.10, 0.10, h + 0.18), mat_trim)
        box("window_right", (x + w * 0.5 + 0.06, front_y - 0.03, z),
            (0.10, 0.10, h + 0.18), mat_trim)
        box("window_cross_v", (x, front_y - 0.055, z), (0.035, 0.03, h),
            mat_trim, 0.0)
        box("window_cross_h", (x, front_y - 0.055, z), (w, 0.03, 0.035),
            mat_trim, 0.0)
        # simple shutters
        for s in (-1, 1):
            box("shutter", (x + s * (w * 0.5 + 0.22), front_y - 0.05, z),
                (0.26, 0.055, h + 0.16), mat_trim, 0.015)
    else:
        box("window_recess", (side_x, x, z), (0.035, w, h), mat_glass, 0.0)
        box("window_top", (side_x, x, z + h * 0.5 + 0.06),
            (0.10, w + 0.16, 0.10), mat_trim)
        box("window_bottom", (side_x, x, z - h * 0.5 - 0.06),
            (0.10, w + 0.16, 0.10), mat_trim)
        box("window_left", (side_x, x - w * 0.5 - 0.06, z),
            (0.10, 0.10, h + 0.18), mat_trim)
        box("window_right", (side_x, x + w * 0.5 + 0.06, z),
            (0.10, 0.10, h + 0.18), mat_trim)
        box("window_cross_v", (side_x, x, z), (0.03, 0.035, h),
            mat_trim, 0.0)
        box("window_cross_h", (side_x, x, z), (0.03, w, 0.035),
            mat_trim, 0.0)


def main():
    global EMISSIVE_IMG
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()
    EMISSIVE_IMG = bpy.data.images.load(str(EMISSIVE))

    siding = material("siding_weathered", (0.72, 0.58, 0.40))
    siding_light = material("siding_pale", (0.88, 0.76, 0.58))
    trim = material("timber_dark", (0.36, 0.27, 0.18))
    roof_mat = material("roof_tin", (0.55, 0.58, 0.55), roughness=0.7)
    floor_mat = material("floor_boards", (0.62, 0.46, 0.30))
    stone = material("stone_foundation", (0.68, 0.65, 0.59), roughness=0.95)
    glass = material("window_glass", (0.58, 0.74, 0.85), roughness=0.15)
    metal = material("metal_rust", (0.50, 0.40, 0.28), roughness=0.65)

    # Foundation / floor / porch deck.
    box("stone_foundation", (0, 0, 0.05), (6.5, 5.5, 0.16), stone)
    box("floor", (0, 0, 0.15), (6.2, 5.2, 0.10), floor_mat)
    box("porch_deck", (0, -3.35, 0.15), (3.6, 1.5, 0.12), floor_mat)
    box("porch_step_1", (0, -4.18, 0.07), (2.6, 0.34, 0.14), stone)
    box("porch_step_2", (0, -4.46, 0.02), (2.9, 0.32, 0.12), stone)

    # Main walls. Front is -Y in Blender (exports to +Z in glTF).
    box("back_wall", (0, 2.55, 1.42), (6.2, 0.20, 2.55), siding)
    box("left_wall", (-3.05, 0, 1.42), (0.20, 5.2, 2.55), siding)
    box("right_wall", (3.05, 0, 1.42), (0.20, 5.2, 2.55), siding)
    # Door opening is 1.44 m wide, 2.2 m high.
    box("front_left", (-1.91, -2.55, 1.42), (2.28, 0.20, 2.55), siding)
    box("front_right", (1.91, -2.55, 1.42), (2.28, 0.20, 2.55), siding)
    box("front_header", (0, -2.55, 2.35), (1.54, 0.20, 0.40), siding_light)
    # Horizontal plank shadow lines and corner posts.
    for z in (0.36, 0.86, 1.36, 1.86, 2.32):
        box("siding_seam", (0, -2.66, z), (6.2, 0.035, 0.035), trim, 0.0)
        box("siding_seam", (0, 2.66, z), (6.2, 0.035, 0.035), trim, 0.0)
    for x in (-3.16, 3.16):
        box("corner_post", (x, -2.66, 1.30), (0.16, 0.16, 2.75), trim)
        box("corner_post", (x, 2.66, 1.30), (0.16, 0.16, 2.75), trim)
    for y in (-2.62, 2.62):
        box("corner_post_side", (3.16, y, 1.30), (0.16, 0.16, 2.75), trim)
        box("corner_post_side", (-3.16, y, 1.30), (0.16, 0.16, 2.75), trim)

    # Door frame and an ajar plank door.
    box("door_frame_left", (-0.78, -2.65, 1.18), (0.16, 0.16, 2.36), trim)
    box("door_frame_right", (0.78, -2.65, 1.18), (0.16, 0.16, 2.36), trim)
    box("door_frame_top", (0, -2.65, 2.34), (1.72, 0.16, 0.16), trim)
    box("door_panel", (-0.58, -2.56, 1.10), (1.28, 0.08, 2.08), floor_mat,
        bevel=0.02, rot_z=-24)
    box("door_handle", (-0.10, -2.62, 1.05), (0.12, 0.05, 0.10), metal)

    # Windows: two front, two side.
    window(-2.66, -2.35, 1.55, trim, glass)
    window(-2.66, 2.35, 1.55, trim, glass)
    window(-0.8, -2.66, 1.58, trim, glass, side_x=-3.16)
    window(0.8, -2.66, 1.58, trim, glass, side_x=3.16)

    # Gable roof, gable ends, fascia and ridge.
    roof("roof", roof_mat)
    gable("gable_left", -3.25, siding_light)
    gable("gable_right", 3.25, siding_light)
    box("ridge_beam", (0, 0, 3.48), (6.9, 0.18, 0.16), trim)
    for y in (-2.92, 2.92):
        box("fascia", (0, y, 2.52), (6.9, 0.12, 0.18), trim)
    # A few exposed rafter tails.
    for x in (-2.7, -1.0, 0.7, 2.4):
        box("rafter", (x, -2.95, 2.47), (0.11, 0.40, 0.13), trim)
        box("rafter", (x, 2.95, 2.47), (0.11, 0.40, 0.13), trim)

    # Porch posts, beam and shed roof.
    for x in (-1.55, 1.55):
        cylinder("porch_post", (x, -3.95, 1.23), 0.09, 2.35, trim)
        box("porch_brace", (x, -3.55, 2.18), (0.10, 0.72, 0.10),
            trim, 0.0, rot_z=0)
    box("porch_beam", (0, -3.95, 2.40), (3.5, 0.14, 0.16), trim)
    box("porch_roof", (0, -3.45, 2.52), (3.8, 1.85, 0.12), roof_mat,
        bevel=0.03)

    # Chimney and a little roof hardware.
    box("chimney", (2.25, 1.70, 3.18), (0.56, 0.56, 1.75), stone)
    box("chimney_cap", (2.25, 1.70, 4.08), (0.72, 0.72, 0.12), stone)
    box("chimney_flue", (2.25, 1.70, 4.22), (0.22, 0.22, 0.28), metal)

    # Interior: back counter, shelf and a small table so the inside is not empty.
    box("counter", (0.0, 2.05, 0.72), (3.8, 0.55, 0.95), floor_mat)
    box("counter_top", (0.0, 2.05, 1.22), (3.9, 0.66, 0.08), trim)
    for z in (1.55, 1.92):
        box("shelf", (0.0, 2.30, z), (3.1, 0.22, 0.07), trim)
    box("table_top", (-1.85, 0.65, 0.82), (1.35, 0.95, 0.08), floor_mat)
    for x in (-2.42, -1.28):
        for y in (0.28, 1.02):
            box("table_leg", (x, y, 0.43), (0.08, 0.08, 0.78), trim)

    # Join everything into one mesh with multiple material slots.
    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.object
    merged.name = "RV_Roadhouse"
    bpy.ops.object.origin_set(type="ORIGIN_CURSOR", center="MEDIAN")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    # Make all outward faces consistent (the custom roof/gable meshes can be
    # wound inward when built from raw pydata).
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
