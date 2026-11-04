import bpy

bpy.ops.preferences.addon_enable(module="blender_mcp")
scene = bpy.context.scene
scene.blendermcp_port = 9876
scene.blendermcp_auto_start_server = True
try:
    bpy.ops.blendermcp.start_server()
    print("BLENDER_MCP_READY 9876", flush=True)
except Exception as exc:
    print("BLENDER_MCP_START_FAILED", repr(exc), flush=True)
