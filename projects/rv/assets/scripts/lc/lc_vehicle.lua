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
      rvParts[#rvParts + 1] = { ent = e, name = n, lx = p.x, ly = p.y, lz = p.z }
    end
  end
end

-- 房车外壳是单面立方体：从车内看到的是背面（被剔除=黑）。
-- 这里补一层朝内的墙板 + 车窗外饰，第一人称才有像样的车舱。
local INTERIOR = {
  -- 内墙板
  { pre = "rv_panel_light", lx = -0.94, ly = 1.13, lz = 0.0,  sx = 0.04, sy = 2.16, sz = 7.86 },
  { pre = "rv_panel_light", lx =  0.94, ly = 1.13, lz = 0.0,  sx = 0.04, sy = 2.16, sz = 7.86 },
  { pre = "rv_panel_light", lx =  0.0,  ly = 1.13, lz = -3.92, sx = 2.24, sy = 2.16, sz = 0.04 },
  -- 驾驶台下半部（上半部留空当挡风玻璃）
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 0.52, lz = 3.94, sx = 2.24, sy = 1.02, sz = 0.06 },
  -- 驾驶台顶沿 + A 柱（驾驶席视角的取景框）
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 1.04, lz = 4.16, sx = 2.24, sy = 0.08, sz = 0.42 },
  { pre = "rv_panel_dark",  lx = -1.06, ly = 1.55, lz = 4.18, sx = 0.10, sy = 1.12, sz = 0.10 },
  { pre = "rv_panel_dark",  lx =  1.06, ly = 1.55, lz = 4.18, sx = 0.10, sy = 1.12, sz = 0.10 },
  -- 地板上的深色地毯
  { pre = "rv_panel_dark",  lx =  0.0,  ly = 0.058, lz = -1.0, sx = 1.45, sy = 0.012, sz = 2.1 },
  -- 外侧车窗（分成两扇 + B 柱），后窗，驾驶室侧窗
  { pre = "rv_panel_window", lx = -1.335, ly = 1.46, lz =  1.35, sx = 0.02, sy = 0.56, sz = 2.0 },
  { pre = "rv_panel_window", lx = -1.335, ly = 1.46, lz = -1.35, sx = 0.02, sy = 0.56, sz = 2.0 },
  { pre = "rv_panel_window", lx =  1.335, ly = 1.46, lz =  1.35, sx = 0.02, sy = 0.56, sz = 2.0 },
  { pre = "rv_panel_window", lx =  1.335, ly = 1.46, lz = -1.35, sx = 0.02, sy = 0.56, sz = 2.0 },
  { pre = "rv_panel_window", lx =  0.0,  ly = 1.52, lz = -4.335, sx = 1.5, sy = 0.5, sz = 0.02 },
  { pre = "rv_panel_window", lx = -1.225, ly = 1.45, lz = 4.85, sx = 0.02, sy = 0.5, sz = 1.0 },
  { pre = "rv_panel_window", lx =  1.225, ly = 1.45, lz = 4.85, sx = 0.02, sy = 0.5, sz = 1.0 },
  -- 车顶内饰板（挡天空，配合暖光做出车舱感）
  { pre = "rv_panel_light", lx = 0.0, ly = 2.12, lz = 0.0, sx = 2.24, sy = 0.04, sz = 7.86 },
  -- 驾驶室顶（保留挡风玻璃上方的开口）
  { pre = "rv_panel_light", lx = 0.0, ly = 2.14, lz = 4.75, sx = 2.24, sy = 0.04, sz = 1.9 },
  { pre = "rv_lamp", lx = 0.0, ly = 2.02, lz = -1.6, sx = 0.16, sy = 0.05, sz = 0.16 },
  { pre = "rv_lamp", lx = 0.0, ly = 2.02, lz = 1.7,  sx = 0.16, sy = 0.05, sz = 0.16 },
  -- 车顶外壳（与墙体同宽，略微出檐）
  { pre = "rv_roof", lx = 0.0, ly = 2.18, lz = 0.2, sx = 2.56, sy = 0.06, sz = 8.1 },
  -- 四个车轮（球体压扁成轮胎，至少让车不再悬浮）
  { pre = "rv_wheel", lx = -1.24, ly = 0.34, lz = -2.6, sx = 0.34, sy = 0.66, sz = 0.66 },
  { pre = "rv_wheel", lx =  1.24, ly = 0.34, lz = -2.6, sx = 0.34, sy = 0.66, sz = 0.66 },
  { pre = "rv_wheel", lx = -1.24, ly = 0.34, lz =  2.6, sx = 0.34, sy = 0.66, sz = 0.66 },
  { pre = "rv_wheel", lx =  1.24, ly = 0.34, lz =  2.6, sx = 0.34, sy = 0.66, sz = 0.66 },
  -- 轮眉（把黑色轮胎和车身衔接起来）
  { pre = "rv_roof", lx = -1.33, ly = 0.62, lz = -2.6, sx = 0.03, sy = 0.22, sz = 1.0 },
  { pre = "rv_roof", lx =  1.33, ly = 0.62, lz = -2.6, sx = 0.03, sy = 0.22, sz = 1.0 },
  { pre = "rv_roof", lx = -1.33, ly = 0.62, lz =  2.6, sx = 0.03, sy = 0.22, sz = 1.0 },
  { pre = "rv_roof", lx =  1.33, ly = 0.62, lz =  2.6, sx = 0.03, sy = 0.22, sz = 1.0 },
  -- 轮毂盖
  { pre = "rv_hub", lx = -1.43, ly = 0.34, lz = -2.6, sx = 0.10, sy = 0.30, sz = 0.30 },
  { pre = "rv_hub", lx =  1.43, ly = 0.34, lz = -2.6, sx = 0.10, sy = 0.30, sz = 0.30 },
  { pre = "rv_hub", lx = -1.43, ly = 0.34, lz =  2.6, sx = 0.10, sy = 0.30, sz = 0.30 },
  { pre = "rv_hub", lx =  1.43, ly = 0.34, lz =  2.6, sx = 0.10, sy = 0.30, sz = 0.30 },
  -- 前后保险杠 / 侧裙
  { pre = "rv_panel_dark", lx = 0.0, ly = 0.55, lz =  5.82, sx = 2.52, sy = 0.30, sz = 0.22 },
  { pre = "rv_panel_dark", lx = 0.0, ly = 0.55, lz = -4.42, sx = 2.52, sy = 0.30, sz = 0.22 },
  { pre = "rv_panel_dark", lx = -1.325, ly = 0.28, lz = 0.0, sx = 0.02, sy = 0.12, sz = 7.8 },
  { pre = "rv_panel_dark", lx =  1.325, ly = 0.28, lz = 0.0, sx = 0.02, sy = 0.12, sz = 7.8 },
  -- 大灯 / 尾灯
  { pre = "rv_lamp", lx = -0.78, ly = 0.92, lz = 5.78, sx = 0.28, sy = 0.13, sz = 0.06 },
  { pre = "rv_lamp", lx =  0.78, ly = 0.92, lz = 5.78, sx = 0.28, sy = 0.13, sz = 0.06 },
  { pre = "rv_light_red", lx = -0.82, ly = 1.08, lz = -4.33, sx = 0.24, sy = 0.14, sz = 0.04 },
  { pre = "rv_light_red", lx =  0.82, ly = 1.08, lz = -4.33, sx = 0.24, sy = 0.14, sz = 0.04 },
  -- 右侧车门（避开长车窗）
  { pre = "rv_panel_window", lx = 1.335, ly = 1.0, lz = -3.2, sx = 0.02, sy = 1.75, sz = 0.78 },
}

function spawnInterior()
  if rvInterior then return end
  rvInterior = {}
  for _, d in ipairs(INTERIOR) do
    local ent = SpawnPrefab(d.pre, { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      SetScale(ent, d.sx, d.sy, d.sz)
      rvInterior[#rvInterior + 1] = {
        ent = ent, lx = d.lx, ly = d.ly, lz = d.lz,
        sx = d.sx, sy = d.sy, sz = d.sz,
      }
    end
  end
end

-- 公路贴花：跟随房车，按里程滚动虚线（Nomad Drive 的公路感）
-- 公路现在是世界固定的，由 spawnRoadNetwork() 一次性生成，不再每帧跟随车头。

function applyRvTransform()
  if rvParts then
    -- 驾驶席第一人称会钻进车头盒体内部（背面被剔除=黑），
    -- 因此驾驶席视角下隐藏车头/挡风玻璃，由内饰 A 柱/仪表台取景框代替。
    local hideCab = (phase == "driving" and driveView == "cab")
    for _, p in ipairs(rvParts) do
      local wx, wz = rotOf(p.lx, p.lz)
      SetPosition(p.ent, { x = wx, y = p.ly, z = wz })
      SetRotationY(p.ent, rv.yaw)
      if p.name == "RV_Cab" or p.name == "RV_Windshield" then
        SetVisible(p.ent, not hideCab)
      end
    end
  end
  if rvInterior then
    for _, p in ipairs(rvInterior) do
      local wx, wz = rotOf(p.lx, p.lz)
      SetPosition(p.ent, { x = wx, y = p.ly, z = wz })
      SetRotationY(p.ent, rv.yaw)
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
