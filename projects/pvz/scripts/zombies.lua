-- 僵尸行为 (Lua 后端): 左行 / 索敌吃植物 / 受"缓慢"状态减速 / 到房子游戏结束。

local function rowOf(y)
  return math.max(0, math.min(4, math.floor((y - 110 + 50) / 100)))
end

local function removeFrom(rowVar, id)
  local list = GetVar(rowVar)
  if type(list) ~= "table" then return end
  for i = #list, 1, -1 do
    if list[i].id == id then
      table.remove(list, i)
      break
    end
  end
  SetVar(rowVar, list)
end

function on_start(e)
  local z = EntityComponent(e, "zombie")
  if not z then return end
  local pos = GetPosition(e)
  -- 行号统一取整 (组件 row 是浮点 3.0, rowOf 返回整数 3), 保证键一致。
  local row = math.floor((z.row ~= nil and z.row) or rowOf(pos.y))
  local list = GetVar("row_zombies_" .. row)
  if type(list) ~= "table" then list = {} end
  list[#list + 1] = { id = e.id, gen = e.gen }
  SetVar("row_zombies_" .. row, list)
end

function on_update(e, dt)
  if GetVar("started") ~= true then return end
  if GetVar("paused") == true then return end
  if GetVar("gameover") == true then return end
  local pos = GetPosition(e)
  local row = rowOf(pos.y)

  -- 死亡: 移除并销�?
  local hp = GetHealth(e)
  if hp ~= nil and hp <= 0 then
    removeFrom("row_zombies_" .. row, e.id)
    Despawn(e)
    return
  end

  local z = EntityComponent(e, "zombie")
  local speed = (z and z.speed) or 26
  if HasStatus(e, "slow") then
    local m = StatusMagnitude(e, "slow")
    if m > 0 and m < 1 then speed = speed * m end
  end

  -- 索敌: 同行"已走到"的最右植物 (僵尸左行, 先碰到右边第一个植物就停下吃)。
  local target = nil
  local list = GetVar("row_plants_" .. row)
  if type(list) == "table" then
    local bestX = -1e9
    for i = 1, #list do
      local p = list[i]
      local dx = pos.x - p.x -- 正值 = 僵尸在植物右侧(已到达/越过)
      if dx >= -10 and dx <= 45 and p.x > bestX then
        bestX = p.x
        target = p
      end
    end
  end

  if target then
    -- 吃植物: 修改其 plant 组件 hp (实体无 SceneHealth, 避免豌豆误伤)
    local ent = { id = target.id, gen = target.gen }
    local plant = EntityComponent(ent, "plant")
    local dmg = (z and z.damage) or 12
    if plant and plant.hp ~= nil then
      local newHp = plant.hp - dmg * dt
      if newHp <= 0 then
        removeFrom("row_plants_" .. row, target.id)
        Despawn(ent)
        PlaySfx("eat")
      else
        SetEntityComponent(ent, "plant", {
          type = plant.type, cost = plant.cost, cooldown = plant.cooldown,
          hp = newHp, maxHp = plant.maxHp, row = plant.row, col = plant.col
        })
      end
    end
  else
    local nx = pos.x - speed * dt
    SetPosition(e, { x = nx, y = pos.y, z = pos.z })
    if nx <= 90 then
      -- 该行还有割草机(未消耗)就交给它兜底, 不立即判负。
      local m = GetVar("mowers")
      local protected = type(m) == "table" and m[row + 1] ~= nil
      if not protected then
        SetVar("gameover", true)
        PlaySfx("zombie")
      end
    end
  end
end
