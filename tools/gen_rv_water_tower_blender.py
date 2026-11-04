"""Blender-authored town water tower, generated through Blender MCP.

Origin is the footprint centre at ground level. Intended bounding footprint:
~3.3 x 3.3 m, tank top around y=7.0.
"""
from __future__ import annotations

import math
from pathlib import Path

import bpy
import bmesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "projects/rv/assets/models/rv/rv_water_tower.gltf"

PARTS = []


def material(name, rgb, roughness=0.8, metallic=0.0):
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


def cyl(name, loc, radius, depth, mat, vertices=20, rot=(0, 0, 0)):
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


def cone(name, loc, r1, r2, depth, mat, vertices=20):
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=r1, radius2=r2,
                                    depth=depth, location=loc)
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.data.materials.append(mat)
    PARTS.append(ob)
    return ob


def main():
    # Clear current objects only; the MCP addon owns this scene.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    PARTS.clear()

    steel = material("water_tower_steel", (0.34, 0.42, 0.46), roughness=0.55,
                     metallic=0.55)
    steel_dark = material("water_tower_steel_dark", (0.19, 0.23, 0.25),
                          roughness=0.6, metallic=0.5)
    tank = material("water_tower_tank", (0.44, 0.56, 0.60), roughness=0.45,
                    metallic=0.35)
    roof = material("water_tower_roof", (0.20, 0.24, 0.25), roughness=0.7,
                    metallic=0.25)
    stone = material("water_tower_stone", (0.50, 0.48, 0.43), roughness=0.95)
    rust = material("water_tower_rust", (0.42, 0.27, 0.14), roughness=0.9)
    pipe_mat = material("water_tower_pipe", (0.28, 0.32, 0.30), roughness=0.7,
                        metallic=0.4)

    # Footings + four steel legs.
    for x in (-1.15, 1.15):
        for y in (-1.15, 1.15):
            box("footing", (x, y, 0.18), (0.55, 0.55, 0.30), stone)
            box("leg", (x, y, 2.35), (0.17, 0.17, 4.45), steel)

    # Level belts and diagonal cross-bracing.
    for z in (1.35, 2.55, 3.75):
        box("belt_x_front", (0, 1.05, z), (2.20, 0.10, 0.12), steel_dark)
        box("belt_x_back", (0, -1.05, z), (2.20, 0.10, 0.12), steel_dark)
        box("belt_y_left", (-1.05, 0, z), (0.10, 2.20, 0.12), steel_dark)
        box("belt_y_right", (1.05, 0, z), (0.10, 2.20, 0.12), steel_dark)
    for s in (-1, 1):
        box("brace_x", (0, s * 1.05, 2.0), (0.09, 0.09, 2.55), steel_dark,
            rot=(0, 42, 0))
        box("brace_x", (0, s * 1.05, 3.3), (0.09, 0.09, 2.0), steel_dark,
            rot=(0, -42, 0))
        box("brace_y", (s * 1.05, 0, 2.0), (0.09, 0.09, 2.55), steel_dark,
            rot=(42, 0, 0))
        box("brace_y", (s * 1.05, 0, 3.3), (0.09, 0.09, 2.0), steel_dark,
            rot=(-42, 0, 0))

    # Service platform and railing.
    box("platform", (0, 0, 4.55), (2.85, 2.85, 0.16), steel)
    for x in (-1.25, 1.25):
        for y in (-1.25, 1.25):
            box("rail_post", (x, y, 5.12), (0.09, 0.09, 1.08), steel_dark)
    box("rail_front", (0, 1.25, 5.58), (2.55, 0.08, 0.10), steel_dark)
    box("rail_back", (0, -1.25, 5.58), (2.55, 0.08, 0.10), steel_dark)
    box("rail_left", (-1.25, 0, 5.58), (0.08, 2.55, 0.10), steel_dark)
    box("rail_right", (1.25, 0, 5.58), (0.08, 2.55, 0.10), steel_dark)

    # Tank body, seam bands, conical roof and hatch.
    cyl("tank", (0, 0, 6.25), 1.55, 2.35, tank, vertices=28)
    for z in (5.20, 6.28, 7.30):
        cyl("tank_band", (0, 0, z), 1.59, 0.075, steel_dark, vertices=28)
    cone("tank_roof", (0, 0, 7.72), 1.66, 0.16, 0.72, roof, vertices=28)
    cyl("roof_hatch", (0, 0, 8.15), 0.24, 0.18, rust, vertices=8)

    # Ladder up the left side to the platform / tank band.
    for x in (-1.40, -1.08):
        box("ladder_rail", (x, 0.98, 2.85), (0.07, 0.07, 5.70), steel_dark)
    for i in range(13):
        z = 0.35 + i * 0.42
        box("rung", (-1.24, 0.98, z), (0.40, 0.06, 0.05), rust)

    # Down pipe with a foot elbow.
    cyl("down_pipe", (-1.08, -0.78, 3.35), 0.11, 6.30, pipe_mat, vertices=10)
    box("pipe_elbow", (-0.78, -0.78, 0.22), (0.72, 0.18, 0.18), pipe_mat)
    box("pipe_foot", (-0.42, -0.78, 0.10), (0.28, 0.28, 0.14), stone)

    # Small warning plate on the tank.
    box("warning_plate", (0, -1.57, 5.75), (0.72, 0.04, 0.46), rust,
        bevel=0.01)

    # Join and fix outward normals.
    bpy.ops.object.select_all(action="DESELECT")
    for ob in PARTS:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = PARTS[0]
    bpy.ops.object.join()
    merged = bpy.context.view_layer.objects.active
    merged.name = "RV_WaterTower"
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
