"""Blender-authored low-poly wildlife, generated through Blender MCP.

Exports (origin at ground/waterline centre, front toward Blender -Y):
  projects/rv/assets/models/rv/rv_rabbit.gltf
  projects/rv/assets/models/rv/rv_bird.gltf
  projects/rv/assets/models/rv/rv_duck.gltf
  projects/rv/assets/models/rv/rv_deer.gltf

The game maps Blender -Y to world +Z, so models face local +Z and can be
aimed with SetRotationY(atan2(dx, dz)).
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv"

PARTS = []


def material(name, rgb, roughness=0.9, metallic=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
    return m


def clear_all():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for m in list(bpy.data.materials):
        bpy.data.materials.remove(m)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()


def box(name, loc, size, mat, bevel=0.008, rot=(0, 0, 0)):
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


def sphere(name, loc, size, mat, segments=10, rings=6):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings,
                                         radius=1.0, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def cone(name, loc, r1, r2, depth, mat, vertices=10, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=r1, radius2=r2,
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


def beam(name, p1, p2, radius, mat, vertices=8):
    a, b = Vector(p1), Vector(p2)
    v = b - a
    if v.length < 1e-5:
        return None
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius,
                                        depth=v.length,
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


def finish(name, filename):
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

    path = OUT / filename
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        export_format="GLTF_SEPARATE",
        use_selection=True,
        export_apply=True,
        export_yup=True,
    )
    print("wrote %s verts=%d polys=%d materials=%d" %
          (path, len(merged.data.vertices), len(merged.data.polygons),
           len(merged.data.materials)))


def build_rabbit():
    fur = material("rabbit_fur", (0.24, 0.15, 0.08))
    fur_light = material("rabbit_fur_light", (0.33, 0.22, 0.13))
    belly = material("rabbit_belly", (0.58, 0.50, 0.40))
    pink = material("rabbit_pink", (0.48, 0.24, 0.24))
    dark = material("rabbit_eye", (0.05, 0.04, 0.035), roughness=0.4)
    tail = material("rabbit_tail", (0.66, 0.61, 0.52))

    # Haunch + body + chest.
    sphere("haunch", (0, 0.10, 0.15), (0.135, 0.145, 0.135), fur)
    sphere("body", (0, -0.055, 0.17), (0.125, 0.185, 0.125), fur)
    sphere("chest", (0, -0.175, 0.175), (0.105, 0.095, 0.105), fur_light)
    sphere("belly", (0, -0.03, 0.115), (0.10, 0.155, 0.085), belly)
    sphere("tail", (0, 0.255, 0.185), (0.055, 0.05, 0.055), tail)

    # Head, muzzle, nose and eyes.
    sphere("head", (0, -0.265, 0.255), (0.093, 0.10, 0.092), fur_light)
    sphere("muzzle", (0, -0.335, 0.225), (0.055, 0.06, 0.048), belly)
    sphere("nose", (0, -0.392, 0.225), (0.022, 0.018, 0.018), pink)
    for s in (-1, 1):
        sphere("eye", (s * 0.060, -0.330, 0.282), (0.017, 0.014, 0.017),
               dark, segments=8, rings=4)
        # Ears: tall cones leaning slightly back, with a pink inner cone.
        cone("ear", (s * 0.048, -0.235, 0.425), 0.035, 0.012, 0.24, fur,
             vertices=8, rot=(-10, 0, s * 8))
        cone("ear_inner", (s * 0.048, -0.252, 0.420), 0.022, 0.007, 0.19,
             pink, vertices=8, rot=(-10, 0, s * 8))

    # Four feet, front pair narrow, hind pair wide.
    for s in (-1, 1):
        box("front_leg", (s * 0.072, -0.185, 0.045),
            (0.052, 0.075, 0.09), fur, bevel=0.012)
        box("hind_leg", (s * 0.088, 0.115, 0.055),
            (0.06, 0.11, 0.11), fur, bevel=0.014)
        box("hind_foot", (s * 0.088, 0.035, 0.020),
            (0.06, 0.17, 0.04), fur_light, bevel=0.010)


def build_bird():
    body = material("bird_body", (0.10, 0.12, 0.15))
    wing = material("bird_wing", (0.06, 0.07, 0.10))
    belly = material("bird_belly", (0.34, 0.36, 0.34))
    beak = material("bird_beak", (0.72, 0.52, 0.16))
    dark = material("bird_eye", (0.03, 0.03, 0.03), roughness=0.4)

    sphere("body", (0, 0.01, 0.115), (0.070, 0.155, 0.072), body)
    sphere("belly", (0, -0.01, 0.090), (0.052, 0.115, 0.045), belly)
    sphere("head", (0, -0.155, 0.155), (0.056, 0.058, 0.052), body)
    cone("beak", (0, -0.215, 0.148), 0.022, 0.002, 0.075, beak,
         vertices=8, rot=(90, 0, 0))
    for s in (-1, 1):
        sphere("eye", (s * 0.040, -0.180, 0.170), (0.010, 0.009, 0.010),
               dark, segments=6, rings=4)

    # Tail fan.
    box("tail", (0, 0.190, 0.110), (0.105, 0.135, 0.016), wing,
        bevel=0.006, rot=(-8, 0, 0))

    # Two swept wings with a raised tip; wingspan ~0.62 m.
    for s in (-1, 1):
        box("wing_inner", (s * 0.125, -0.005, 0.128),
            (0.200, 0.105, 0.018), wing, bevel=0.006,
            rot=(0, s * -7, s * 7))
        box("wing_tip", (s * 0.285, 0.045, 0.160),
            (0.155, 0.075, 0.014), wing, bevel=0.005,
            rot=(0, s * -13, s * 14))


def build_duck():
    body = material("duck_body", (0.25, 0.18, 0.10))
    wing = material("duck_wing", (0.18, 0.13, 0.08))
    head = material("duck_head", (0.05, 0.19, 0.11))
    ring = material("duck_ring", (0.56, 0.55, 0.48))
    beak = material("duck_beak", (0.80, 0.58, 0.16))
    dark = material("duck_eye", (0.03, 0.03, 0.03), roughness=0.4)
    feet = material("duck_feet", (0.72, 0.40, 0.12))

    sphere("body", (0, 0.015, 0.110), (0.105, 0.205, 0.100), body)
    sphere("breast", (0, -0.155, 0.120), (0.085, 0.085, 0.080), ring)
    sphere("tail", (0, 0.215, 0.130), (0.055, 0.080, 0.045), wing)
    beam("neck", (0, -0.150, 0.170), (0, -0.180, 0.280), 0.042, head,
         vertices=8)
    sphere("head", (0, -0.190, 0.315), (0.062, 0.070, 0.064), head)
    box("beak", (0, -0.262, 0.300), (0.065, 0.085, 0.032), beak,
        bevel=0.012, rot=(-6, 0, 0))
    sphere("ring", (0, -0.182, 0.258), (0.055, 0.055, 0.018), ring)
    for s in (-1, 1):
        sphere("eye", (s * 0.045, -0.232, 0.338), (0.011, 0.010, 0.011),
               dark, segments=6, rings=4)
        sphere("wing_fold", (s * 0.082, 0.020, 0.135),
               (0.035, 0.145, 0.062), wing)
        box("foot", (s * 0.055, -0.010, 0.012), (0.055, 0.10, 0.024), feet,
            bevel=0.006)


def build_deer():
    fur = material("deer_fur", (0.26, 0.17, 0.09))
    fur_light = material("deer_fur_light", (0.34, 0.23, 0.14))
    belly = material("deer_belly", (0.60, 0.53, 0.43))
    dark = material("deer_dark", (0.14, 0.09, 0.05))
    antler = material("deer_antler", (0.45, 0.38, 0.27))
    eye = material("deer_eye", (0.03, 0.025, 0.02), roughness=0.35)

    # Torso, chest, rump and white belly.
    sphere("torso", (0, 0.0, 0.80), (0.235, 0.50, 0.275), fur)
    sphere("chest", (0, -0.32, 0.82), (0.225, 0.235, 0.255), fur_light)
    sphere("rump", (0, 0.33, 0.80), (0.215, 0.235, 0.245), fur)
    sphere("belly", (0, -0.02, 0.60), (0.185, 0.40, 0.115), belly)
    box("tail", (0, 0.52, 0.82), (0.055, 0.09, 0.16), belly,
        bevel=0.012, rot=(-22, 0, 0))

    # Neck and head.
    beam("neck", (0, -0.36, 0.93), (0, -0.60, 1.19), 0.125, fur,
         vertices=8)
    sphere("head", (0, -0.65, 1.235), (0.105, 0.155, 0.115), fur_light)
    sphere("muzzle", (0, -0.795, 1.195), (0.060, 0.075, 0.055), fur)
    sphere("nose", (0, -0.862, 1.200), (0.028, 0.022, 0.022), dark)
    for s in (-1, 1):
        cone("ear", (s * 0.105, -0.60, 1.335), 0.055, 0.012, 0.16, fur,
             vertices=8, rot=(0, 0, s * 38))
        sphere("eye", (s * 0.085, -0.755, 1.270), (0.019, 0.016, 0.019),
               eye, segments=6, rings=4)
        # Simple forked antlers.
        beam("antler_main", (s * 0.060, -0.62, 1.310),
             (s * 0.150, -0.56, 1.560), 0.022, antler, vertices=6)
        beam("antler_tine_a", (s * 0.100, -0.590, 1.420),
             (s * 0.100, -0.66, 1.535), 0.014, antler, vertices=5)
        beam("antler_tine_b", (s * 0.125, -0.575, 1.480),
             (s * 0.205, -0.60, 1.590), 0.013, antler, vertices=5)

    # Four slim legs with dark hooves.
    for sx in (-1, 1):
        for sy in (-1, 1):
            x = sx * 0.155
            y = sy * 0.33
            beam("leg", (x, y, 0.62), (x * 1.05, y * 1.02, 0.10), 0.045,
                 fur, vertices=6)
            box("hoof", (x * 1.05, y * 1.02, 0.045), (0.085, 0.11, 0.09),
                dark, bevel=0.008)


def main():
    clear_all()
    build_rabbit()
    finish("RV_Rabbit", "rv_rabbit.gltf")

    clear_all()
    build_bird()
    finish("RV_Bird", "rv_bird.gltf")

    clear_all()
    build_duck()
    finish("RV_Duck", "rv_duck.gltf")

    clear_all()
    build_deer()
    finish("RV_Deer", "rv_deer.gltf")


if __name__ == "__main__":
    main()
