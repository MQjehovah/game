-- 植物共享行为 (Lua 后端)
-- 每个植物实体读取自己的 plant 组件, 按类型执行: 向日葵产阳光 / 射手发射豌豆。
-- 实体自行登记到 row_plants_<row> 注册表, 供僵尸索敌与豌豆命中判定。

local function rowOf(y)
  return math.max(0, math.min(4, math.floor((y - 110 + 50) / 100)))
end

local timers = {}

function on_start(e)
  local p = EntityComponent(e, "plant")
  if not p then return end
  local pos = GetPosition(e)
  -- 种植弹入: 从小放大 (缓出动画)。目标尺寸按原版比例约占格 0.8-0.9。
  SetScale(e, 28, 34, 1)
  Tween(e, 2, { x = 28, y = 34, z = 1 }, { x = 80, y = 92, z = 1 }, 0.22, 2)
  -- 行号统一取整: 组件里的 row 是浮点(3.0), rowOf 返回整数(3), 键必须一致。
  local row = math.floor((p.row ~= nil and p.row) or rowOf(pos.y))
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then list = {} end
  list[#list + 1] = { id = e.id, gen = e.gen, x = pos.x }
  SetVar("row_plants_" .. row, list)
  timers[e.id] = 0
end

function on_update(e, dt)
  if GetVar("started") ~= true then return end
  if GetVar("paused") == true then return end
  local p = EntityComponent(e, "plant")
  if not p then return end

  -- 死亡: 从注册表移除并销毁
  if p.hp ~= nil and p.hp <= 0 then
    local row = math.floor((p.row ~= nil and p.row) or rowOf(GetPosition(e).y))
    local list = GetVar("row_plants_" .. row)
    if type(list) == "table" then
      for i = #list, 1, -1 do
        if list[i].id == e.id then
          table.remove(list, i)
          break
        end
      end
      SetVar("row_plants_" .. row, list)
    end
    timers[e.id] = nil
    timers[e.id .. "_boom"] = nil
    Despawn(e)
    return
  end

  local t = timers[e.id] or 0
  t = t + dt
  timers[e.id] = t
  local pos = GetPosition(e)
  local row = math.floor((p.row ~= nil and p.row) or rowOf(pos.y))

  -- 行内是否有僵尸在射手前方(右侧): 标准 PvZ 射手只在有目标时才开火。
  local function rowHasZombieAhead()
    local zlist = GetVar("row_zombies_" .. row)
    if type(zlist) ~= "table" then return false end
    for i = 1, #zlist do
      local zp = GetPosition({ id = zlist[i].id, gen = zlist[i].gen })
      if zp ~= nil and zp.x > pos.x - 20 then return true end
    end
    return false
  end

  if p.type == "sunflower" then
    if t >= 6 then
      timers[e.id] = 0
      SpawnPrefab("sun", { x = pos.x, y = pos.y, z = 0 })
    end
  elseif p.type == "peashooter" or p.type == "snowpea" then
    if t >= 1.4 and rowHasZombieAhead() then
      timers[e.id] = 0
      SpawnPrefab(p.type == "snowpea" and "snow_pea" or "pea",
                  { x = pos.x + 46, y = pos.y, z = 0 })
      PlaySfx("shoot")
    end
  elseif p.type == "repeater" then
    -- 标准双发: 每 1.4s 一次齐射两颗豌豆(间隔 20px), 仅当行内有僵尸。
    if t >= 1.4 and rowHasZombieAhead() then
      timers[e.id] = 0
      SpawnPrefab("pea", { x = pos.x + 46, y = pos.y, z = 0 })
      SpawnPrefab("pea", { x = pos.x + 66, y = pos.y, z = 0 })
      PlaySfx("shoot")
    end
  elseif p.type == "cherrybomb" then
    -- Fuse then blow up: damage zombies in this row + neighbours.
    local boomKey = e.id .. "_boom"
    if t >= (p.fuse or 1.1) then
      if timers[boomKey] == nil then
        local row = rowOf(pos.y)
        for r = math.max(0, row - 1), math.min(4, row + 1) do
          local zlist = GetVar("row_zombies_" .. r)
          if type(zlist) == "table" then
            for i = #zlist, 1, -1 do
              local zent = { id = zlist[i].id, gen = zlist[i].gen }
              local zp = GetPosition(zent)
              if zp ~= nil and math.abs(zp.x - pos.x) < 110 and math.abs(zp.y - pos.y) < 130 then
                local zhp = GetHealth(zent)
                if zhp ~= nil then SetHealth(zent, zhp - 1800) end
              end
            end
          end
        end
        PlaySfx("explosion")
        -- 爆炸动画: 切 Boom spritesheet (13 帧 @ 14fps ≈ 0.93s), 等比放大后消失。
        SetSpriteSheet(e, "assets/sprites/cherryboom.sheet.png", 13, 14)
        SetScale(e, 200, 184, 1)
        timers[boomKey] = 0
      end
      local bt = timers[boomKey] + dt
      timers[boomKey] = bt
      if bt >= 0.93 then
        timers[e.id] = nil
        timers[boomKey] = nil
        Despawn(e)
      end
      return
    end
  end
end
