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
      rvParts[#rvParts + 1] = { ent = e, lx = p.x, ly = p.y, lz = p.z }
    end
  end
end

function applyRvTransform()
  if rvParts then
    for _, p in ipairs(rvParts) do
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
