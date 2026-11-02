-- lc_util.lua
-- 通用工具：格坐标/旋转变换/经济/颜色/节点查询

function cellIndex(cx, cz) return cz * GW + cx end

-- 世界点 -> 房车局部（逆旋转 -yaw）再落格：房车停在任意位置/朝向都能放置
function worldToCell(x, z)
  local dx, dz = x - rv.x, z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local lx = dx * cy - dz * sy
  local lz = dx * sy + dz * cy
  local cx = math.floor((lx - ORIGIN_X) / CELL)
  local cz = math.floor((lz - ORIGIN_Z) / CELL)
  if cx < 0 or cx >= GW or cz < 0 or cz >= GH then return nil end
  return cx, cz
end

-- 房车局部点 -> 世界坐标（前进 = (sin yaw, cos yaw)）
function rotOf(lx, lz)
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  return rv.x + lx * cy + lz * sy, rv.z - lx * sy + lz * cy
end

-- 世界点 -> 房车局部坐标（自由摆放用）
function worldToLocal(x, z)
  local dx, dz = x - rv.x, z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  return dx * cy - dz * sy, dx * sy + dz * cy
end

-- 旋转后的轴对齐半宽/半长（家具自由旋转的粗略碰撞盒）
function freeExtents(cat, rotY)
  local hx, hz = (cat.w or 1) * CELL * 0.5, (cat.h or 1) * CELL * 0.5
  local c, s = math.abs(math.cos(rotY or 0)), math.abs(math.sin(rotY or 0))
  return c * hx + s * hz, s * hx + c * hz
end

function instanceExtents(inst)
  return freeExtents(inst.cat, inst.lrot or 0)
end

-- 自由摆放合法性：车内边界 + 同层家具 AABB 不重叠
function freeFits(cat, lx, lz, rotY, ignore)
  local ex, ez = freeExtents(cat, rotY)
  if math.abs(lx) + ex > 1.02 or math.abs(lz) + ez > 3.48 then return false end
  for _, inst in pairs(instances) do
    if inst ~= ignore and inst.layer == cat.layer then
      local iex, iez = instanceExtents(inst)
      if math.abs(lx - inst.lx) < ex + iex - 0.035 and
         math.abs(lz - inst.lz) < ez + iez - 0.035 then
        return false
      end
    end
  end
  return true
end

-- 方向向量 -> yaw（前进 = (sin yaw, cos yaw)）
function dirYaw(dx, dz)
  if dz > 0 then return math.atan(dx / dz)
  elseif dz < 0 then return math.atan(dx / dz) + (dx >= 0 and math.pi or -math.pi)
  else return (dx >= 0) and (math.pi * 0.5) or (-math.pi * 0.5) end
end

function footprint(cat, r)
  if r % 2 == 1 then return cat.h, cat.w end
  return cat.w, cat.h
end

function cellsOf(cat, cx, cz, r)
  local w, h = footprint(cat, r)
  local cells = {}
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      local x, z = cx + dx, cz + dz
      if x < 0 or x >= GW or z < 0 or z >= GH then return nil end
      cells[#cells + 1] = { x = x, z = z }
    end
  end
  return cells
end

function occGrid(layer)
  return layer == "roof" and roofOcc or occupancy
end

function fits(cat, cx, cz, r)
  local cells = cellsOf(cat, cx, cz, r)
  if not cells then return false end
  local occ = occGrid(cat.layer)
  for _, c in ipairs(cells) do
    if occ[cellIndex(c.x, c.z)] ~= nil then return false end
  end
  return true
end

function findInstanceAt(cx, cz)
  local id = occupancy[cellIndex(cx, cz)]
  return id and instances[id] or nil
end

-- POI building footprint (all non-camp nodes spawn the same base shell at
-- node + (6.5, 6.5)). Shared by collision, loot placement and co-op demos.
POI_HALF_X, POI_HALF_Z = 3.05, 2.55
POI_DOOR_HALF = 0.72

function poiCenter(nd)
  if nd == nil then return nil end
  return nd.x + 6.5, nd.z + 6.5
end

-- Axis-separated wall collision for the POI shell: side/back walls always
-- block, the front wall only blocks outside the doorway span. Applied to both
-- local prediction and the authoritative server so walls feel solid online.
function collidePoiWalls(px, pz, nx, nz)
  local margin = 0.14
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then
      local cx, cz = poiCenter(nd)
      local hx, hz = POI_HALF_X, POI_HALF_Z

      -- Side walls.
      if pz > cz - hz - margin and pz < cz + hz + margin then
        local left, right = cx - hx, cx + hx
        if px >= left then
          if nx < left + margin then nx = left + margin end
        elseif nx > left - margin then
          nx = left - margin
        end
        if px <= right then
          if nx > right - margin then nx = right - margin end
        elseif nx < right + margin then
          nx = right + margin
        end
      end

      -- Back wall.
      if px > cx - hx - margin and px < cx + hx + margin then
        local back = cz - hz
        if pz >= back then
          if nz < back + margin then nz = back + margin end
        elseif nz > back - margin then
          nz = back - margin
        end
      end

      -- Front wall, split around the doorway.
      if px > cx - hx - margin and px < cx + hx + margin then
        local front = cz + hz
        if math.abs(nx - cx) > POI_DOOR_HALF then
          if pz <= front then
            if nz > front - margin then nz = front - margin end
          elseif nz < front + margin then
            nz = front + margin
          end
        end
      end
    end
  end
  return nx, nz
end

function canAfford(cost)
  for k, v in pairs(cost) do
    if (materials[k] or 0) < v then return false end
  end
  return true
end

function payCost(cost)
  for k, v in pairs(cost) do materials[k] = (materials[k] or 0) - v end
end

function refundCost(cost)
  for k, v in pairs(cost) do materials[k] = (materials[k] or 0) + v end
end

function costText(cost)
  local parts = {}
  for k, v in pairs(cost) do parts[#parts + 1] = MAT_NAMES[k] .. "x" .. v end
  return table.concat(parts, " ")
end

function hexColor(s)
  local n = tonumber(string.sub(s, 2), 16)
  if not n then return 1, 1, 1 end
  local b = n % 256
  local g = math.floor(n / 256) % 256
  local r = math.floor(n / 65536) % 256
  return r / 255, g / 255, b / 255
end

function nodeById(id)
  for _, nd in ipairs(NODES) do
    if nd.id == id then return nd end
  end
  return nil
end

function hasModule(id)
  for _, inst in pairs(instances) do
    if inst.cat.id == id then return inst end
  end
  return nil
end

function nodeName(id)
  local nd = nodeById(id)
  return nd and nd.name or "荒野"
end
