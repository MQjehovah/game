-- lc_vehicle.lua
-- 房车装配：车体部件/模块/贴花随车位姿整体变换

function collectRvParts()
  if rvParts then return end
  rvParts = {}
  local names = { "RV_Floor", "RV_WallL", "RV_WallR", "RV_RearWall", "RV_Cab",
                  "RV_Windshield" }
  for _, n in ipairs(names) do
    local e = FindNamedEntity(n)
    if e ~= nil then
      local p = GetPosition(e)
      if n == "RV_Cab" then SetScale(e, 2.6, 1.8, 1.7) end  -- 与车身同宽
      -- 外壳换成 glTF 模型；旧盒体隐藏，仅保留地板
      if n ~= "RV_Floor" then
        SetVisible(e, false)
      end
      rvParts[#rvParts + 1] = { ent = e, name = n, lx = p.x, ly = p.y, lz = p.z }
    end
  end
end

-- 房车外壳是单面立方体：从车内看到的是背面（被剔除=黑）。
-- 这里补一层朝内的墙板 + 车窗外饰，第一人称才有像样的车舱。
local INTERIOR = {
  -- 内墙板
  { pre = "rv_panel_light", lx = -0.99, ly = 1.13, lz = 0.0, sx = 0.02, sy = 2.16, sz = 7.86 },
  -- 右侧内墙在车门处留出真实开口（上/后/前三块），否则打开车门只能看到内墙板
  { pre = "rv_panel_light", lx =  0.99, ly = 2.035, lz = 0.0, sx = 0.02, sy = 0.35, sz = 7.86 },
  { pre = "rv_panel_light", lx =  0.99, ly = 0.955, lz = -3.735, sx = 0.02, sy = 1.81, sz = 0.39 },
  { pre = "rv_panel_light", lx =  0.99, ly = 0.955, lz = 0.585, sx = 0.02, sy = 1.81, sz = 6.69 },
  { pre = "rv_panel_light", lx =  0.0,  ly = 1.13, lz = -3.92, sx = 2.24, sy = 2.16, sz = 0.04 },
  -- 驾驶台下半部（上半部留空当挡风玻璃）
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 0.52, lz = 3.94, sx = 2.24, sy = 1.02, sz = 0.06 },
  -- 驾驶台顶沿 + A 柱（驾驶席视角的取景框）
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 1.04, lz = 4.16, sx = 2.24, sy = 0.08, sz = 0.42 },
  { pre = "rv_panel_dark",  lx = -1.06, ly = 1.55, lz = 4.18, sx = 0.10, sy = 1.12, sz = 0.10 },
  { pre = "rv_panel_dark",  lx =  1.06, ly = 1.55, lz = 4.18, sx = 0.10, sy = 1.12, sz = 0.10 },
  -- 地板上的深色地毯
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 0.058, lz = -1.0, sx = 1.45, sy = 0.012, sz = 2.1 },
  -- 车顶内饰板（挡天空，配合暖光做出车舱感）
  { pre = "rv_panel_light", lx = 0.0, ly = 2.12, lz = 0.0, sx = 2.24, sy = 0.04, sz = 7.86, hideInCab = true },
  -- 驾驶室顶（保留挡风玻璃上方的开口）
  { pre = "rv_panel_light", lx = 0.0, ly = 2.14, lz = 4.75, sx = 2.24, sy = 0.04, sz = 1.9, hideInCab = true },
  { pre = "rv_lamp", lx = 0.0, ly = 2.02, lz = -1.6, sx = 0.16, sy = 0.05, sz = 0.16 },
  { pre = "rv_lamp", lx = 0.0, ly = 2.02, lz = 1.7,  sx = 0.16, sy = 0.05, sz = 0.16 },
}

function spawnRvShell()
  if rvShell then return end
  rvShell = SpawnPrefab("rv_body", { x = 0, y = 0, z = 0 })
  -- The side door is a separate mesh (hinged at the rear edge) so it can swing
  -- open without a cube overlay.
  rvDoor = SpawnPrefab("rv_door", { x = 0, y = 0, z = 0 })
  if rvDoor ~= nil then SetRotationY(rvDoor, rv.yaw) end
end

-- Four standalone wheels: each follows its own terrain contact (vertical
-- suspension travel) and the front pair steers. Local layout matches the old
-- baked wheels and the wheel arches.
local WHEEL_DEFS = {
  { lx = -1.26, lz =  2.92, front = true },   -- front left
  { lx =  1.26, lz =  2.92, front = true },   -- front right
  { lx = -1.26, lz = -2.92, front = false },  -- rear left
  { lx =  1.26, lz = -2.92, front = false },  -- rear right
}
local WHEEL_CENTER_Y = 0.38
local WHEEL_RADIUS = 0.40

function spawnRvWheels()
  if rvWheels then return end
  rvWheels = {}
  for i, w in ipairs(WHEEL_DEFS) do
    local ent = SpawnPrefab("rv_wheel", { x = rv.x, y = WHEEL_CENTER_Y, z = rv.z })
    if ent ~= nil then
      rvWheels[i] = { ent = ent, lx = w.lx, lz = w.lz, front = w.front }
    end
  end
end

function spawnInterior()
  if rvInterior then return end
  rvInterior = {}
  for _, d in ipairs(INTERIOR) do
    local ent = SpawnPrefab(d.pre, { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      SetScale(ent, d.sx, d.sy, d.sz)
      rvInterior[#rvInterior + 1] = {
        ent = ent, pre = d.pre, lx = d.lx, ly = d.ly, lz = d.lz,
        sx = d.sx, sy = d.sy, sz = d.sz, hideInCab = d.hideInCab,
      }
    end
  end
end

-- 车内灯开关（L）：rv_lamp 带点光，SetVisible 只能隐藏网格、灯还亮着，
-- 所以真正熄灭要 Despawn，再开时按原局部坐标重新生成。
function setInteriorLights(on)
  if rvInterior == nil then return end
  for _, p in ipairs(rvInterior) do
    if p.pre == "rv_lamp" then
      if on and p.ent == nil then
        local e = SpawnPrefab("rv_lamp", { x = rv.x, y = -60, z = rv.z })
        if e ~= nil then
          SetScale(e, p.sx or 0.16, p.sy or 0.05, p.sz or 0.16)
          p.ent = e
        end
      elseif not on and p.ent ~= nil then
        Despawn(p.ent)
        p.ent = nil
      end
    end
  end
  interiorLightsOn = on and true or false
end

-- 公路贴花：跟随房车，按里程滚动虚线（Nomad Drive 的公路感）
-- 公路现在是世界固定的，由 spawnRoadNetwork() 一次性生成，不再每帧跟随车头。

-- Full 3D rotation of an RV-local offset by roll -> pitch -> yaw. Mirrors the
-- engine SetRotationEuler order (q = yaw * pitch * roll).
function rvOffset(lx, ly, lz)
  local cr, sr = math.cos(rv.roll or 0), math.sin(rv.roll or 0)
  local x1 = lx * cr - ly * sr
  local y1 = lx * sr + ly * cr
  local z1 = lz
  local cp, sp = math.cos(rv.pitch or 0), math.sin(rv.pitch or 0)
  local y2 = y1 * cp - z1 * sp
  local z2 = y1 * sp + z1 * cp
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  return x1 * cy + z2 * sy, y2, -x1 * sy + z2 * cy
end

-- Height of the tilted floor at an RV-local (lx, lz), relative to the RV
-- origin. The first-person player inside the RV stands on this surface, so the
-- eye follows the lean instead of floating through the wall on a slope.
function rvFloorOffset(lx, lz)
  local cp, sp = math.cos(rv.pitch or 0), math.sin(rv.pitch or 0)
  local sr = math.sin(rv.roll or 0)
  return lx * sr * cp - lz * sp
end

local function setTilted(ent, lx, ly, lz, extraYaw)
  local ox, oy, oz = rvOffset(lx, ly, lz)
  SetPosition(ent, { x = rv.x + ox, y = (rv.groundY or 0) + oy, z = rv.z + oz })
  local pitch, roll = rv.pitch or 0, rv.roll or 0
  local yaw = rv.yaw + (extraYaw or 0)
  if SetRotationEuler ~= nil then
    SetRotationEuler(ent, pitch, roll, yaw)
  else
    SetRotationY(ent, yaw)
  end
end

function applyRvTransform()
  local gy = rv.groundY or 0
  if rvParts then
    for _, p in ipairs(rvParts) do
      setTilted(p.ent, p.lx, p.ly, p.lz, 0)
    end
  end
  if rvShell then
    setTilted(rvShell, 0, 0, 0, 0)
    -- 驾驶席视角或人在车内时隐藏外壳：避免镜头钻进车窗/墙板
    SetVisible(rvShell, not (phase == "driving" and driveView == "cab")
                        and not isPlayerInsideRv())
  end
  if rvDoor ~= nil then
    setTilted(rvDoor, 1.205, 0, -3.54, rvDoorAngle or 0)
  end
  if rvInterior then
    local hideCab = (phase == "driving" and driveView == "cab")
    for _, p in ipairs(rvInterior) do
      if p.ent ~= nil then
        setTilted(p.ent, p.lx, p.ly, p.lz, 0)
        if p.hideInCab then SetVisible(p.ent, not hideCab) end
      end
    end
  end
  -- Wheels: use the per-wheel rigid body pose when available (real suspension
  -- travel), otherwise place them from the fitted plane + travel.
  if rvWheels then
    local s = rv.susp
    for i, w in ipairs(rvWheels) do
      local wb = rv.wheelBodies and rv.wheelBodies[i]
      local bp = wb and PhysicsGetPosition and PhysicsGetPosition(wb.body)
      if bp ~= nil then
        SetPosition(w.ent, { x = bp.x, y = bp.y, z = bp.z })
      else
        local travel, plane = 0, 0
        if s ~= nil and s.travel ~= nil then
          travel = s.travel[i] or 0
          plane = (s.plane and s.plane[i]) or 0
        end
        local ly = WHEEL_CENTER_Y + plane + travel
        local ox, oy, oz = rvOffset(w.lx, ly, w.lz)
        SetPosition(w.ent, { x = rv.x + ox, y = gy + oy, z = rv.z + oz })
      end
      local steer = 0
      if w.front then
        steer = (i == 1) and (rv.steerLeft or rv.steerVis or 0)
                          or (rv.steerRight or rv.steerVis or 0)
      end
      local spin = (rv.wheelSpinI and rv.wheelSpinI[i]) or rv.wheelSpin or 0
      local pitch = (rv.pitch or 0) + spin
      if SetRotationEuler ~= nil then
        SetRotationEuler(w.ent, pitch, rv.roll or 0, rv.yaw + steer)
      else
        SetRotationY(w.ent, rv.yaw + steer)
      end
    end
  end
  for _, inst in pairs(instances) do
    setTilted(inst.ent, inst.lx, inst.ly, inst.lz, inst.lrot or 0)
  end
  if gridFloorEnt ~= nil then
    setTilted(gridFloorEnt, 0, FLOOR_Y, 0, 0)
  end
  if gridRoofEnt ~= nil then
    setTilted(gridRoofEnt, 0, ROOF_Y, 0, 0)
  end
end

-- ===========================================================================
-- 刹车/倒车尾灯：车尾两块红色自发光，刹车或倒车时点亮。
-- 常驻实体（只是自发光网格，不占灯光槽），SetVisible 切换。
-- ===========================================================================
local TAILLIGHTS = {
  { -0.66, 1.02, -4.02 },
  {  0.66, 1.02, -4.02 },
}

function spawnRvTaillights()
  if rvParts == nil or #rvTaillights > 0 then return end
  for _, t in ipairs(TAILLIGHTS) do
    local e = SpawnPrefab("rv_taillight", { x = rv.x, y = -60, z = rv.z })
    if e ~= nil then
      SetScale(e, 0.36, 0.15, 0.05)
      local part = { ent = e, name = "taillight", lx = t[1], ly = t[2], lz = t[3] }
      rvParts[#rvParts + 1] = part
      rvTaillights[#rvTaillights + 1] = e
      setTilted(e, part.lx, part.ly, part.lz, 0)
      SetVisible(e, false)
    end
  end
  rv.brakeLit = false
end

function updateTaillights()
  if #rvTaillights == 0 then return end
  local braking = (rv.brakeTest == true) or (rv.braking == true) or
                  ((rv.speed or 0) < -0.05)
  if braking == rv.brakeLit then return end
  rv.brakeLit = braking
  for _, e in ipairs(rvTaillights) do SetVisible(e, braking) end
end

-- ===========================================================================
-- 夜间前大灯：两盏聚光灯挂在车头，随车身旋转；白天/熄火时整个实体移除，
-- 避免占用引擎有限的点光/聚光槽，也避免白天出现两道亮斑。
-- ===========================================================================
local HEADLIGHTS = {
  { -0.78, 0.96, 4.26 },
  {  0.78, 0.96, 4.26 },
}

function setRvHeadlights(on)
  if rvParts == nil then return end
  if on then
    if #rvHeadlights > 0 then return end
    for _, h in ipairs(HEADLIGHTS) do
      local e = SpawnPrefab("rv_headlight", { x = rv.x, y = -60, z = rv.z })
      if e ~= nil then
        SetScale(e, 0.24, 0.17, 0.07)
        local part = { ent = e, name = "headlight", lx = h[1], ly = h[2], lz = h[3] }
        rvParts[#rvParts + 1] = part
        rvHeadlights[#rvHeadlights + 1] = part
        setTilted(e, part.lx, part.ly, part.lz, 0)
      end
    end
  else
    if #rvHeadlights == 0 then return end
    for _, p in ipairs(rvHeadlights) do
      Despawn(p.ent)
      for i = #rvParts, 1, -1 do
        if rvParts[i] == p then table.remove(rvParts, i) break end
      end
    end
    rvHeadlights = {}
  end
end

-- driving: 单机用 phase，联机由调用方传入“是否有人在开/车在动”
function updateVehicleLights(driving)
  local want = false
  if isNight ~= nil and isNight() then
    if driving == nil then
      want = (phase == "driving")
    else
      want = driving and true or false
    end
  end
  if rv.forceHeadlights then want = true end
  setRvHeadlights(want)
end

-- ===========================================================================
-- 夜间步行手电：一盏跟随视角的聚光灯，让夜里下车探索/搜刮看得见。
-- 白天或回到车内自动移除，不占聚光槽。
-- ===========================================================================
function setFlashlight(on)
  if on then
    if flashlightEnt ~= nil then return end
    local e = SpawnPrefab("rv_flashlight", { x = 0, y = -60, z = 0 })
    if e ~= nil then
      SetScale(e, 0.10, 0.10, 0.06)
      -- 光源离眼睛太近，网格会糊住整个视野；只保留 light 组件。
      SetVisible(e, false)
      flashlightEnt = e
    end
  else
    if flashlightEnt ~= nil then Despawn(flashlightEnt) end
    flashlightEnt = nil
  end
end

function updateFlashlight()
  local want = flashlightOn and onFoot and player.active and
               (isNight ~= nil and isNight())
  if not want then
    if flashlightEnt ~= nil then setFlashlight(false) end
    return
  end
  setFlashlight(true)
  if flashlightEnt ~= nil then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    SetPosition(flashlightEnt, {
      x = player.x + fx * 0.35,
      y = (player.groundY or 0) + 1.55,
      z = player.z + fz * 0.35 })
    if SetRotationEuler ~= nil then
      SetRotationEuler(flashlightEnt, player.pitch or 0, 0, player.yaw)
    else
      SetRotationY(flashlightEnt, player.yaw)
    end
  end
end
