-- lc_interact.lua
-- 第一人称交互：靠近物资箱/家具/房车按 E

local function forwardDot(dx, dz, dist)
  if dist < 0.001 then return 0 end
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  return (dx * fx + dz * fz) / dist
end

function nearestCrate()
  local best, bestScore, bestD = nil, -1, 1e9
  for i, c in ipairs(crates) do
    local dx, dz = c.x - player.x, c.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 2.9 then
      local dot = forwardDot(dx, dz, d)
      local score = dot - d * 0.08
      if dot > 0.05 and score > bestScore then
        best, bestScore, bestD = i, score, d
      end
    end
  end
  return best
end

function nearestStation()
  local best, bestScore, bestD = nil, -1, 1e9
  for _, inst in pairs(instances) do
    if inst.layer == "floor" and inst.cat.use ~= nil then
      local wx, wz = rotOf(inst.lx, inst.lz)
      local dx, dz = wx - player.x, wz - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      -- 大件家具按占地放宽一点
      local reach = 2.2 + math.max(inst.cat.w, inst.cat.h) * CELL * 0.25
      if d < reach then
        local dot = forwardDot(dx, dz, d)
        local score = dot - d * 0.10
        if dot > 0.15 and score > bestScore then
          best, bestScore, bestD = inst, score, d
        end
      end
    end
  end
  return best
end

function nearRv()
  local dx, dz = rv.x - player.x, rv.z - player.z
  return dx * dx + dz * dz < 100  -- 约 10m（车长 8.6m，站在车旁即可）
end

function updateInteraction()
  nearAction = nil
  if buildMode or phase ~= "parked" then return end

  local ci = nearestCrate()
  if ci then
    local c = crates[ci]
    nearAction = { kind = "crate", index = ci, verb = "搜刮物资箱",
                   detail = "左键/E 搜索", x = c.x, z = c.z }
    return
  end

  local inst = nearestStation()
  if inst then
    local verb, detail, enabled = stationPrompt(inst)
    if verb then
      nearAction = { kind = "station", inst = inst, verb = verb, detail = detail,
                     enabled = enabled, x = inst.lx, z = inst.lz }
      local wx, wz = rotOf(inst.lx, inst.lz)
      nearAction.x, nearAction.z = wx, wz
      return
    end
  end

  -- 营地货运板：接顺路合同（不占家具格）
  if jobBoard ~= nil then
    local dx, dz = jobBoard.x - player.x, jobBoard.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < JOBS.boardRadius then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        local detail
        if job ~= nil then
          detail = "当前：" .. job.cargo .. " → " .. nodeName(job.destId) .. "（按 0 放弃）"
        elseif jobDelivered > 0 then
          detail = "已完成 " .. jobDelivered .. " 单"
        else
          detail = "顺路载货换材料/燃料"
        end
        nearAction = { kind = "jobboard", verb = "货运板 · 接单",
                       detail = detail, x = jobBoard.x, z = jobBoard.z }
        return
      end
    end
  end

  for _, p in ipairs(ponds) do
    local dx, dz = p.x - player.x, p.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 4.8 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.12 then
        local verb, detail = "钓鱼", "E 抛竿（5-9 秒） / Q 取浑水 / " ..
                              (pondTrapStatusText and pondTrapStatusText() or "Z 放鱼笼")
        if fishingBite then
          verb, detail = "收线！", "E 收线（咬钩窗口）"
        elseif fishingT > 0 then
          verb, detail = "等鱼咬钩", "别走动，咬钩后按 E 收线"
        end
        nearAction = { kind = "fish", verb = verb, detail = detail,
                       x = p.x, z = p.z }
        return
      end
    end
  end

  if campfire then
    local dx, dz = campfire.x - player.x, campfire.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.4 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        local verb, detail = "篝火烤鱼", "E 烤熟生鱼（生鱼x1）"
        if campfireOn == false then
          verb = "篝火已熄灭"
          detail = string.format("G 添柴（木材 %d）", materials.wood or 0)
        elseif (campfireFuelMin or 0) < 120 then
          detail = string.format("E 烤鱼 / 煮水 · 燃料将尽，G 添柴（木材 %d）",
                                 materials.wood or 0)
        end
        nearAction = { kind = "campfire", verb = verb, detail = detail,
                       x = campfire.x, z = campfire.z }
        return
      end
    end
  end

  -- 附近成熟的浆果丛
  for i, b in ipairs(berryBushes or {}) do
    if b.ready then
      local dx, dz = b.x - player.x, b.z - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 2.8 then
        local dot = forwardDot(dx, dz, d)
        if dot > 0.05 then
          nearAction = { kind = "berry", index = i, verb = "采摘野浆果",
                         detail = "E 采集 x2（约 12 秒后再生）",
                         x = b.x, z = b.z }
          return
        end
      end
    end
  end

  -- 路边补给箱
  for i, c in ipairs(roadCaches or {}) do
    if c.ready then
      local dx, dz = c.x - player.x, c.z - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 2.9 then
        local dot = forwardDot(dx, dz, d)
        if dot > 0.05 then
          nearAction = { kind = "cache", index = i, verb = "搜刮补给箱",
                         detail = "E 搜刮（金属/布料，约 180 分钟再生）",
                         x = c.x, z = c.z }
          return
        end
      end
    end
  end

  -- 路边搭车客
  if hitchhiker ~= nil then
    local dx, dz = hitchhiker.x - player.x, hitchhiker.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.4 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "hitchhiker", verb = "路边搭车客",
                       detail = "E 给他 5L 燃料，换回补给",
                       x = hitchhiker.x, z = hitchhiker.z }
        return
      end
    end
  end

  -- 路边抛锚车
  if strandedCar ~= nil and not strandedCar.looted then
    local dx, dz = strandedCar.x - player.x, strandedCar.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.4 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "stranded", verb = "搜刮抛锚车",
                       detail = "E 搜刮（金属/燃油/修理包）",
                       x = strandedCar.x, z = strandedCar.z }
        return
      end
    end
  end

  -- 未读的路牌笔记
  for i, n in ipairs(loreNotes or {}) do
    if not notesFound[n.id] then
      local dx, dz = n.x - player.x, n.z - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 2.4 then
        local dot = forwardDot(dx, dz, d)
        if dot > 0.05 then
          nearAction = { kind = "note", index = i, verb = "阅读笔记",
                         detail = "E 读《" .. (n.title or "笔记") .. "》",
                         x = n.x, z = n.z }
          return
        end
      end
    end
  end

  -- 加油站自动售货机
  if vendingMachine ~= nil then
    local dx, dz = vendingMachine.x - player.x, vendingMachine.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.0 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "vending", verb = "自动售货机",
                       detail = "E 买一份食物/水（1 金属）",
                       x = vendingMachine.x, z = vendingMachine.z }
        return
      end
    end
  end

  -- 营地篝火旁柴堆
  do
    local wx, wz = 0.9, 2.9
    local dx, dz = wx - player.x, wz - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 2.3 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "woodpile", verb = "营地柴堆",
                       detail = (campWoodpileText and campWoodpileText()) or
                                "E 取木材",
                       x = wx, z = wz }
        return
      end
    end
  end

  -- 农场风车水泵
  if windmill ~= nil then
    local dx, dz = windmill.x - player.x, windmill.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.8 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "windmill", verb = "风车水泵",
                       detail = (windmillWaterText and windmillWaterText()) or
                                "E 接水",
                       x = windmill.x, z = windmill.z }
        return
      end
    end
  end

  if campRadio ~= nil then
    local dx, dz = campRadio.x - player.x, campRadio.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 3.0 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        nearAction = { kind = "radio",
                       verb = radioOn and "营地收音机 · 关" or "营地收音机 · 开",
                       detail = "E 循环电台 1/2/3/4 → 关闭（驻车/步行）",
                       x = campRadio.x, z = campRadio.z }
        return
      end
    end
  end

  if radioMast ~= nil then
    local dx, dz = radioMast.x - player.x, radioMast.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 4.5 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.05 then
        if radioMastFixed then
          nearAction = { kind = "mast", verb = "无线电塔 · 已修好",
                         detail = "E 检查信号", x = radioMast.x, z = radioMast.z }
        else
          nearAction = { kind = "mast", verb = "修无线电塔",
                         detail = string.format("E 消耗 金属6 电子件3（现有 %d/%d）",
                           materials.metal or 0, materials.electronics or 0),
                         x = radioMast.x, z = radioMast.z }
        end
        return
      end
    end
  end

  if nearestWaterTower ~= nil then
    local tw, twd = nearestWaterTower(player.x, player.z)
    if tw ~= nil and twd < 4.5 then
      local dx, dz = tw.x - player.x, tw.z - player.z
      if forwardDot(dx, dz, twd) > 0.05 then
        if waterTowerFixed then
          nearAction = { kind = "watertower", verb = "小镇水塔 · 已修好",
                         detail = "E 查看供水（镇上停靠自动补水）",
                         x = tw.x, z = tw.z }
        else
          nearAction = { kind = "watertower", verb = "修水塔",
                         detail = string.format("E 消耗 金属5 木材4（现有 %d/%d）",
                           materials.metal or 0, materials.wood or 0),
                         x = tw.x, z = tw.z }
        end
        return
      end
    end
  end

  -- 废弃邮局邮箱：走近按 E 翻找一次补给
  for _, nd in ipairs(NODES) do
    local mb = nd.mailbox
    if mb ~= nil then
      local dx, dz = mb.x - player.x, mb.z - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 3.2 and forwardDot(dx, dz, d) > 0.05 then
        nearAction = { kind = "mailbox",
                       verb = mb.taken and "邮箱（空）" or "查看邮箱",
                       detail = mb.taken and "已经翻过了" or "E 翻找邮件",
                       x = mb.x, z = mb.z, node = nd }
        return
      end
    end
  end

  -- 加油站油泵：走近按 E 修复，修好后加油永久 7 折
  for _, nd in ipairs(NODES) do
    local fp = nd.fuelPump
    if fp ~= nil then
      local dx, dz = fp.x - player.x, fp.z - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d < 3.2 and forwardDot(dx, dz, d) > 0.05 then
        if fuelPumpFixed then
          nearAction = { kind = "fuelpump", verb = "加油站油泵 · 已修好",
                         detail = "E 查看（加油永久 7 折）",
                         x = fp.x, z = fp.z, node = nd }
        else
          nearAction = { kind = "fuelpump", verb = "修油泵",
                         detail = string.format("E 消耗 金属5 电子件2（现有 %d/%d）",
                           materials.metal or 0, materials.electronics or 0),
                         x = fp.x, z = fp.z, node = nd }
        end
        return
      end
    end
  end

  if nearRv() then
    if hasItem("fuelcan", 1) and (fuel or 0) < fuelMax() - 0.5 then
      nearAction = { kind = "refuel", verb = "加注燃油",
                     detail = string.format("E 使用燃油罐（%.0f / %d L）", fuel, fuelMax()),
                     x = rv.x, z = rv.z }
    else
      local hurt = false
      for _, p in ipairs(REPAIR_PARTS or {}) do
        if partValue(p.id) < 99.5 then hurt = true break end
      end
      local hint = "W 油门 / 空格停车 / B 改装"
      if hurt and hasItem("repairkit", 1) then
        hint = hint .. " / R 现场维修"
      end
      nearAction = { kind = "rv", verb = "驾驶房车", detail = hint,
                     x = rv.x, z = rv.z }
    end
  end
end

function doInteract()
  -- 咬钩窗口内按 E：稳定收线
  if fishingBite then
    fishingBite, fishingBiteT = false, 0
    finishFishing(true)
    return true
  end
  if nearAction == nil then return false end
  if nearAction.kind == "crate" then
    searchCrate(nearAction.index)
    updateInteraction()
    return true
  elseif nearAction.kind == "station" then
    if nearAction.enabled == false then
      toast.text = nearAction.detail or "现在不能用"
      toast.t = 2.2
    elseif nearAction.inst.cat.use == "stove" then
      stationUI = { kind = "stove", inst = nearAction.inst }
    elseif nearAction.inst.cat.use == "workbench" then
      stationUI = { kind = "workbench", inst = nearAction.inst }
    elseif nearAction.inst.cat.use == "bed" and coopRole == nil then
      bedMenuOpen = true
    else
      useStation(nearAction.inst)
    end
    updateInteraction()
    return true
  elseif nearAction.kind == "fish" then
    startFishing()
    return true
  elseif nearAction.kind == "jobboard" then
    jobOpenBoard()
    return true
  elseif nearAction.kind == "campfire" then
    -- 单机开菜单自选料理；联机保持服务端自动烹饪
    if coopRole == nil then
      campfireMenuOpen = true
    else
      cookAtCampfire()
    end
    return true
  elseif nearAction.kind == "berry" then
    gatherBerries(nearAction.index)
    updateInteraction()
    return true
  elseif nearAction.kind == "cache" then
    lootRoadCache(nearAction.index)
    updateInteraction()
    return true
  elseif nearAction.kind == "stranded" then
    lootStrandedCar()
    updateInteraction()
    return true
  elseif nearAction.kind == "hitchhiker" then
    helpHitchhiker()
    updateInteraction()
    return true
  elseif nearAction.kind == "note" then
    readLoreNote(nearAction.index)
    updateInteraction()
    return true
  elseif nearAction.kind == "woodpile" then
    gatherCampWoodpile()
    updateInteraction()
    return true
  elseif nearAction.kind == "vending" then
    buyVendingSnack()
    updateInteraction()
    return true
  elseif nearAction.kind == "windmill" then
    collectWindmillWater()
    updateInteraction()
    return true
  elseif nearAction.kind == "radio" then
    if not radioOn then
      radioOn, radioStation = true, 1
      toast.text = "营地收音机 · 电台 1"
    elseif (radioStation or 1) < 4 then
      radioStation = (radioStation or 1) + 1
      toast.text = "营地收音机 · 切换到电台 " .. radioStation
    else
      radioOn = false
      toast.text = "营地收音机关闭"
    end
    toast.t = 2.4
    if PlaySfx then PlaySfx("click") end
    updateInteraction()
    return true
  elseif nearAction.kind == "mast" then
    if repairRadioMast ~= nil then repairRadioMast() end
    updateInteraction()
    return true
  elseif nearAction.kind == "watertower" then
    if waterTowerFixed then
      toast.text = "水塔已修好——停到镇上会自动补水"
      toast.t = 2.4
    elseif repairWaterTower ~= nil then
      repairWaterTower()
    end
    updateInteraction()
    return true
  elseif nearAction.kind == "mailbox" then
    lootMailbox(nearAction.node)
    updateInteraction()
    return true
  elseif nearAction.kind == "fuelpump" then
    if fuelPumpFixed then
      toast.text = "油泵已修好——加油永久 7 折"
      toast.t = 2.4
    elseif repairFuelPumpAt ~= nil then
      local fixedOk, fixedMsg = repairFuelPumpAt(nearAction.node)
      toast.text = fixedMsg or (fixedOk and "油泵修好了" or "修油泵失败")
      toast.t = 3.0
    end
    updateInteraction()
    return true
  elseif nearAction.kind == "refuel" then
    useFuelCan()
    updateInteraction()
    return true
  elseif nearAction.kind == "rv" then
    if playDoorSfx ~= nil then playDoorSfx() end
    setPhase("driving")
    return true
  end
  return false
end
