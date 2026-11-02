-- lc_vehicle.lua
-- 房车装配：车体部件/模块/贴花随车位姿整体变换

function collectRvParts()
  if rvParts then return end
  rvParts = {}
  local names = { "RV_Floor", "RV_WallL", "RV_WallR", "RV_RearWall", "RV_Cab",
                  "RV_Windshield", "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" }
  for _, n in ipairs(names) do
    local e = FindNamedEntity(n)
    if e ~= nil then
      local p = GetPosition(e)
      if n == "RV_Cab" then SetScale(e, 2.6, 1.8, 1.7) end  -- 与车身同宽
      -- 外壳换成 glTF 模型；旧盒体隐藏，仅保留地板
      if n ~= "RV_Floor" and n ~= "Wheel_FL" and n ~= "Wheel_FR"
         and n ~= "Wheel_RL" and n ~= "Wheel_RR" then
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

function spawnInterior()
  if rvInterior then return end
  rvInterior = {}
  for _, d in ipairs(INTERIOR) do
    local ent = SpawnPrefab(d.pre, { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      SetScale(ent, d.sx, d.sy, d.sz)
      rvInterior[#rvInterior + 1] = {
        ent = ent, lx = d.lx, ly = d.ly, lz = d.lz,
        sx = d.sx, sy = d.sy, sz = d.sz, hideInCab = d.hideInCab,
      }
    end
  end
end

-- 公路贴花：跟随房车，按里程滚动虚线（Nomad Drive 的公路感）
-- 公路现在是世界固定的，由 spawnRoadNetwork() 一次性生成，不再每帧跟随车头。

function applyRvTransform()
  if rvParts then
    for _, p in ipairs(rvParts) do
      local wx, wz = rotOf(p.lx, p.lz)
      SetPosition(p.ent, { x = wx, y = p.ly, z = wz })
      SetRotationY(p.ent, rv.yaw)
    end
  end
  if rvShell then
    SetPosition(rvShell, { x = rv.x, y = 0, z = rv.z })
    SetRotationY(rvShell, rv.yaw)
    -- 驾驶席视角或人在车内时隐藏外壳：避免镜头钻进车窗/墙板
    SetVisible(rvShell, not (phase == "driving" and driveView == "cab")
                        and not isPlayerInsideRv())
  end
  if rvDoor ~= nil then
    local dx, dz = rotOf(1.205, -3.54)
    SetPosition(rvDoor, { x = dx, y = 0, z = dz })
    SetRotationY(rvDoor, rv.yaw + (rvDoorAngle or 0))
  end
  if rvInterior then
    local hideCab = (phase == "driving" and driveView == "cab")
    for _, p in ipairs(rvInterior) do
      local wx, wz = rotOf(p.lx, p.lz)
      SetPosition(p.ent, { x = wx, y = p.ly, z = wz })
      SetRotationY(p.ent, rv.yaw)
      if p.hideInCab then SetVisible(p.ent, not hideCab) end
    end
  end
  for _, inst in pairs(instances) do
    local wx, wz = rotOf(inst.lx, inst.lz)
    SetPosition(inst.ent, { x = wx, y = inst.ly, z = wz })
    SetRotationY(inst.ent, rv.yaw + (inst.lrot or 0))
  end
  if gridFloorEnt ~= nil then
    SetPosition(gridFloorEnt, { x = rv.x, y = FLOOR_Y, z = rv.z })
    SetRotationY(gridFloorEnt, rv.yaw)
  end
  if gridRoofEnt ~= nil then
    SetPosition(gridRoofEnt, { x = rv.x, y = ROOF_Y, z = rv.z })
    SetRotationY(gridRoofEnt, rv.yaw)
  end
end
