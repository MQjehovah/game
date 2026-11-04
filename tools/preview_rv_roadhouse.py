import bpy
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "projects/rv/assets/models/rv/rv_roadhouse.gltf"
OUT = Path(r"C:\Users\mqjeh\AppData\Local\Temp\opencode\rv_roadhouse_preview.png")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(MODEL))
# Camera front-left, slightly above.
bpy.ops.object.camera_add(location=(-7.0, -11.0, 5.2))
cam = bpy.context.object
cam.rotation_euler = (math.radians(74), 0, math.radians(-32))
bpy.context.scene.camera = cam
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "MATERIAL"
scene.display.shading.show_shadows = True
scene.display.shading.show_cavity = True
scene.render.resolution_x = 960
scene.render.resolution_y = 720
scene.render.resolution_percentage = 100
scene.render.filepath = str(OUT)
bpy.ops.render.render(write_still=True)
print(f"wrote {OUT}")
