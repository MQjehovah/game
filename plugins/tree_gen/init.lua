-- 程序化树木生成器 (editor plugin example, Lua)
--
-- Registers a dockable editor panel that builds a procedural tree mesh
-- (cylinder trunk + cone crown), writes it as an OBJ asset and spawns it
-- into the scene at the selected entity's position (undoable).

local treeHeight = 6.0
local crownSize = 3.0
local trunkRadius = 0.35
local meshName = "tree_oak"

-- Appends a ring of vertices at (cy, radius) and returns the 1-based index of
-- the first vertex (Lua arrays are 1-based; OBJ faces are 1-based too).
local function ring(verts, cy, r, segs)
  local first = #verts + 1
  for i = 0, segs - 1 do
    local a = i / segs * 2 * math.pi
    verts[#verts + 1] = { x = math.cos(a) * r, y = cy, z = math.sin(a) * r }
  end
  return first
end

local function buildTree(h, crown, radius)
  local verts = {}
  local idx = {}
  local segs = 10
  local trunkTop = h * 0.55
  local b0 = ring(verts, 0.0, radius, segs)
  local b1 = ring(verts, trunkTop, radius * 0.72, segs)
  -- Trunk side quads (two triangles each).
  for i = 0, segs - 1 do
    local n = i % segs + 1
    idx[#idx + 1] = b0 + i
    idx[#idx + 1] = b1 + i
    idx[#idx + 1] = b1 + n
    idx[#idx + 1] = b0 + i
    idx[#idx + 1] = b1 + n
    idx[#idx + 1] = b0 + n
  end
  -- Crown: base ring + apex cone.
  local cb = ring(verts, trunkTop, crown, segs)
  verts[#verts + 1] = { x = 0, y = h, z = 0 }
  local apex = #verts
  for i = 0, segs - 1 do
    local n = i % segs + 1
    idx[#idx + 1] = cb + i
    idx[#idx + 1] = apex
    idx[#idx + 1] = cb + n
  end
  return verts, idx
end

function on_load()
  NeonEditor.panel("tree_gen", "树木生成器", function()
    NeonEditor.ui.SliderFloat("树高", treeHeight, 2, 20)
    NeonEditor.ui.SliderFloat("冠幅", crownSize, 1, 12)
    NeonEditor.ui.SliderFloat("树干半径", trunkRadius, 0.1, 1.5)
    NeonEditor.ui.InputText("网格名称", meshName)
    NeonEditor.ui.Separator()
    if NeonEditor.ui.Button("生成到场景") then
      local verts, idx = buildTree(treeHeight, crownSize, trunkRadius)
      local key = NeonEditor.buildMesh(meshName, verts, idx)
      if key ~= "" then
        local x, y, z = 0, 0, 0
        local sel = NeonEditor.selected()
        if sel ~= nil then x, y, z = sel.x, sel.y, sel.z end
        NeonEditor.spawn(key, x, y, z)
        NeonEditor.log("已生成树木 '" .. meshName .. "' 并放入场景")
      end
    end
  end)
  NeonEditor.tool("tree_gen", "生成树", function()
    local verts, idx = buildTree(treeHeight, crownSize, trunkRadius)
    local key = NeonEditor.buildMesh(meshName, verts, idx)
    if key ~= "" then NeonEditor.spawn(key, 0, 0, 0) end
  end)
end
