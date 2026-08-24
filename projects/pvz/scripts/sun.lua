-- 阳光实体 (prefab 动态生成): 缓慢下落, 点击收集, 超时消失。

local timers = {}

function on_start(e)
  local s = EntityComponent(e, "sun")
  local pos = GetPosition(e)
  timers[e.id] = { t = 0, targetY = pos.y + 60, value = (s and s.value) or 25 }
end

function on_update(e, dt)
  if GetVar("started") ~= true then return end
  local st = timers[e.id]
  if not st then return end
  local pos = GetPosition(e)
  st.t = st.t + dt

  if pos.y < st.targetY then
    pos.y = math.min(st.targetY, pos.y + 30 * dt)
    SetPosition(e, { x = pos.x, y = pos.y, z = pos.z })
  end
  if st.t > 10 then
    timers[e.id] = nil
    Despawn(e)
    return
  end

  if InputMousePressed(0) then
    local m = InputMousePos()
    if m ~= nil and math.abs(m.x - pos.x) < 32 and math.abs(m.y - pos.y) < 32 then
      local s = GetVar("sun")
      if type(s) ~= "number" then s = 0 end
      SetVar("sun", s + st.value)
      timers[e.id] = nil
      PlaySfx("sun")
      Despawn(e)
    end
  end
end
