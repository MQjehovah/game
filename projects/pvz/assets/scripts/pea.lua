-- 豌豆实体 (prefab 动态生成): 向右飞行(僵尸从右进攻), 命中同行僵尸后结算伤害/减速。

local function rowOf(y)
  return math.max(0, math.min(4, math.floor((y - 110 + 50) / 100)))
end

function on_start(e)
  -- 组件已由 prefab 提供, on_start 无需额外初始化
end

function on_update(e, dt)
  if GetVar("started") ~= true then return end
  if GetVar("paused") == true then return end
  local p = EntityComponent(e, "pea")
  if not p then return end
  local pos = GetPosition(e)
  local nx = pos.x + p.speed * dt
  if nx > 1160 then
    Despawn(e)
    return
  end
  SetPosition(e, { x = nx, y = pos.y, z = pos.z })

  local row = rowOf(pos.y)
  local list = GetVar("row_zombies_" .. row)
  if type(list) == "table" then
    for i = 1, #list do
      local z = list[i]
      local ent = { id = z.id, gen = z.gen }
      local zp = GetPosition(ent)
      if zp ~= nil and math.abs(zp.x - nx) < 34 and math.abs(zp.y - pos.y) < 34 then
        local hp = GetHealth(ent)
        if hp ~= nil then
          SetHealth(ent, hp - p.damage)
          SpawnFloatText({ x = zp.x, y = zp.y + 30, z = zp.z },
                         tostring(math.floor(p.damage)), false, 0.5)
          if p.snow == 1 then ApplyStatus(ent, "slow", 3, 0.5) end
        end
        Despawn(e)
        return
      end
    end
  end
end
